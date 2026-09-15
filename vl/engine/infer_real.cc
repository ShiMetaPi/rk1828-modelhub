// infer_real.cc — Qwen2.5-VL-3B 真实推理后端。
//
// 链路：JPEG 解码(libjpeg, RGB888) → expand2square 灰底127.5 letterbox →
// 双线性缩放 392×392 → CLIP mean/std 归一化 → temporal 复制×2 → HF
// pixel_values 排布（merge 窗口序，[784, 1176] fp16）→ vision.rknn(普通
// rknn3 API) → image_embed [196, 2048] fp16 → rknn3_session MULTIMODAL 输入
// （prompt 原文，runtime 自动套 chatml 模板并插入视觉 token）。
//
// 排布依据（照抄 HF Qwen2VL processor / 官方 export_vision_qwen2.py）：
//   reshape (t, T, C, gh2, mh, ph, gw2, mw, pw) → permute(0,3,6,4,7,2,1,5,8)
//   行序 = (t, gh2, gw2, mh, mw)，列序 = (C, T, ph, pw)；T 维两帧同图。
#include "infer_real.h"

#include <float16.h>
#include <rknn3_api.h>
#include <Tokenizer.h>

#include <jpeglib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <csetjmp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vl {
namespace {

// ---- 模型常量（vision.rknn 392×392 固定输入）----
const int kSide = 392;          // 模型输入边长
const int kPatch = 14;          // patch 像素
const int kMerge = 2;           // 2×2 merge 窗口
const int kGrid = kSide / kPatch;          // 28
const int kGrid2 = kGrid / kMerge;         // 14
const int kNPatch = kGrid * kGrid;         // 784
const int kPatchDim = 3 * 2 * kPatch * kPatch;  // 1176
const int kImgTokens = kNPatch / (kMerge * kMerge);  // 196

// CLIP 归一化（官方 export_vision_rknn.py qwen2 分支，×255 作用于 0..255 像素）
const float kMean[3] = {0.48145466f * 255, 0.4578275f * 255, 0.40821073f * 255};
const float kStd[3]  = {0.26862954f * 255, 0.26130258f * 255, 0.27577711f * 255};

const int kMaxCtx = 1024;       // LLM 上下文（模型 kvcache 组只有 1024，见
                                // 启动警告 "kvcache_buffer_lens[0]: 1024"）
const int kMaxNew = 128;        // 每次回答上限（视频问答要快；调小可多留上下文）
// 摘要续命：KV 满自动清空时，把最近 kRecapTurns 轮问答的文字摘要拼进下一条
// prompt——模型跨清空仍记得聊过什么（图像 token 太贵不进摘要，只存文字）
const int kRecapTurns = 3;
const size_t kRecapQBytes = 60, kRecapABytes = 120;

double NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

// UTF-8 安全截断（不劈开多字节字符）
std::string TruncUtf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t end = max_bytes;
    while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80) --end;
    return s.substr(0, end) + "…";
}

// ---- libjpeg 解码（标准 setjmp 错误处理样板）----
struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf setjmp_buf;
};
void JpegErrorExit(j_common_ptr cinfo) {
    JpegErrorMgr* e = (JpegErrorMgr*)cinfo->err;
    longjmp(e->setjmp_buf, 1);
}

