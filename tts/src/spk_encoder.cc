// spk_encoder.cc — 板端说话人编码器（Qwen3-TTS 声音克隆）
//
// rknn 加载/推理沿用 text_projection 的普通模型模式（rknn3_run，core 0x01）；
// 预处理（STFT + mel）在本文件实现，滤波器组从 spk_mel_128x513.f32 加载，
// 避免在 C++ 里复刻 librosa 的 Slaney mel 公式。
#include "spk_encoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "float16.h"
#include "rknn3_api.h"

namespace {

const int kSampleRate = 24000;
const int kAudioLen = 240000;                    // 10 秒（板端固定输入）
const int kNfft = 1024;
const int kHop = 256;
const int kSpecBins = kNfft / 2 + 1;             // 513
const int kMelBins = 128;
const int kPad = (kNfft - kHop) / 2;             // 384（reflect 两侧）
const int kFrames = 1 + (kAudioLen + 2 * kPad - kNfft) / kHop;  // 937
const int kEmbedDim = 2048;

const int kFftStages = 10;                       // log2(1024)

#ifndef LOGE
#define LOGE(fmt, ...) fprintf(stderr, "[SpkEncoder][E] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOGI
#define LOGI(fmt, ...) printf("[SpkEncoder][I] " fmt "\n", ##__VA_ARGS__)
#endif

}  // namespace

// FFT 状态（位反转表 + 旋转因子）。独立小结构：文件级 fft_init/fft_run
// 无法引用私有的 SpkEncoder::Impl，所以把 FFT 状态单独拎出来。
struct FFTPlan {
    std::vector<int> bitrev;
    std::vector<float> tw_re, tw_im;
};

struct SpkEncoder::Impl {
    rknn3_context ctx = 0;
    rknn3_tensor input;
    rknn3_tensor output;
    size_t in_elems = 0;
    size_t out_elems = 0;
    bool in_fp16 = true;   // 由 attr 的 aligned_size/n_elems 推断（*2=fp16，*4=fp32）
    bool out_fp16 = true;

    std::vector<float> mel_fb;      // 128 * 513 行优先
    std::vector<float> hann;        // 周期 Hann（与 torch.hann_window 默认一致）
    FFTPlan fft;                    // 位反转表 + 旋转因子

    std::vector<float> work;        // 定长波形（含 reflect pad）
    std::vector<float> fft_re, fft_im;
    std::vector<float> mag;         // 513
    std::vector<float> mel_out;     // 937 * 128，模型输入

    bool ready = false;
};

SpkEncoder::SpkEncoder() : impl_(new Impl) {}
SpkEncoder::~SpkEncoder() { delete impl_; }

bool SpkEncoder::Ready() const { return impl_->ready; }

// ── FFT（基-2 迭代，N=1024 固定）───────────────────────────────────────
static void fft_init(FFTPlan* p) {
    const int N = kNfft;
    p->bitrev.resize(N);
    for (int i = 0; i < N; ++i) {
        int r = 0;
        for (int b = 0; b < kFftStages; ++b) {
            r = (r << 1) | ((i >> b) & 1);
        }
        p->bitrev[i] = r;
    }
    p->tw_re.resize(N / 2);
    p->tw_im.resize(N / 2);
    for (int k = 0; k < N / 2; ++k) {
        double ang = -2.0 * M_PI * k / N;
        p->tw_re[k] = (float)cos(ang);
        p->tw_im[k] = (float)sin(ang);
    }
}

static void fft_run(const FFTPlan* p, float* re, float* im_out) {
    const int N = kNfft;
    for (int i = 0; i < N; ++i) {
        int j = p->bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im_out[i]; im_out[i] = im_out[j]; im_out[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1;
        int step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int j = 0; j < half; ++j) {
                int k = j * step;
                float wr = p->tw_re[k], wi = p->tw_im[k];
                float vr = re[i + j + half], vi = im_out[i + j + half];
                float tr = vr * wr - vi * wi;
                float ti = vr * wi + vi * wr;
                float ur = re[i + j], ui = im_out[i + j];
                re[i + j] = ur + tr;
                im_out[i + j] = ui + ti;
                re[i + j + half] = ur - tr;
                im_out[i + j + half] = ui - ti;
            }
        }
    }
}