bool DecodeJpeg(const uint8_t* data, size_t len, int* w, int* h,
                std::vector<uint8_t>* rgb) {
    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;
    if (setjmp(jerr.setjmp_buf)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<uint8_t*>(data), (unsigned long)len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;  // JPEG 内部 YCbCr→RGB 由 libjpeg 做
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    if (cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    rgb->resize((size_t)(*w) * (*h) * 3);
    JSAMPROW row = nullptr;
    while (cinfo.output_scanline < cinfo.output_height) {
        row = rgb->data() + (size_t)cinfo.output_scanline * (*w) * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

}  // namespace

// 每进程一个实例（回调 userdata 走 this），内部持全部句柄。
class RealInfer::Impl {
public:
    ~Impl() { Cleanup(); }

    bool Init(const std::string& dir, std::string* err);
    bool Run(const AskRequest& req,
             const std::function<void(const std::string&)>& on_token,
             InferStats* stats, std::string* err,
             const std::function<void(const std::string&)>& on_notice);
    void Reset();

    // 回调目标（Run 期间有效）
    const std::function<void(const std::string&)>* sink = nullptr;
    struct timeval t_start, t_first;
    bool flip180 = false;   // 输入 180° 翻转（倒置摄像头用；Init 后由 RealInfer 传入）
    bool got_first = false;
    bool run_error = false;

    Tokenizer* tokenizer = nullptr;

private:
    static int ResultCb(void* userdata, RKLLMResult* result, LLMCallState state);
    static int TokenizerCb(void* userdata, const char* text, int32_t text_len,
                           int32_t* tokens, int32_t n_tokens_max);
    static int EmbedCb(void* userdata, int32_t* tokens, uint64_t num_tokens,
                       void* embed, uint64_t len);

    bool InitVision(const std::string& dir, std::string* err);
    bool InitLlm(const std::string& dir, std::string* err);
    bool Preprocess(const uint8_t* jpeg, size_t len, std::string* err);
    void Cleanup();

    // vision 模型（普通 rknn3 API）
    rknn3_context vctx_ = 0;
    rknn3_tensor_attr vin_attr_ = {};
    rknn3_tensor_attr vout_attr_ = {};
    rknn3_tensor_mem* vin_mem_ = nullptr;
    rknn3_tensor_mem* vout_mem_ = nullptr;
    rknn3_tensor vin_t_ = {};
    rknn3_tensor vout_t_ = {};
    int n_img_tokens_ = 0;    // 从输出 attr 读出（应为 196）
    int embed_dim_ = 0;       // 应为 2048

    // LLM session
    rknn3_context lctx_ = 0;
    rknn3_session* sess_ = nullptr;

    // embed 表 mmap
    int emb_fd_ = -1;
    float16* emb_data_ = nullptr;
    size_t emb_size_ = 0;
    int emb_dim_ = 0;
    struct stat emb_st_ = {};

    std::vector<float> norm_;  // 预处理中间图 [3][392][392]

    // 视觉标签 token id（Init 时从 tokenizer 解析）+ 多模态输入 token 缓冲
    int vs_id_ = 0, ve_id_ = 0, pad_id_ = 0;
    std::vector<int32_t> mm_tokens_;
    bool ResolveTag(const char* tag, const char* name, int* id, std::string* err);

    // 对话摘要续命：turns_ 存最近几轮问答文字；KV 清空的那轮把摘要拼进 prompt
    std::vector<std::pair<std::string, std::string>> turns_;
    std::string cur_answer_;    // 本轮累计的回答文字（ResultCb 里攒）
    std::string BuildRecap() const;
};

bool RealInfer::Impl::InitVision(const std::string& dir, std::string* err) {
    std::string rknn = dir + "/Qwen2.5-VL-3B-vision.rknn";
    std::string weight = dir + "/Qwen2.5-VL-3B-vision.weight";
    int ret = rknn3_init(&vctx_, nullptr);
    if (ret) { *err = "rknn3_init(vision) ret=" + std::to_string(ret); return false; }
    ret = rknn3_load_model_from_path(vctx_, rknn.c_str(), weight.c_str());
    if (ret) { *err = "vision load_model ret=" + std::to_string(ret); return false; }
    rknn3_config cfg = {};
    cfg.run_core_mask = 0xff;
    ret = rknn3_model_init(vctx_, &cfg);
    if (ret) { *err = "vision model_init ret=" + std::to_string(ret); return false; }

    rknn3_input_output_num io = {};
    ret = rknn3_query(vctx_, RKNN3_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (ret || io.n_input != 1 || io.n_output != 1) {
        *err = "vision io query ret=" + std::to_string(ret);
        return false;
    }
    vin_attr_.index = 0;
    ret = rknn3_query(vctx_, RKNN3_QUERY_INPUT_ATTR, &vin_attr_, sizeof(vin_attr_));
    vout_attr_.index = 0;
    ret |= rknn3_query(vctx_, RKNN3_QUERY_OUTPUT_ATTR, &vout_attr_, sizeof(vout_attr_));
    if (ret) { *err = "vision attr query ret=" + std::to_string(ret); return false; }
    if (vin_attr_.n_elems != (uint32_t)(kNPatch * kPatchDim) ||
        vin_attr_.dtype != RKNN3_TENSOR_FLOAT16) {
        char buf[256];
        snprintf(buf, sizeof(buf), "vision 输入不是 [%d,%d] fp16（elems=%u dtype=%d）",
                 kNPatch, kPatchDim, vin_attr_.n_elems, (int)vin_attr_.dtype);
        *err = buf;
        return false;
    }
    if (vout_attr_.n_dims == 2 && vout_attr_.shape[1] > 1) {
        n_img_tokens_ = vout_attr_.shape[0];
        embed_dim_ = vout_attr_.shape[1];
    }
    if (n_img_tokens_ != kImgTokens) {
        fprintf(stderr, "[real] 警告: vision 输出 token 数 %d != 预期 %d\n",
                n_img_tokens_, kImgTokens);
        n_img_tokens_ = kImgTokens;
    }

    vin_mem_ = rknn3_create_mem(vctx_, vin_attr_.aligned_size, vin_attr_.core_id,
                                RKNN3_FLAG_MEMORY_CACHEABLE);
    vout_mem_ = rknn3_create_mem(vctx_, vout_attr_.aligned_size, vout_attr_.core_id,
                                 RKNN3_FLAG_MEMORY_CACHEABLE);
    if (!vin_mem_ || !vout_mem_) { *err = "vision create_mem 失败"; return false; }
    vin_t_ = {vin_mem_, &vin_attr_};
    vout_t_ = {vout_mem_, &vout_attr_};
    return true;
}

bool RealInfer::Impl::InitLlm(const std::string& dir, std::string* err) {
    std::string rknn = dir + "/Qwen2.5-VL-3B-llm.rknn";
    std::string weight = dir + "/Qwen2.5-VL-3B-llm.weight";
    int ret = rknn3_init(&lctx_, nullptr);
    if (ret) { *err = "rknn3_init(llm) ret=" + std::to_string(ret); return false; }
    ret = rknn3_load_model_from_path(lctx_, rknn.c_str(), weight.c_str());
    if (ret) { *err = "llm load_model ret=" + std::to_string(ret); return false; }
    rknn3_config cfg = {};
    cfg.run_core_mask = 0xff;
    ret = rknn3_model_init(lctx_, &cfg);
    if (ret) { *err = "llm model_init ret=" + std::to_string(ret); return false; }

    // session 参数：贪心（top_k=1）+ 官方 demo 同款惩罚参数
    VocabInfo vi = {};
    tokenizer->GetVocabInfo(&vi);
    rknn3_llm_param param = {};
    param.logits_name = (char*)"output";
    param.max_context_len = kMaxCtx;
    param.sampling_param.temperature = 1.0f;
    param.sampling_param.top_k = 1;
    param.sampling_param.top_p = 0.9f;
    param.sampling_param.repeat_penalty = 1.1f;
    param.vocab_info.vocab_size = vi.vocab_size;
    param.vocab_info.n_special_bos_id = vi.n_special_bos_id;
    param.vocab_info.n_special_eos_id = vi.n_special_eos_id;
    memcpy(param.vocab_info.special_bos_id, vi.special_bos_id, sizeof(vi.special_bos_id));
    memcpy(param.vocab_info.special_eos_id, vi.special_eos_id, sizeof(vi.special_eos_id));
    param.vocab_info.linefeed_id = vi.linefeed_id;
    param.vocab_info.skip_special_token = true;
    sess_ = rknn3_session_init(lctx_, &param, 1);
    if (!sess_) { *err = "session_init 失败"; return false; }

    RKLLMCallback cb = {};
    cb.result_callback = ResultCb;
    cb.result_userdata = this;
    cb.tokenizer_callback = TokenizerCb;
    cb.tokenizer_userdata = tokenizer;
    cb.embed_callback = EmbedCb;
    cb.embed_userdata = this;
    ret = rknn3_session_set_callback(sess_, &cb);
    if (ret) { *err = "set_callback ret=" + std::to_string(ret); return false; }
    return true;
}

bool RealInfer::Impl::ResolveTag(const char* tag, const char* name, int* id, std::string* err) {
    int32_t t[8] = {};
    int n = tokenizer->Tokenize(tag, (int32_t)strlen(tag), t, 8);
    if (n != 1) {
        *err = std::string("标签 ") + name + " 分词异常(n=" + std::to_string(n) + ")，无法作为单一特殊 token";
        return false;
    }
    *id = t[0];
    fprintf(stderr, "[real] %s token id = %d\n", name, t[0]);
    return true;
}

bool RealInfer::Impl::Init(const std::string& dir, std::string* err) {
    std::string mdir = dir + "/model";
    tokenizer = new Tokenizer(TOKENIZER_BACKEND_LLAMA,
                              (mdir + "/Qwen2.5-VL-3B-llm.tokenizer.gguf").c_str());
    if (!ResolveTag("<|vision_start|>", "<|vision_start|>", &vs_id_, err) ||
        !ResolveTag("<|vision_end|>", "<|vision_end|>", &ve_id_, err) ||
        !ResolveTag("<|image_pad|>", "<|image_pad|>", &pad_id_, err)) {
        return false;
    }

    // embed 表 mmap（embed_callback 逐 token 查表）
    std::string emb_path = mdir + "/Qwen2.5-VL-3B-llm.embed.bin";
    emb_fd_ = open(emb_path.c_str(), O_RDONLY);
    if (emb_fd_ < 0) { *err = "打不开 " + emb_path; return false; }
    if (fstat(emb_fd_, &emb_st_) != 0) { *err = "fstat embed 失败"; return false; }
    emb_data_ = (float16*)mmap(nullptr, emb_st_.st_size, PROT_READ, MAP_PRIVATE, emb_fd_, 0);
    if (emb_data_ == MAP_FAILED) { emb_data_ = nullptr; *err = "mmap embed 失败"; return false; }
    emb_size_ = emb_st_.st_size;
    VocabInfo vi = {};
    tokenizer->GetVocabInfo(&vi);
    emb_dim_ = (int)((emb_size_ / vi.vocab_size) / sizeof(float16));
    fprintf(stderr, "[real] tokenizer vocab=%d embed_dim=%d\n", vi.vocab_size, emb_dim_);
    if (emb_dim_ != 2048) {
        *err = "embed 维度异常: " + std::to_string(emb_dim_);
        return false;
    }

    double t0 = NowMs();
    if (!InitVision(mdir, err)) return false;
    fprintf(stderr, "[real] vision 模型就绪 %.0fms\n", NowMs() - t0);
    t0 = NowMs();
    if (!InitLlm(mdir, err)) return false;
    fprintf(stderr, "[real] llm session 就绪 %.0fms\n", NowMs() - t0);
    norm_.resize((size_t)3 * kSide * kSide);
    return true;
}

// JPEG → letterbox 归一化中间图 → [784,1176] fp16 patch 张量
bool RealInfer::Impl::Preprocess(const uint8_t* jpeg, size_t len, std::string* err) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!DecodeJpeg(jpeg, len, &w, &h, &rgb)) {
        *err = "JPEG 解码失败";
        return false;
    }

    // expand2square：以 max(w,h) 为边，图像居中，pad 127.5（归一化前灰底，
    // 与官方 demo 一致）；摄像头画面物理倒置 → 采样时源坐标 180° 翻转。
    const int side = w > h ? w : h;
    const float scale = (float)kSide / side;
    const float ox = (side - w) * 0.5f;   // 方形坐标→原图坐标偏移
    const float oy = (side - h) * 0.5f;
    const float pad = 127.5f;

    // 归一化中间图 norm_[c][y][x]
    for (int ty = 0; ty < kSide; ty++) {
        const float sy = (ty + 0.5f) / scale - 0.5f - oy;
        for (int tx = 0; tx < kSide; tx++) {
            const float sx = (tx + 0.5f) / scale - 0.5f - ox;
            if (sx < -0.5f || sy < -0.5f || sx > w - 0.5f || sy > h - 0.5f) {
                for (int c = 0; c < 3; c++)  // letterbox 灰底
                    norm_[((size_t)c * kSide + ty) * kSide + tx] =
                        (pad - kMean[c]) / kStd[c];
                continue;
            }
            // 源坐标 180° 翻转（可选：倒置摄像头专用；CLI 正立图片必须关）
            const float fx = flip180 ? w - 1 - sx : sx;
            const float fy = flip180 ? h - 1 - sy : sy;
            const int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            const int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            const int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
            const float dx = fx - x0, dy = fy - y0;
            const size_t p00 = ((size_t)y0 * w + x0) * 3;
            const size_t p10 = ((size_t)y0 * w + x1) * 3;
            const size_t p01 = ((size_t)y1 * w + x0) * 3;
            const size_t p11 = ((size_t)y1 * w + x1) * 3;
            for (int c = 0; c < 3; c++) {
                const float v = rgb[p00 + c] * (1 - dx) * (1 - dy) +
                                rgb[p10 + c] * dx * (1 - dy) +
                                rgb[p01 + c] * (1 - dx) * dy +
                                rgb[p11 + c] * dx * dy;
                norm_[((size_t)c * kSide + ty) * kSide + tx] = (v - kMean[c]) / kStd[c];
            }
        }
    }

    // patch 化：行序 (gh2, gw2, mh, mw)，列序 (C, T, ph, pw)（T 两帧同图）
    float16* out = (float16*)vin_mem_->virt_addr;
    int row = 0;
    for (int gh2 = 0; gh2 < kGrid2; gh2++) {
        for (int gw2 = 0; gw2 < kGrid2; gw2++) {
            for (int mh = 0; mh < kMerge; mh++) {
                for (int mw = 0; mw < kMerge; mw++) {
                    const int py0 = (gh2 * kMerge + mh) * kPatch;
                    const int px0 = (gw2 * kMerge + mw) * kPatch;
                    float16* dst = out + (size_t)row * kPatchDim;
                    int col = 0;
                    for (int c = 0; c < 3; c++) {
                        for (int t = 0; t < 2; t++) {  // temporal 副本
                            for (int py = 0; py < kPatch; py++) {
                                for (int px = 0; px < kPatch; px++) {
                                    dst[col++] = fp32_to_fp16(
                                        norm_[((size_t)c * kSide + py0 + py) * kSide + px0 + px]);
                                }
                            }
                        }
                    }
                    row++;
                }
            }
        }
    }
    return true;
}

bool RealInfer::Impl::Run(const AskRequest& req,
                          const std::function<void(const std::string&)>& on_token,
                          InferStats* stats, std::string* err,
                          const std::function<void(const std::string&)>& on_notice) {
    // 上下文压力检查：塞不下"196图+问+答"就清 KV 重开，并把最近对话以文字
    // 摘要拼进本轮 prompt（续命）；同时发系统提示给 Web 聊天框
    bool recap = false;
    RKLLMRunState st = {};
    if (rknn3_session_query_state(sess_, &st) == 0 &&
        (int)st.n_total_tokens + kImgTokens + 96 + kMaxNew > kMaxCtx) {
        fprintf(stderr, "[real] 上下文将满(%llu/%d)，自动清 KV\n",
                (unsigned long long)st.n_total_tokens, kMaxCtx);
        rknn3_session_clear_kvcache(sess_, RKNN3_KVCACHE_CLEAR_ALL);
        recap = !turns_.empty();
        if (on_notice) on_notice("上下文已满，自动清空记忆（最近几轮对话已带摘要给模型）");
    }
    cur_answer_.clear();

    bool have_image = !req.jpeg.empty();
    if (have_image) {
        double t0 = NowMs();
        if (!Preprocess(req.jpeg.data(), req.jpeg.size(), err)) return false;
        int ret = rknn3_mem_sync(vctx_, vin_mem_, RKNN3_MEMORY_SYNC_TO_DEVICE);
        if (ret) { *err = "vision mem_sync ret=" + std::to_string(ret); return false; }
        ret = rknn3_run(vctx_, &vin_t_, 1, &vout_t_, 1);
        if (ret) { *err = "vision run ret=" + std::to_string(ret); return false; }
        // CACHEABLE 输出内存：device 写完后 CPU 读前必须 FROM_DEVICE 同步，
        // 否则读到的是旧数据/噪声 → 视觉 token 失效 → 模型幻觉（踩坑实测）
        ret = rknn3_mem_sync(vctx_, vout_mem_, RKNN3_MEMORY_SYNC_FROM_DEVICE);
        if (ret) { *err = "vision 输出 mem_sync ret=" + std::to_string(ret); return false; }
        stats->vit_ms = NowMs() - t0;
    }

    // LLM 多模态输入（vision 输出 fp16 [196,2048] 直接作为 image_embed）
    rknn3_llm_input in = {};
    rknn3_llm_infer_param ip = {};
    ip.keep_history = 1;
    ip.max_new_tokens = kMaxNew;
    if (have_image) {
        // 手工组装 chatml token 序列（runtime 在 token 流中定位视觉标签并
        // 把 image_pad 段替换为 image_embed，plain prompt 找不到标签会报错）：
        // <|im_start|>user\n <|vision_start|> pad×196 <|vision_end|> 问句
        // <|im_end|>\n <|im_start|>assistant\n
        auto append_piece = [&](const char* s) -> bool {
            int32_t buf[1024];  // 摘要/长问题可能几百 token，别用小缓冲
            int n = tokenizer->Tokenize(s, (int32_t)strlen(s), buf, 1024);
            if (n <= 0 || n > 1024) return false;
            for (int i = 0; i < n; i++) mm_tokens_.push_back(buf[i]);
            return true;
        };
        mm_tokens_.clear();
        if (!append_piece("<|im_start|>") || !append_piece("user") || !append_piece("\n")) {
            *err = "模板前缀分词失败";
            return false;
        }
        mm_tokens_.push_back(vs_id_);
        for (int i = 0; i < n_img_tokens_; i++) mm_tokens_.push_back(pad_id_);
        mm_tokens_.push_back(ve_id_);
        if (recap && !append_piece(BuildRecap().c_str())) {
            *err = "对话摘要分词失败";
            return false;
        }
        if (!append_piece(req.text.c_str()) || !append_piece("<|im_end|>") ||
            !append_piece("\n") || !append_piece("<|im_start|>") ||
            !append_piece("assistant") || !append_piece("\n")) {
            *err = "模板后缀分词失败";
            return false;
        }
        in.role = "user";
        in.input_type = RKNN3_LLM_INPUT_MULTIMODAL;
        rknn3_llm_multimodal_tensor& m = in.multimodal_input;
        m.tokens = mm_tokens_.data();
        m.n_tokens = mm_tokens_.size();
        m.enable_thinking = false;
        m.image.image_embed = (float16*)vout_mem_->virt_addr;
        m.image.n_image_tokens = n_img_tokens_;
        m.image.n_image = 1;
        m.image.image_start = "<|vision_start|>";
        m.image.image_end = "<|vision_end|>";
        m.image.image_content = "<|image_pad|>";
        m.image.image_width = kSide;
        m.image.image_height = kSide;
    } else {
        in.role = "user";
        in.input_type = RKNN3_LLM_INPUT_PROMPT;
        in.llm_input.prompt = req.text.c_str();
        in.llm_input.enable_thinking = false;
    }

    sink = &on_token;
    got_first = false;
    run_error = false;
    gettimeofday(&t_start, nullptr);
    double t_run0 = NowMs();
    int ret = rknn3_session_run(sess_, &in, 1, &ip);
    double t_end = NowMs();
    sink = nullptr;
    if (ret) { *err = "session_run ret=" + std::to_string(ret); return false; }
    if (run_error) { *err = "推理回调报错"; return false; }

    if (rknn3_session_query_state(sess_, &st) == 0) stats->tokens = (int)st.n_decode_tokens;
    // 记录本轮问答文字（供 KV 清空后的摘要续命；图像 token 太贵不存）
    turns_.push_back({TruncUtf8(req.text, kRecapQBytes),
                      TruncUtf8(cur_answer_, kRecapABytes)});
    while (turns_.size() > (size_t)kRecapTurns) turns_.erase(turns_.begin());
    double t_first_ms = got_first
        ? t_first.tv_sec * 1000.0 + t_first.tv_usec / 1000.0 : t_end;
    stats->prefill_ms = t_first_ms - t_run0;
    stats->decode_ms = t_end - t_first_ms;
    return true;
}

void RealInfer::Impl::Reset() {
    if (sess_) rknn3_session_clear_kvcache(sess_, RKNN3_KVCACHE_CLEAR_ALL);
    turns_.clear();  // 手动重置 = 干净开始，摘要也一并丢弃
}

std::string RealInfer::Impl::BuildRecap() const {
    std::string r = "［此前对话文字摘要，图像内容不在此列］";
    for (size_t i = 0; i < turns_.size(); i++)
        r += "问:" + turns_[i].first + " 答:" + turns_[i].second + "。";
    return r;
}

void RealInfer::Impl::Cleanup() {
    if (sess_) { rknn3_session_destroy(sess_); sess_ = nullptr; }
    if (lctx_) { rknn3_destroy(lctx_); lctx_ = 0; }
    if (vin_mem_) { rknn3_destroy_mem(vctx_, vin_mem_); vin_mem_ = nullptr; }
    if (vout_mem_) { rknn3_destroy_mem(vctx_, vout_mem_); vout_mem_ = nullptr; }
    if (vctx_) { rknn3_destroy(vctx_); vctx_ = 0; }
    if (emb_data_) { munmap(emb_data_, emb_size_); emb_data_ = nullptr; }
    if (emb_fd_ >= 0) { close(emb_fd_); emb_fd_ = -1; }
    delete tokenizer;
    tokenizer = nullptr;
}

// ---- 回调（userdata 约定见 InitLlm）----

int RealInfer::Impl::ResultCb(void* userdata, RKLLMResult* result, LLMCallState state) {
    Impl* self = (Impl*)userdata;
    if (state == RKLLM_RUN_ERROR) { self->run_error = true; return 0; }
    if (state != RKLLM_RUN_NORMAL || !self->sink) return 0;
    std::string piece = result->num_tokens == 1
        ? self->tokenizer->TokenToPiece(result->token_ids[0])
        : self->tokenizer->Decode(result->token_ids, result->num_tokens);
    (*self->sink)(piece);
    self->cur_answer_ += piece;  // 攒本轮回答文字，进 turns_ 供摘要续命
    if (!self->got_first) {
        gettimeofday(&self->t_first, nullptr);
        self->got_first = true;
    }
    return 0;
}

int RealInfer::Impl::TokenizerCb(void* userdata, const char* text, int32_t text_len,
                                 int32_t* tokens, int32_t n_tokens_max) {
    Tokenizer* tok = (Tokenizer*)userdata;
    return tok->Tokenize(text, text_len, tokens, n_tokens_max);
}

int RealInfer::Impl::EmbedCb(void* userdata, int32_t* tokens, uint64_t num_tokens,
                             void* embed, uint64_t len) {
    Impl* self = (Impl*)userdata;
    if (len != num_tokens * (uint64_t)self->emb_dim_ * sizeof(float16)) return -1;
    for (uint64_t n = 0; n < num_tokens; n++) {
        memcpy((uint8_t*)embed + n * self->emb_dim_ * sizeof(float16),
               self->emb_data_ + (size_t)tokens[n] * self->emb_dim_,
               self->emb_dim_ * sizeof(float16));
    }
    return 0;
}

// ---- InferEngine 转发 ----

RealInfer::~RealInfer() { delete impl_; }

bool RealInfer::Init(std::string* err) {
    impl_ = new Impl();
    if (!impl_->Init(model_dir_, err)) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    impl_->flip180 = flip180_;
    return true;
}

bool RealInfer::Run(const AskRequest& req,
                    const std::function<void(const std::string&)>& on_token,
                    InferStats* stats, std::string* err,
                    const std::function<void(const std::string&)>& on_notice) {
    return impl_ ? impl_->Run(req, on_token, stats, err, on_notice) : false;
}

void RealInfer::Reset() { if (impl_) impl_->Reset(); }

}  // namespace vl