int SpkEncoder::Init(const std::string& model_dir, const char* device_id) {
    Impl* im = impl_;
    if (im->ready) return 0;

    // 1) mel 滤波器组（导出脚本产物，杜绝公式复刻误差）
    std::string fb_path = model_dir + "/spk_mel_128x513.f32";
    {
        std::ifstream f(fb_path, std::ios::binary);
        if (!f) {
            LOGE("Init: 缺 %s（板端提取不可用，跑 tools/export_spk_encoder.py 生成）", fb_path.c_str());
            return -1;
        }
        f.seekg(0, std::ios::end);
        if ((size_t)f.tellg() != (size_t)kMelBins * kSpecBins * sizeof(float)) {
            LOGE("Init: %s 大小不对", fb_path.c_str());
            return -1;
        }
        f.seekg(0, std::ios::beg);
        im->mel_fb.resize((size_t)kMelBins * kSpecBins);
        f.read(reinterpret_cast<char*>(im->mel_fb.data()), im->mel_fb.size() * sizeof(float));
        if (f.fail()) return -1;
    }

    // 2) rknn 模型（与 text_projection 同一套普通模型流程）
    rknn3_init_extend init_extend;
    memset(&init_extend, 0, sizeof(init_extend));
    if (device_id != NULL && device_id[0] != '\0') {
        init_extend.device_id = const_cast<char*>(device_id);
    }
    if (rknn3_init(&im->ctx, &init_extend) < 0) {
        LOGE("Init: rknn3_init failed");
        return -1;
    }
    std::string model_path = model_dir + "/spk_embed.rknn";
    std::string weight_path = model_dir + "/spk_embed.weight";
    if (rknn3_load_model_from_path(im->ctx, model_path.c_str(), weight_path.c_str()) < 0) {
        LOGE("Init: 加载 %s 失败", model_path.c_str());
        return -1;
    }
    rknn3_config config;
    memset(&config, 0, sizeof(config));
    config.run_core_mask = 0xFF;  // 模型按 8 核转换（与 vl_demo 普通模型一致）
    if (rknn3_model_init(im->ctx, &config) < 0) {
        LOGE("Init: rknn3_model_init failed");
        return -1;
    }

    rknn3_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn3_query(im->ctx, RKNN3_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) < 0 ||
        io_num.n_input != 1 || io_num.n_output != 1) {
        LOGE("Init: 期望 1 输入 1 输出");
        return -1;
    }

    rknn3_tensor_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.index = 0;
    if (rknn3_query(im->ctx, RKNN3_QUERY_INPUT_ATTR, &attr, sizeof(attr)) < 0) return -1;
    im->in_elems = attr.n_elems;
    im->in_fp16 = (attr.aligned_size >= attr.n_elems * 2 && attr.aligned_size < attr.n_elems * 4);
    memset(&im->input, 0, sizeof(im->input));
    im->input.attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
    memcpy(im->input.attr, &attr, sizeof(attr));
    im->input.mem = rknn3_create_mem(im->ctx, attr.aligned_size, attr.core_id, RKNN3_FLAG_MEMORY_CACHEABLE);
    if (im->input.mem == NULL) return -1;

    memset(&attr, 0, sizeof(attr));
    attr.index = 0;
    if (rknn3_query(im->ctx, RKNN3_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) < 0) return -1;
    im->out_elems = attr.n_elems;
    im->out_fp16 = (attr.aligned_size >= attr.n_elems * 2 && attr.aligned_size < attr.n_elems * 4);
    memset(&im->output, 0, sizeof(im->output));
    im->output.attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
    memcpy(im->output.attr, &attr, sizeof(attr));
    im->output.mem = rknn3_create_mem(im->ctx, attr.aligned_size, attr.core_id, RKNN3_FLAG_MEMORY_CACHEABLE);
    if (im->output.mem == NULL) return -1;

    LOGI("输入 %zu elems(%s) 输出 %zu elems(%s)",
         im->in_elems, im->in_fp16 ? "fp16" : "fp32",
         im->out_elems, im->out_fp16 ? "fp16" : "fp32");
    if (im->in_elems != (size_t)kFrames * kMelBins || im->out_elems != (size_t)kEmbedDim) {
        LOGE("Init: IO 形状不符，期望 (%d,%d)→%d", kFrames, kMelBins, kEmbedDim);
        return -1;
    }

    // 3) 预处理常量表
    fft_init(&im->fft);
    im->hann.resize(kNfft);
    for (int i = 0; i < kNfft; ++i) {
        im->hann[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * i / kNfft)));  // periodic
    }
    im->work.assign(kAudioLen + 2 * kPad, 0.f);
    im->fft_re.resize(kNfft);
    im->fft_im.assign(kNfft, 0.f);
    im->mag.resize(kSpecBins);
    im->mel_out.assign((size_t)kFrames * kMelBins, 0.f);

    im->ready = true;
    return 0;
}

int SpkEncoder::Extract(const float* pcm, int n_samples, float* out) {
    Impl* im = impl_;
    if (!im->ready) return -1;
    if (pcm == NULL || n_samples <= 0) return -1;

    // 1) 定长波形 + 两侧 reflect pad（与 F.pad(mode="reflect") 一致，不重复边缘样本）
    const int copy_n = n_samples < kAudioLen ? n_samples : kAudioLen;
    memcpy(im->work.data() + kPad, pcm, copy_n * sizeof(float));
    for (int i = 0; i < kPad; ++i) {
        im->work[i] = pcm[kPad - i];
        im->work[kPad + kAudioLen + i] = pcm[kAudioLen - 1 - i];
    }
    // 注意：copy_n < kAudioLen 时右侧 reflect 取到补零区，与 python 侧“先截/补到
    // 定长再 pad”一致（python 侧先 pad 波形到 10s 再 reflect）。

    // 2) STFT → |spec| → mel → log
    for (int f = 0; f < kFrames; ++f) {
        const float* base = im->work.data() + (size_t)f * kHop;
        for (int i = 0; i < kNfft; ++i) {
            im->fft_re[i] = base[i] * im->hann[i];
            im->fft_im[i] = 0.f;
        }
        fft_run(&im->fft, im->fft_re.data(), im->fft_im.data());
        for (int k = 0; k < kSpecBins; ++k) {
            im->mag[k] = sqrtf(im->fft_re[k] * im->fft_re[k] + im->fft_im[k] * im->fft_im[k]);
        }
        float* dst = im->mel_out.data() + (size_t)f * kMelBins;
        for (int j = 0; j < kMelBins; ++j) {
            const float* fb = im->mel_fb.data() + (size_t)j * kSpecBins;
            float s = 0.f;
            for (int k = 0; k < kSpecBins; ++k) s += fb[k] * im->mag[k];
            dst[j] = logf(s < 1e-5f ? 1e-5f : s);
        }
    }

    // 3) 推理（dtype 自适应：fp16 模型转半精度进出）
    void* in_addr = im->input.mem->virt_addr;
    if (im->in_fp16) {
        float16* p = (float16*)in_addr;
        for (size_t i = 0; i < im->in_elems; ++i) p[i] = fp32_to_fp16(im->mel_out[i]);
    } else {
        memcpy(in_addr, im->mel_out.data(), im->in_elems * sizeof(float));
    }
    if (rknn3_mem_sync(im->ctx, im->input.mem, RKNN3_MEMORY_SYNC_TO_DEVICE) != RKNN3_SUCCESS) {
        LOGE("Extract: input mem_sync failed");
        return -1;
    }
    if (rknn3_run(im->ctx, &im->input, 1, &im->output, 1) != RKNN3_SUCCESS) {
        LOGE("Extract: rknn3_run failed");
        return -1;
    }
    if (rknn3_mem_sync(im->ctx, im->output.mem, RKNN3_MEMORY_SYNC_FROM_DEVICE) != RKNN3_SUCCESS) {
        LOGE("Extract: output mem_sync failed");
        return -1;
    }
    if (im->out_fp16) {
        float16* p = (float16*)im->output.mem->virt_addr;
        for (size_t i = 0; i < im->out_elems; ++i) out[i] = fp16_to_fp32(p[i]);
    } else {
        memcpy(out, im->output.mem->virt_addr, im->out_elems * sizeof(float));
    }
    return 0;
}

int SpkEncoder_SaveNpy(const std::string& path, const float* v, int dim) {
    // numpy v1：magic + 版本 + u16 头长 + 字典（补空格对齐 64B，以 \n 结尾）+ 数据
    std::string dict = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(dim) + ",), }";
    while ((10 + dict.size() + 1) % 64 != 0) dict += ' ';
    dict += '\n';

    std::ofstream f(path, std::ios::binary);
    if (!f) return -1;
    char magic[10] = {(char)0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, 0, 0};
    uint16_t hlen = (uint16_t)dict.size();
    memcpy(magic + 8, &hlen, 2);
    f.write(magic, 10);
    f.write(dict.data(), dict.size());
    f.write(reinterpret_cast<const char*>(v), (size_t)dim * sizeof(float));
    return f.good() ? 0 : -1;
}
