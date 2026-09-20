// YOLO26 三任务常驻推理引擎（RK1828 板端）：seg 分割 / pose 姿态 / det 检测
//
// 外壳同 tts/vl/depth：Unix-socket 行 JSON 服务，模型按 load 命令懒加载。
// 后处理移植自 rknn3-model-zoo examples/yolo26{,_segment,_pose}/cpp（W8A8 INT8 路径）：
//   - 每尺度输出按通道数识别角色（score 判定在 box 前——80 也满足 %4==0）
//   - seg：box 4ch/score 80ch/coeff 32ch ×3 + proto [1,32,160,160]；
//     mask = coeff×proto 整数点积，只在 bbox 对应 proto 矩形内算，
//     nearest 上采样到 bbox-local 原图像素，再 1-bit 打包 base64 回传
//   - pose：box 4ch/score 1ch/kpt 51ch；关键点 (dx+j+0.5)*stride，conf 过 sigmoid
//   - det：box 4ch/score 80ch[/score_sum 1ch 快速预过滤]（6 或 9 输出）
//   - box 解码 x1=(-b0+j+0.5)*stride（YOLO26 直接回归，无 DFL softmax）
//
// 命令行: ./yolo26_engine <model_dir> [sock_path]      # socket 服务（默认 /tmp/yolo26_engine.sock）
//         ./yolo26_engine --cli <model_dir> <task> <img> # 单图 CLI（金标准对照用）

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <algorithm>
#include <string>
#include <vector>

#include "rknn3_api.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ── 常量（与官方 postprocess.h 对齐）────────────────────────────────
static const char* kDefaultSock = "/tmp/yolo26_engine.sock";
static const char* kDefaultModelDir = "/root/yolo26_demo/model";

#define OBJ_CLASS_NUM 80
#define OBJ_MASK_DIM 32
#define OBJ_KPT_NUM 17          // COCO 17 关键点（pose）
#define BOX_THRESH 0.25f
#define NMS_THRESH 0.45f
#define SEG_MASK_THRESH 0.5f
#define OBJ_NUMB_MAX_SIZE 128   // 官方上限
#define EMIT_OBJ_MAX 32         // 实时回传上限（mask/kp JSON 体积控制）

// 任务表
static const struct {
    const char* task;
    const char* rknn;
    const char* weight;
} kTasks[] = {
    {"seg", "yolo26n-seg.rknn", "yolo26n-seg.weight"}, // [0] 兜底默认（无 load 直接 infer 时）
    {"pose", "yolo26n-pose.rknn", "yolo26n-pose.weight"},
    {"det", "yolo26n-det.rknn", "yolo26n-det.weight"},
};

// COCO 80 类（内嵌，免外部文件依赖）
static const char* kCocoNames[OBJ_CLASS_NUM] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"};

static double get_time_ms()
{
    timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int clampi(float val, int lo, int hi)
{
    return val > lo ? (val < hi ? (int)val : hi) : lo;
}

// ── base64（mask 打包回传用）────────────────────────────────────────
static std::string b64_encode(const uint8_t* p, size_t n)
{
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += T[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = p[i] << 16;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += "==";
    } else if (n - i == 2) {
        uint32_t v = (p[i] << 16) | (p[i + 1] << 8);
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// ── letterbox：任意尺寸 RGB → 640×640，bg=114，双线性 ────────────────
struct LetterBox {
    float scale;
    int x_pad, y_pad;
};

static void letterbox_rgb(const uint8_t* src, int sw, int sh, int sc,
                          uint8_t* dst, int dw, int dh, LetterBox* lb)
{
    // 恒等情形（摄像头环路发来的就是 640×640 RGB）：直接拷贝
    if (sw == dw && sh == dh && sc == 3) {
        memcpy(dst, src, (size_t)dw * dh * 3);
        lb->scale = 1.0f;
        lb->x_pad = lb->y_pad = 0;
        return;
    }
    float scale = std::min((float)dw / sw, (float)dh / sh);
    int nw = (int)roundf(sw * scale);
    int nh = (int)roundf(sh * scale);
    int px = (dw - nw) / 2;
    int py = (dh - nh) / 2;
    lb->scale = scale;
    lb->x_pad = px;
    lb->y_pad = py;

    memset(dst, 114, (size_t)dw * dh * 3);
    for (int y = 0; y < nh; y++) {
        float sy = (y + 0.5f) / scale - 0.5f;
        int y0 = clampi(sy, 0, sh - 1);
        int y1 = clampi(sy + 1, 0, sh - 1);
        float fy = sy - floorf(sy);
        if (sy < 0) { y0 = y1 = 0; fy = 0; }
        if (sy > sh - 1) { y0 = y1 = sh - 1; fy = 0; }
        uint8_t* drow = dst + ((size_t)(py + y) * dw + px) * 3;
        const uint8_t* s0 = src + (size_t)y0 * sw * sc;
        const uint8_t* s1 = src + (size_t)y1 * sw * sc;
        for (int x = 0; x < nw; x++) {
            float sx = (x + 0.5f) / scale - 0.5f;
            int x0 = clampi(sx, 0, sw - 1);
            int x1 = clampi(sx + 1, 0, sw - 1);
            float fx = sx - floorf(sx);
            if (sx < 0) { x0 = x1 = 0; fx = 0; }
            if (sx > sw - 1) { x0 = x1 = sw - 1; fx = 0; }
            for (int c = 0; c < 3; c++) {
                float v00 = s0[(size_t)x0 * sc + c], v10 = s0[(size_t)x1 * sc + c];
                float v01 = s1[(size_t)x0 * sc + c], v11 = s1[(size_t)x1 * sc + c];
                float v = (v00 * (1 - fx) + v10 * fx) * (1 - fy) +
                          (v01 * (1 - fx) + v11 * fx) * fy;
                drow[x * 3 + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

// ── 摄像头环路内容带粘贴：浏览器已把画面缩放到模型分辨率，
// 引擎只需居中贴进 640×640（bg 114）；bpp=4 时顺带剥掉 alpha。
// 内容像素与模型像素 1:1，故 lb.scale=1，pad 即内容带偏移。
static void paste_content(const uint8_t* src, int cw, int ch, int bpp,
                          uint8_t* dst, int dw, int dh, LetterBox* lb)
{
    int px = (dw - cw) / 2, py = (dh - ch) / 2;
    // 坐标恒等映射：内容带即模型分辨率，输出一律 640 全幅空间，
    // 页面自己拿 letterbox 的 pad 换算到视频画面（pad 只用于贴图，不进 lb）
    lb->scale = 1.0f;
    lb->x_pad = 0;
    lb->y_pad = 0;
    if (px == 0 && py == 0 && cw == dw && ch == dh && bpp == 3) {
        memcpy(dst, src, (size_t)dw * dh * 3);
        return;
    }
    memset(dst, 114, (size_t)dw * dh * 3);
    for (int y = 0; y < ch; y++) {
        const uint8_t* s = src + (size_t)y * cw * bpp;
        uint8_t* d = dst + ((size_t)(py + y) * dw + px) * 3;
        if (bpp == 3) {
            memcpy(d, s, (size_t)cw * 3);
        } else {
            for (int x = 0; x < cw; x++) {
                d[x * 3] = s[x * 4];
                d[x * 3 + 1] = s[x * 4 + 1];
                d[x * 3 + 2] = s[x * 4 + 2];
            }
        }
    }
}

// ── tensor 布局辅助（移植自官方 seg postprocess.cc）──────────────────
static int tensor_channel(const rknn3_tensor_attr* a)
{
    return a->n_dims != 4 ? 0 : (a->layout == RKNN3_TENSOR_NHWC ? a->shape[3] : a->shape[1]);
}
static int tensor_grid_h(const rknn3_tensor_attr* a)
{
    return a->n_dims != 4 ? 0 : (a->layout == RKNN3_TENSOR_NHWC ? a->shape[1] : a->shape[2]);
}
static int tensor_grid_w(const rknn3_tensor_attr* a)
{
    return a->n_dims != 4 ? 0 : (a->layout == RKNN3_TENSOR_NHWC ? a->shape[2] : a->shape[3]);
}
static void tensor_chw_strides(const rknn3_tensor_attr* a, int* cs, int* hs, int* ws)
{
    int h = tensor_grid_h(a), w = tensor_grid_w(a), c = tensor_channel(a);
    if (a->n_stride >= a->n_dims && a->n_dims == 4) {
        *cs = a->stride[a->layout == RKNN3_TENSOR_NHWC ? 3 : 1];
        *hs = a->stride[a->layout == RKNN3_TENSOR_NHWC ? 1 : 2];
        *ws = a->stride[a->layout == RKNN3_TENSOR_NHWC ? 2 : 3];
        return;
    }
    if (a->layout == RKNN3_TENSOR_NHWC) {
        *cs = 1;
        *hs = w * c;
        *ws = c;
        return;
    }
    *cs = h * w;
    *hs = w;
    *ws = 1;
}

static inline float deqnt_i8(int8_t q, int32_t zp, float scale)
{
    return ((float)q - (float)zp) * scale;
}
static inline int8_t qnt_f32(float f, int32_t zp, float scale)
{
    float v = f / scale + zp;
    return (int8_t)(v <= -128 ? -128 : (v >= 127 ? 127 : v));
}
static inline float sigmoidf(float x) { return 1.0f / (1.0f + expf(-x)); }

// score 输出是否已在概率域（官方同名逻辑的精简版：名字带 sigmoid，
// 或量化范围本身就落在 [0,1]）
static bool score_is_probability(const rknn3_tensor_attr* a)
{
    if (a->name && strstr(a->name, "sigmoid"))
        return true;
    if (a->dtype == RKNN3_TENSOR_INT8 && a->qnt_info.scale > 0.0f) {
        float qmin = deqnt_i8(-128, a->qnt_info.zero_point, a->qnt_info.scale);
        float qmax = deqnt_i8(127, a->qnt_info.zero_point, a->qnt_info.scale);
        if (qmin >= -0.01f && qmax <= 1.01f)
            return true;
    }
    return false;
}

// ── NMS（与官方一致：+1 像素重叠约定，按类过滤）─────────────────────
struct Cand {
    float x1, y1, w, h; // 模型输入空间
    float score;
    int cls;
    int8_t coeff[OBJ_MASK_DIM];
    int32_t coeff_zp;  // 所在尺度 seg 头的量化参数（尺度间不同，必须跟着候选走）
    float coeff_scale;
    float kp[OBJ_KPT_NUM * 3]; // pose：x,y,conf（模型输入空间，置信度已过 sigmoid）
};

static float overlap_iou(const Cand& a, const Cand& b)
{
    float ax2 = a.x1 + a.w, ay2 = a.y1 + a.h;
    float bx2 = b.x1 + b.w, by2 = b.y1 + b.h;
    float iw = fmaxf(0.f, fminf(ax2, bx2) - fmaxf(a.x1, b.x1) + 1.0f);
    float ih = fmaxf(0.f, fminf(ay2, by2) - fmaxf(a.y1, b.y1) + 1.0f);
    float inter = iw * ih;
    float ua = (ax2 - a.x1 + 1.0f) * (ay2 - a.y1 + 1.0f) +
               (bx2 - b.x1 + 1.0f) * (by2 - b.y1 + 1.0f) - inter;
    return ua <= 0.f ? 0.f : inter / ua;
}

// ── 引擎主体 ─────────────────────────────────────────────────────────
struct Engine {
    rknn3_context ctx = 0;
    rknn3_input_output_num io{};
    rknn3_tensor* inputs = nullptr;
    rknn3_tensor* outputs = nullptr;
    int model_w = 0, model_h = 0, model_c = 0;
    bool is_quant = false;
    bool loaded = false;
    std::string task;
    double load_ms = 0;

    // postprocess 常驻 buffer（避免每帧分配）
    std::vector<Cand> cands;
    std::vector<int> order;
    std::vector<int> x_to_proto, y_to_proto;
    std::vector<uint8_t> proto_mask_scratch;
    std::vector<uint8_t> input_buf;

    // 单帧输入缓存（640×640 RGB）
    int cap_w = 0, cap_h = 0;
    std::vector<uint8_t> letterboxed;
    std::vector<uint8_t> raw_buf;  // 摄像头环路裸 RGB 文件读取缓冲

    int load(const char* model_dir, const char* want_task, std::string* err);
    void release();

    // 返回 0 成功；objects JSON 由 infer_json 组装
    int infer(const char* img_path, float conf_thresh, float nms_thresh,
              std::string* objs_json, int* n_out, int* ow, int* oh,
              double* pre_ms, double* npu_ms, double* post_ms, std::string* err);
    // raw_rgb 路径：rw×rh 为内容带尺寸（已是模型分辨率），bpp=3 RGB / 4 RGBA
    int infer_ex(const char* img_path, const uint8_t* raw_rgb, int rw, int rh,
                 int bpp, float conf_thresh, float nms_thresh,
                 std::string* objs_json, int* n_out, int* ow, int* oh,
                 double* pre_ms, double* npu_ms, double* post_ms,
                 std::string* err);

    int postprocess(const LetterBox& lb, int ow, int oh, float conf, float nms,
                    std::string* objs_json, int* n_out);
    int build_mask_i8(const rknn3_tensor* proto_t, const int8_t* coeff,
                      int32_t coeff_zp, float coeff_scale,
                      const int box[4], int ow, int oh,
                      int* mx, int* my, int* mw, int* mh,
                      std::vector<uint8_t>* packed);
};

int Engine::load(const char* model_dir, const char* want_task, std::string* err)
{
    const char* rknn_name = nullptr;
    const char* weight_name = nullptr;
    for (auto& t : kTasks) {
        if (strcmp(t.task, want_task) == 0) {
            rknn_name = t.rknn;
            weight_name = t.weight;
            break;
        }
    }
    if (!rknn_name) {
        *err = std::string("未知任务: ") + want_task;
        return -1;
    }

    if (loaded) {
        if (task == want_task)
            return 0;
        release(); // 换任务：先卸旧模型
    }

    std::string rknn_path = std::string(model_dir) + "/" + rknn_name;
    std::string weight_path = std::string(model_dir) + "/" + weight_name;

    double t0 = get_time_ms();
    int ret = rknn3_init(&ctx, NULL);
    if (ret < 0) {
        *err = "rknn3_init 失败 ret=" + std::to_string(ret);
        return ret;
    }
    ret = rknn3_load_model_from_path(ctx, rknn_path.c_str(), weight_path.c_str());
    if (ret < 0) {
        *err = "加载模型失败（检查 " + rknn_path + "）ret=" + std::to_string(ret);
        rknn3_destroy(ctx);
        ctx = 0;
        return ret;
    }
    rknn3_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.run_core_mask = 0x1; // core-num 1 转换的模型（README：8 核转换才用 0xff）
    ret = rknn3_model_init(ctx, &cfg);
    if (ret < 0) {
        *err = "model_init 失败 ret=" + std::to_string(ret) +
               "（若转换用了 --core-num 8，这里会失败，需要 0xff）";
        rknn3_destroy(ctx);
        ctx = 0;
        return ret;
    }
    ret = rknn3_query(ctx, RKNN3_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (ret < 0 || io.n_input <= 0 || io.n_output <= 0) {
        *err = "query io_num 失败";
        release();
        return -1;
    }
    fprintf(stderr, "[yolo26] %s: input=%d output=%d\n", task.c_str(), io.n_input,
            io.n_output);

    std::vector<rknn3_tensor_attr> in_attrs(io.n_input);
    for (int i = 0; i < io.n_input; i++) {
        in_attrs[i].index = i;
        rknn3_query(ctx, RKNN3_QUERY_INPUT_ATTR, &in_attrs[i], sizeof(rknn3_tensor_attr));
    }
    std::vector<rknn3_tensor_attr> out_attrs(io.n_output);
    for (int i = 0; i < io.n_output; i++) {
        out_attrs[i].index = i;
        rknn3_query(ctx, RKNN3_QUERY_OUTPUT_ATTR, &out_attrs[i], sizeof(rknn3_tensor_attr));
        fprintf(stderr, "[yolo26] out[%d] %s shape=[%d,%d,%d,%d] dtype=%d qnt=%d\n", i,
                out_attrs[i].name ? out_attrs[i].name : "?", out_attrs[i].shape[0],
                out_attrs[i].shape[1], out_attrs[i].shape[2], out_attrs[i].shape[3],
                (int)out_attrs[i].dtype, (int)out_attrs[i].qnt_type);
    }

    inputs = (rknn3_tensor*)calloc(io.n_input, sizeof(rknn3_tensor));
    outputs = (rknn3_tensor*)calloc(io.n_output, sizeof(rknn3_tensor));
    for (int i = 0; i < io.n_input; i++) {
        inputs[i].mem = rknn3_create_mem(ctx, in_attrs[i].aligned_size, in_attrs[i].core_id,
                                         RKNN3_FLAG_MEMORY_CACHEABLE);
        inputs[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        memcpy(inputs[i].attr, &in_attrs[i], sizeof(rknn3_tensor_attr));
    }
    for (int i = 0; i < io.n_output; i++) {
        outputs[i].mem = rknn3_create_mem(ctx, out_attrs[i].aligned_size,
                                          out_attrs[i].core_id,
                                          RKNN3_FLAG_MEMORY_CACHEABLE);
        outputs[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        memcpy(outputs[i].attr, &out_attrs[i], sizeof(rknn3_tensor_attr));
    }

    is_quant = (out_attrs[0].qnt_type == RKNN3_TENSOR_PER_LAYER_ASYMMETRIC &&
                out_attrs[0].dtype == RKNN3_TENSOR_INT8);
    if (!is_quant) {
        *err = "本 demo 只支持 W8A8 INT8 模型（当前输出不是 INT8）";
        release();
        return -1;
    }

    if (in_attrs[0].layout == RKNN3_TENSOR_NHWC) {
        model_h = in_attrs[0].shape[1];
        model_w = in_attrs[0].shape[2];
        model_c = in_attrs[0].shape[3];
    } else {
        model_c = in_attrs[0].shape[1];
        model_h = in_attrs[0].shape[2];
        model_w = in_attrs[0].shape[3];
    }
    fprintf(stderr, "[yolo26] input %dx%dx%d quant=%d\n", model_w, model_h, model_c,
            is_quant);

    task = want_task;
    loaded = true;
    load_ms = get_time_ms() - t0;
    return 0;
}

void Engine::release()
{
    if (inputs) {
        for (int i = 0; i < io.n_input; i++) {
            if (inputs[i].mem)
                rknn3_destroy_mem(ctx, inputs[i].mem);
            free(inputs[i].attr);
        }
        free(inputs);
        inputs = nullptr;
    }
    if (outputs) {
        for (int i = 0; i < io.n_output; i++) {
            if (outputs[i].mem)
                rknn3_destroy_mem(ctx, outputs[i].mem);
            free(outputs[i].attr);
        }
        free(outputs);
        outputs = nullptr;
    }
    if (ctx) {
        rknn3_destroy(ctx);
        ctx = 0;
    }
    io.n_input = io.n_output = 0;
    loaded = false;
    task.clear();
}

// INT8 单尺度解码（box/score/coeff 三个 tensor）
static int process_scale_i8(const rknn3_tensor* box_t, const rknn3_tensor* score_t,
                            const rknn3_tensor* mask_t, int stride,
                            std::vector<Cand>& cands, float threshold)
{
    const rknn3_tensor_attr* ba = box_t->attr;
    const rknn3_tensor_attr* sa = score_t->attr;
    const rknn3_tensor_attr* ma = mask_t->attr;
    int8_t* box_d = (int8_t*)box_t->mem->virt_addr;
    int8_t* score_d = (int8_t*)score_t->mem->virt_addr;
    int8_t* mask_d = (int8_t*)mask_t->mem->virt_addr;

    int gh = tensor_grid_h(ba), gw = tensor_grid_w(ba);
    int class_count = std::min(tensor_channel(sa), OBJ_CLASS_NUM);
    int bcs, bhs, bws, scs, shs, sws, mcs, mhs, mws;
    tensor_chw_strides(ba, &bcs, &bhs, &bws);
    tensor_chw_strides(sa, &scs, &shs, &sws);
    tensor_chw_strides(ma, &mcs, &mhs, &mws);

    bool score_prob = score_is_probability(sa);
    float cmp_th = threshold;
    if (!score_prob)
        cmp_th = logf(threshold / (1.0f - threshold));
    int8_t score_th_i8 = qnt_f32(cmp_th, sa->qnt_info.zero_point, sa->qnt_info.scale);

    int added = 0;
    for (int i = 0; i < gh; i++) {
        for (int j = 0; j < gw; j++) {
            int max_cls = -1;
            int8_t max_i8 = -128;
            int sbase = i * shs + j * sws;
            for (int c = 0; c < class_count; c++) {
                int8_t v = score_d[sbase + c * scs];
                if (v >= score_th_i8 && v > max_i8) {
                    max_i8 = v;
                    max_cls = c;
                }
            }
            if (max_cls < 0)
                continue;
            float score = deqnt_i8(max_i8, sa->qnt_info.zero_point, sa->qnt_info.scale);
            if (!score_prob)
                score = sigmoidf(score);
            if (score <= threshold)
                continue;

            Cand cd;
            int bbase = i * bhs + j * bws;
            float b[4];
            for (int k = 0; k < 4; k++)
                b[k] = deqnt_i8(box_d[bbase + k * bcs], ba->qnt_info.zero_point,
                                ba->qnt_info.scale);
            // YOLO26 box：直接回归，左/上为负向距离
            cd.x1 = (-b[0] + j + 0.5f) * stride;
            cd.y1 = (-b[1] + i + 0.5f) * stride;
            cd.w = (b[2] + j + 0.5f) * stride - cd.x1;
            cd.h = (b[3] + i + 0.5f) * stride - cd.y1;
            cd.score = score;
            cd.cls = max_cls;
            cd.coeff_zp = ma->qnt_info.zero_point;
            cd.coeff_scale = ma->qnt_info.scale;
            int mbase = i * mhs + j * mws;
            for (int c = 0; c < OBJ_MASK_DIM; c++)
                cd.coeff[c] = mask_d[mbase + c * mcs];
            cands.push_back(cd);
            added++;
        }
    }
    return added;
}

// INT8 单尺度 pose 解码（box/score/kpt 三个 tensor，官方 process_i8 移植）
// box、score 与 seg 同一套；第三张 51ch = 17×(dx,dy,logit)：
//   x = (dx + j + 0.5) * stride，置信度过 sigmoid（KEYPOINT_THRESH 在页面画图时过滤）
static int process_scale_pose_i8(const rknn3_tensor* box_t, const rknn3_tensor* score_t,
                                 const rknn3_tensor* kpt_t, int stride,
                                 std::vector<Cand>& cands, float threshold)
{
    const rknn3_tensor_attr* ba = box_t->attr;
    const rknn3_tensor_attr* sa = score_t->attr;
    const rknn3_tensor_attr* ka = kpt_t->attr;
    int8_t* box_d = (int8_t*)box_t->mem->virt_addr;
    int8_t* score_d = (int8_t*)score_t->mem->virt_addr;
    int8_t* kpt_d = (int8_t*)kpt_t->mem->virt_addr;

    int gh = tensor_grid_h(ba), gw = tensor_grid_w(ba);
    int class_count = std::min(tensor_channel(sa), OBJ_CLASS_NUM);
    int bcs, bhs, bws, scs, shs, sws, kcs, khs, kws;
    tensor_chw_strides(ba, &bcs, &bhs, &bws);
    tensor_chw_strides(sa, &scs, &shs, &sws);
    tensor_chw_strides(ka, &kcs, &khs, &kws);

    bool score_prob = score_is_probability(sa);
    float cmp_th = threshold;
    if (!score_prob)
        cmp_th = logf(threshold / (1.0f - threshold));
    int8_t score_th_i8 = qnt_f32(cmp_th, sa->qnt_info.zero_point, sa->qnt_info.scale);

    int added = 0;
    for (int i = 0; i < gh; i++) {
        for (int j = 0; j < gw; j++) {
            int max_cls = -1;
            int8_t max_i8 = -128;
            int sbase = i * shs + j * sws;
            for (int c = 0; c < class_count; c++) {
                int8_t v = score_d[sbase + c * scs];
                if (v >= score_th_i8 && v > max_i8) {
                    max_i8 = v;
                    max_cls = c;
                }
            }
            if (max_cls < 0)
                continue;
            float score = deqnt_i8(max_i8, sa->qnt_info.zero_point, sa->qnt_info.scale);
            if (!score_prob)
                score = sigmoidf(score);
            if (score <= threshold)
                continue;

            Cand cd;
            int bbase = i * bhs + j * bws;
            float b[4];
            for (int k = 0; k < 4; k++)
                b[k] = deqnt_i8(box_d[bbase + k * bcs], ba->qnt_info.zero_point,
                                ba->qnt_info.scale);
            cd.x1 = (-b[0] + j + 0.5f) * stride;
            cd.y1 = (-b[1] + i + 0.5f) * stride;
            cd.w = (b[2] + j + 0.5f) * stride - cd.x1;
            cd.h = (b[3] + i + 0.5f) * stride - cd.y1;
            cd.score = score;
            cd.cls = max_cls;
            int kbase = i * khs + j * kws;
            int32_t kzp = ka->qnt_info.zero_point;
            float ksc = ka->qnt_info.scale;
            for (int k = 0; k < OBJ_KPT_NUM; k++) {
                float dx = deqnt_i8(kpt_d[kbase + (k * 3 + 0) * kcs], kzp, ksc);
                float dy = deqnt_i8(kpt_d[kbase + (k * 3 + 1) * kcs], kzp, ksc);
                float cf = sigmoidf(deqnt_i8(kpt_d[kbase + (k * 3 + 2) * kcs], kzp, ksc));
                cd.kp[k * 3 + 0] = (dx + j + 0.5f) * stride;
                cd.kp[k * 3 + 1] = (dy + i + 0.5f) * stride;
                cd.kp[k * 3 + 2] = cf;
            }
            cands.push_back(cd);
            added++;
        }
    }
    return added;
}

// INT8 单尺度 det 解码（box/score [+score_sum]，官方 process_i8 移植）
// box/score 与 seg 同一套；score_sum 是 1ch 快速预过滤（低于阈值的格子
// 直接跳过 80 类扫描），转换产物没有这个输出时传 nullptr
static int process_scale_det_i8(const rknn3_tensor* box_t, const rknn3_tensor* score_t,
                                const rknn3_tensor* sum_t, int stride,
                                std::vector<Cand>& cands, float threshold)
{
    const rknn3_tensor_attr* ba = box_t->attr;
    const rknn3_tensor_attr* sa = score_t->attr;
    int8_t* box_d = (int8_t*)box_t->mem->virt_addr;
    int8_t* score_d = (int8_t*)score_t->mem->virt_addr;
    int8_t* sum_d = sum_t ? (int8_t*)sum_t->mem->virt_addr : nullptr;

    int gh = tensor_grid_h(ba), gw = tensor_grid_w(ba);
    int class_count = std::min(tensor_channel(sa), OBJ_CLASS_NUM);
    int bcs, bhs, bws, scs, shs, sws;
    tensor_chw_strides(ba, &bcs, &bhs, &bws);
    tensor_chw_strides(sa, &scs, &shs, &sws);
    int scs_sum = 0, shs_sum = 0, sws_sum = 0;
    int8_t sum_th_i8 = -128;
    if (sum_d) {
        tensor_chw_strides(sum_t->attr, &scs_sum, &shs_sum, &sws_sum);
        // 官方对 score_sum 直接用概率域阈值（不走 logits 换算）
        sum_th_i8 = qnt_f32(threshold, sum_t->attr->qnt_info.zero_point,
                            sum_t->attr->qnt_info.scale);
    }

    bool score_prob = score_is_probability(sa);
    float cmp_th = threshold;
    if (!score_prob)
        cmp_th = logf(threshold / (1.0f - threshold));
    int8_t score_th_i8 = qnt_f32(cmp_th, sa->qnt_info.zero_point, sa->qnt_info.scale);

    int added = 0;
    for (int i = 0; i < gh; i++) {
        for (int j = 0; j < gw; j++) {
            if (sum_d && sum_d[i * shs_sum + j * sws_sum] < sum_th_i8)
                continue;
            int max_cls = -1;
            int8_t max_i8 = -128;
            int sbase = i * shs + j * sws;
            for (int c = 0; c < class_count; c++) {
                int8_t v = score_d[sbase + c * scs];
                if (v >= score_th_i8 && v > max_i8) {
                    max_i8 = v;
                    max_cls = c;
                }
            }
            if (max_cls < 0)
                continue;
            float score = deqnt_i8(max_i8, sa->qnt_info.zero_point, sa->qnt_info.scale);
            if (!score_prob)
                score = sigmoidf(score);
            if (score <= threshold)
                continue;

            Cand cd;
            int bbase = i * bhs + j * bws;
            float b[4];
            for (int k = 0; k < 4; k++)
                b[k] = deqnt_i8(box_d[bbase + k * bcs], ba->qnt_info.zero_point,
                                ba->qnt_info.scale);
            cd.x1 = (-b[0] + j + 0.5f) * stride;
            cd.y1 = (-b[1] + i + 0.5f) * stride;
            cd.w = (b[2] + j + 0.5f) * stride - cd.x1;
            cd.h = (b[3] + i + 0.5f) * stride - cd.y1;
            cd.score = score;
            cd.cls = max_cls;
            cands.push_back(cd);
            added++;
        }
    }
    return added;
}

// bbox-local mask：整数点积（官方 build_instance_mask_i8 移植）
// 输出 packed：1-bit/px，行按字节对齐，MSB 在前
int Engine::build_mask_i8(const rknn3_tensor* proto_t, const int8_t* coeff,
                          int32_t coeff_zp, float coeff_scale,
                          const int box[4], int ow, int oh,
                          int* mx, int* my, int* mw, int* mh,
                          std::vector<uint8_t>* packed)
{
    if (proto_t->attr->dtype != RKNN3_TENSOR_INT8)
        return -1;
    int ph = tensor_grid_h(proto_t->attr), pw = tensor_grid_w(proto_t->attr);
    float dot_scale = coeff_scale * proto_t->attr->qnt_info.scale;
    if (dot_scale <= 0.0f)
        return -1;

    int left = clampi(box[0], 0, ow - 1), right = clampi(box[2], 1, ow);
    int top = clampi(box[1], 0, oh - 1), bottom = clampi(box[3], 1, oh);
    left = std::min(left, right - 1);
    top = std::min(top, bottom - 1);
    if (left >= right || top >= bottom)
        return 0;

    int w = right - left, h = bottom - top;
    *mx = left;
    *my = top;
    *mw = w;
    *mh = h;

    int proto_left = x_to_proto[left], proto_right = x_to_proto[right - 1];
    int proto_top = y_to_proto[top], proto_bottom = y_to_proto[bottom - 1];
    if (proto_left > proto_right)
        std::swap(proto_left, proto_right);
    if (proto_top > proto_bottom)
        std::swap(proto_top, proto_bottom);

    int pcs, phs, pws;
    tensor_chw_strides(proto_t->attr, &pcs, &phs, &pws);
    const int8_t* proto_d = (const int8_t*)proto_t->mem->virt_addr;
    int32_t proto_zp = proto_t->attr->qnt_info.zero_point;
    float thresh_logit = logf(SEG_MASK_THRESH / (1.0f - SEG_MASK_THRESH));
    int32_t acc_th = (int32_t)floorf(thresh_logit / dot_scale);

    int32_t coeff_delta[OBJ_MASK_DIM];
    int proto_c_off[OBJ_MASK_DIM];
    for (int c = 0; c < OBJ_MASK_DIM; c++) {
        coeff_delta[c] = (int32_t)coeff[c] - coeff_zp;
        proto_c_off[c] = c * pcs;
    }

    proto_mask_scratch.assign((size_t)ph * pw, 0);
    for (int py = proto_top; py <= proto_bottom; py++) {
        int ybase = py * phs;
        for (int px = proto_left; px <= proto_right; px++) {
            int pbase = ybase + px * pws;
            int32_t acc = 0;
            for (int c = 0; c < OBJ_MASK_DIM; c++)
                acc += coeff_delta[c] * ((int32_t)proto_d[proto_c_off[c] + pbase] - proto_zp);
            proto_mask_scratch[py * pw + px] = acc > acc_th ? 255 : 0;
        }
    }

    // nearest 上采样到 bbox-local，同时 1-bit 打包
    int stride_bytes = (w + 7) / 8;
    packed->assign((size_t)stride_bytes * h, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* prow = proto_mask_scratch.data() + (size_t)y_to_proto[top + y] * pw;
        uint8_t* orow = packed->data() + (size_t)y * stride_bytes;
        for (int x = 0; x < w; x++)
            if (prow[x_to_proto[left + x]])
                orow[x >> 3] |= 0x80 >> (x & 7);
    }
    return 1;
}

int Engine::postprocess(const LetterBox& lb, int ow, int oh, float conf, float nms,
                        std::string* objs_json, int* n_out)
{
    cands.clear();
    cands.reserve(1024);
    bool pose = (task == "pose");
    bool det = (task == "det");

    // 输出角色识别（score 判定要在 box 前面——80 也满足 %4==0）：
    //   det：每尺度 box 4ch/score 80ch[/score_sum 1ch]，共 6 或 9 个输出
    //   pose：每尺度 box 4ch/score 1ch/kpt 51ch，共 9 个
    //   seg：每尺度 box 4ch/score 80ch/coeff 32ch + 第 10 个 proto
    if (det) {
        if (io.n_output != 6 && io.n_output != 9) {
            fprintf(stderr, "[yolo26] 输出数 %d 不是 6/9，不是 det 模型？\n", io.n_output);
            return -1;
        }
        int per = io.n_output / 3;
        for (int i = 0; i < 3; i++) {
            int box_idx = -1, score_idx = -1, sum_idx = -1;
            for (int j = 0; j < per; j++) {
                int idx = i * per + j;
                int ch = tensor_channel(outputs[idx].attr);
                if (ch == OBJ_CLASS_NUM)
                    score_idx = idx;
                else if (ch == 1)
                    sum_idx = idx;
                else if (ch >= 4 && ch % 4 == 0)
                    box_idx = idx;
            }
            if (box_idx < 0 || score_idx < 0) {
                fprintf(stderr, "[yolo26] det 尺度 %d 输出布局无法识别\n", i);
                return -1;
            }
            int gh = tensor_grid_h(outputs[box_idx].attr);
            int stride = model_h / gh;
            process_scale_det_i8(&outputs[box_idx], &outputs[score_idx],
                                 sum_idx >= 0 ? &outputs[sum_idx] : nullptr,
                                 stride, cands, conf);
        }
    } else if (pose) {
        if (io.n_output != 9) {
            fprintf(stderr, "[yolo26] 输出数 %d != 9，不是 pose 模型？\n", io.n_output);
            return -1;
        }
        for (int i = 0; i < 3; i++) {
            int box_idx = -1, score_idx = -1, kpt_idx = -1;
            for (int j = 0; j < 3; j++) {
                int idx = i * 3 + j;
                int ch = tensor_channel(outputs[idx].attr);
                if (ch == OBJ_CLASS_NUM || ch == 1)
                    score_idx = idx;
                else if (ch == OBJ_KPT_NUM * 3)
                    kpt_idx = idx;
                else if (ch >= 4 && ch % 4 == 0)
                    box_idx = idx;
            }
            if (box_idx < 0 || score_idx < 0 || kpt_idx < 0) {
                fprintf(stderr, "[yolo26] pose 尺度 %d 输出布局无法识别\n", i);
                return -1;
            }
            int gh = tensor_grid_h(outputs[box_idx].attr);
            int stride = model_h / gh;
            process_scale_pose_i8(&outputs[box_idx], &outputs[score_idx],
                                  &outputs[kpt_idx], stride, cands, conf);
        }
    } else {
        if (io.n_output != 10) {
            fprintf(stderr, "[yolo26] 输出数 %d != 10，不是 seg 模型？\n", io.n_output);
            return -1;
        }
        for (int i = 0; i < 3; i++) {
            int box_idx = -1, score_idx = -1, mask_idx = -1;
            for (int j = 0; j < 3; j++) {
                int idx = i * 3 + j;
                int ch = tensor_channel(outputs[idx].attr);
                if (ch == OBJ_CLASS_NUM)
                    score_idx = idx;
                else if (ch == OBJ_MASK_DIM)
                    mask_idx = idx;
                else if (ch >= 4 && ch % 4 == 0)
                    box_idx = idx;
            }
            if (box_idx < 0 || score_idx < 0 || mask_idx < 0) {
                fprintf(stderr, "[yolo26] 尺度 %d 输出布局无法识别\n", i);
                return -1;
            }
            int gh = tensor_grid_h(outputs[box_idx].attr);
            int stride = model_h / gh;
            process_scale_i8(&outputs[box_idx], &outputs[score_idx], &outputs[mask_idx],
                             stride, cands, conf);
        }
    }
    int valid = (int)cands.size();
    if (valid <= 0) {
        *objs_json = "[]";
        *n_out = 0;
        return 0;
    }

    // 按分排序（降序），同类 NMS
    order.resize(valid);
    for (int i = 0; i < valid; i++)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return cands[a].score > cands[b].score; });
    for (int i = 0; i < valid; i++) {
        int n = order[i];
        if (n < 0)
            continue;
        for (int j = i + 1; j < valid; j++) {
            int m = order[j];
            if (m < 0 || cands[m].cls != cands[n].cls)
                continue;
            if (overlap_iou(cands[n], cands[m]) > nms)
                order[j] = -1;
        }
    }

    // 逆 letterbox 回原图 + mask/kp 生成 + JSON（proto 查表只有 seg 要建）
    const rknn3_tensor* proto_t = &outputs[9];
    if (!pose && !det) {
        if ((int)x_to_proto.size() < ow)
            x_to_proto.resize(ow);
        if ((int)y_to_proto.size() < oh)
            y_to_proto.resize(oh);
        for (int x = 0; x < ow; x++) {
            float mx = x * lb.scale + lb.x_pad;
            x_to_proto[x] = clampi(mx * tensor_grid_w(proto_t->attr) / model_w, 0,
                                   tensor_grid_w(proto_t->attr) - 1);
        }
        for (int y = 0; y < oh; y++) {
            float my = y * lb.scale + lb.y_pad;
            y_to_proto[y] = clampi(my * tensor_grid_h(proto_t->attr) / model_h, 0,
                                   tensor_grid_h(proto_t->attr) - 1);
        }
    }

    std::vector<uint8_t> packed;
    objs_json->clear();
    objs_json->reserve(pose ? 16384 : 4096);
    *objs_json = "[";
    int emitted = 0;
    for (int i = 0; i < valid && emitted < EMIT_OBJ_MAX; i++) {
        int n = order[i];
        if (n < 0)
            continue;
        const Cand& cd = cands[n];
        // 逆 letterbox：clamp 到模型框内再除 scale
        float x1 = std::min(std::max(cd.x1 - lb.x_pad, 0.0f), (float)model_w) / lb.scale;
        float y1 = std::min(std::max(cd.y1 - lb.y_pad, 0.0f), (float)model_h) / lb.scale;
        float x2 = std::min(std::max(cd.x1 + cd.w - lb.x_pad, 0.0f), (float)model_w) / lb.scale;
        float y2 = std::min(std::max(cd.y1 + cd.h - lb.y_pad, 0.0f), (float)model_h) / lb.scale;
        int box[4] = {(int)x1, (int)y1, (int)x2, (int)y2};

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s{\"cls\":%d,\"name\":\"%s\",\"p\":%.3f,"
                 "\"box\":[%.1f,%.1f,%.1f,%.1f]",
                 emitted ? "," : "", cd.cls, kCocoNames[cd.cls], cd.score, x1, y1, x2, y2);
        *objs_json += buf;

        if (pose) {
            // 17 个关键点（原图空间），低置信度交给页面按 0.25 过滤（官方同款）
            *objs_json += ",\"kp\":[";
            for (int k = 0; k < OBJ_KPT_NUM; k++) {
                float kx = std::min(std::max(cd.kp[k * 3] - lb.x_pad, 0.0f),
                                    (float)model_w) / lb.scale;
                float ky = std::min(std::max(cd.kp[k * 3 + 1] - lb.y_pad, 0.0f),
                                    (float)model_h) / lb.scale;
                snprintf(buf, sizeof(buf), "%s[%.1f,%.1f,%.2f]",
                         k ? "," : "", kx, ky, cd.kp[k * 3 + 2]);
                *objs_json += buf;
            }
            *objs_json += "]}";
        } else if (det) {
            *objs_json += "}"; // det：只有框，无 mask/kp
        } else {
            int mx, my, mw, mh;
            build_mask_i8(proto_t, cd.coeff, cd.coeff_zp, cd.coeff_scale, box, ow, oh,
                          &mx, &my, &mw, &mh, &packed);

            *objs_json += ",\"m\":{\"x\":" + std::to_string(mx) +
                          ",\"y\":" + std::to_string(my) +
                          ",\"w\":" + std::to_string(mw) + ",\"h\":" + std::to_string(mh) +
                          ",\"d\":\"" + b64_encode(packed.data(), packed.size()) + "\"}}";
        }
        emitted++;
    }
    *objs_json += "]";
    *n_out = emitted;
    return 0;
}

int Engine::infer(const char* img_path, float conf, float nms, std::string* objs_json,
                  int* n_out, int* ow, int* oh, double* pre_ms, double* npu_ms,
                  double* post_ms, std::string* err)
{
    return infer_ex(img_path, NULL, 0, 0, 3, conf, nms, objs_json, n_out, ow, oh,
                    pre_ms, npu_ms, post_ms, err);
}

// img_path 走 stbi 解码（上传图，任意格式尺寸）；
// raw_rgb 非空时为内容带（rw×rh，已是模型分辨率），居中贴进 640×640
int Engine::infer_ex(const char* img_path, const uint8_t* raw_rgb, int rw, int rh,
                     int bpp, float conf, float nms, std::string* objs_json,
                     int* n_out, int* ow, int* oh, double* pre_ms, double* npu_ms,
                     double* post_ms, std::string* err)
{
    double t0 = get_time_ms();
    int w = 0, h = 0, c = 0;
    uint8_t* img;
    LetterBox lb;
    if (raw_rgb) {
        if (rw <= 0 || rh <= 0 || rw > model_w || rh > model_h) {
            *err = "raw 内容带尺寸非法";
            return -1;
        }
        if ((int)letterboxed.size() < (size_t)model_w * model_h * 3)
            letterboxed.resize((size_t)model_w * model_h * 3);
        paste_content(raw_rgb, rw, rh, bpp, letterboxed.data(), model_w, model_h, &lb);
        // 内容带即模型分辨率：坐标一律映射回 640×640 全幅空间（页面按 pad 换算）
        *ow = model_w;
        *oh = model_h;
        img = nullptr;
    } else {
        img = stbi_load(img_path, &w, &h, &c, 3);
        if (!img) {
            *err = std::string("图片解码失败: ") + img_path;
            return -1;
        }
        *ow = w;
        *oh = h;
        if ((int)letterboxed.size() < (size_t)model_w * model_h * 3)
            letterboxed.resize((size_t)model_w * model_h * 3);
        letterbox_rgb(img, w, h, 3, letterboxed.data(), model_w, model_h, &lb);
        stbi_image_free(img);
    }
    *pre_ms = get_time_ms() - t0;

    memcpy(inputs[0].mem->virt_addr, letterboxed.data(), (size_t)model_w * model_h * 3);
    int ret = rknn3_mem_sync(ctx, inputs[0].mem, RKNN3_MEMORY_SYNC_TO_DEVICE);
    if (ret != RKNN3_SUCCESS) {
        *err = "输入 mem_sync 失败";
        return ret;
    }

    double t1 = get_time_ms();
    ret = rknn3_run(ctx, inputs, io.n_input, outputs, io.n_output);
    *npu_ms = get_time_ms() - t1;
    if (ret < 0) {
        *err = "rknn3_run 失败 ret=" + std::to_string(ret);
        return ret;
    }
    for (int i = 0; i < io.n_output; i++)
        rknn3_mem_sync(ctx, outputs[i].mem, RKNN3_MEMORY_SYNC_FROM_DEVICE);

    double t2 = get_time_ms();
    ret = postprocess(lb, *ow, *oh, conf, nms, objs_json, n_out);
    *post_ms = get_time_ms() - t2;
    if (ret != 0) {
        *err = "后处理失败";
        return ret;
    }
    return 0;
}

// ── 迷你 JSON：扁平对象字段提取（协议自用）──────────────────────────
static std::string json_get_str(const std::string& s, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos)
        return "";
    p = s.find(':', p + pat.size());
    if (p == std::string::npos)
        return "";
    ++p;
    while (p < s.size() && s[p] == ' ')
        ++p;
    if (p >= s.size() || s[p] != '"')
        return "";
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size())
            ++p;
        out += s[p++];
    }
    return out;
}

static long json_get_int(const std::string& s, const char* key, long dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos)
        return dflt;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos)
        return dflt;
    return strtol(s.c_str() + p + 1, NULL, 10);
}

static double json_get_flt(const std::string& s, const char* key, double dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos)
        return dflt;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos)
        return dflt;
    return strtof(s.c_str() + p + 1, NULL);
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if ((unsigned char)ch < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", ch);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out;
}

// ── main ─────────────────────────────────────────────────────────────
static int run_cli(const char* model_dir, const char* task, const char* img_path)
{
    Engine eng;
    std::string err;
    if (eng.load(model_dir, task, &err) != 0) {
        fprintf(stderr, "[cli] load 失败: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[cli] 加载 %.0fms，输入 %dx%d\n", eng.load_ms, eng.model_w, eng.model_h);

    std::string objs;
    int n = 0, ow = 0, oh = 0;
    double pre, npu, post;
    if (eng.infer(img_path, BOX_THRESH, NMS_THRESH, &objs, &n, &ow, &oh, &pre, &npu,
                  &post, &err) != 0) {
        fprintf(stderr, "[cli] infer 失败: %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[cli] pre=%.1f npu=%.1f post=%.1f (ms)，%d 个目标，原图 %dx%d\n",
            pre, npu, post, n, ow, oh);
    fprintf(stderr, "[cli] JSON: %s\n", objs.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "--cli") == 0) {
        if (argc < 5) {
            fprintf(stderr, "用法: %s --cli <model_dir> <task> <img>\n", argv[0]);
            return 1;
        }
        return run_cli(argv[2], argv[3], argv[4]);
    }

    const char* model_dir = (argc > 1) ? argv[1] : kDefaultModelDir;
    const char* sock_path = (argc > 2) ? argv[2] : kDefaultSock;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[yolo26] model_dir=%s sock=%s\n", model_dir, sock_path);

    static Engine eng;
    static bool busy = false;

    ::unlink(sock_path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (::listen(fd, 4) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "[yolo26] listening on %s\n", sock_path);

    while (true) {
        int cli = ::accept(fd, nullptr, nullptr);
        if (cli < 0) {
            perror("accept");
            continue;
        }
        std::string buf;
        char tmp[4096];
        while (true) {
            pollfd p{cli, POLLIN, 0};
            int r = ::poll(&p, 1, 500);
            if (r < 0)
                break;
            if (r == 0)
                continue;
            int nn = ::recv(cli, tmp, sizeof(tmp) - 1, 0);
            if (nn <= 0)
                break;
            tmp[nn] = '\0';
            buf += tmp;

            while (true) {
                size_t pos = buf.find('\n');
                if (pos == std::string::npos)
                    break;
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);

                std::string cmd = json_get_str(line, "cmd");
                long qid = json_get_int(line, "qid", 0);
                fprintf(stderr, "[yolo26] cmd=%s qid=%ld\n", cmd.c_str(), qid);

                if (cmd == "ping") {
                    char resp[256];
                    snprintf(resp, sizeof(resp),
                             "{\"ev\":\"pong\",\"loaded\":%d,\"busy\":%d,\"task\":\"%s\","
                             "\"load_ms\":%.0f}\n",
                             eng.loaded ? 1 : 0, busy ? 1 : 0, eng.task.c_str(),
                             eng.load_ms);
                    ::send(cli, resp, strlen(resp), 0);
                } else if (cmd == "load") {
                    std::string task = json_get_str(line, "task");
                    if (task.empty())
                        task = kTasks[0].task;
                    if (busy) {
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"engine busy\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    if (eng.loaded && eng.task == task) {
                        std::string resp = "{\"ev\":\"done\",\"qid\":" + std::to_string(qid) +
                                           ",\"task\":\"" + task + "\",\"load_ms\":0}\n";
                        ::send(cli, resp.c_str(), resp.size(), 0);
                        continue;
                    }
                    busy = true;
                    std::string err;
                    int ret = eng.load(model_dir, task.c_str(), &err);
                    busy = false;
                    if (ret != 0) {
                        fprintf(stderr, "[yolo26] load failed: %s\n", err.c_str());
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"" + json_escape(err) + "\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    std::string resp =
                        "{\"ev\":\"done\",\"qid\":" + std::to_string(qid) +
                        ",\"task\":\"" + task + "\",\"res\":\"" +
                        std::to_string(eng.model_w) + "x" + std::to_string(eng.model_h) +
                        "\",\"load_ms\":" + std::to_string((long)eng.load_ms) + "}\n";
                    ::send(cli, resp.c_str(), resp.size(), 0);
                } else if (cmd == "infer") {
                    std::string img = json_get_str(line, "img");
                    float conf = (float)json_get_flt(line, "conf", BOX_THRESH);
                    float nms = (float)json_get_flt(line, "nms", NMS_THRESH);
                    if (img.empty()) {
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"img missing\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    if (busy) {
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"engine busy\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    busy = true;
                    std::string err;
                    // 没加载就先加载（兜底；正常路径 web 层会先发 load）
                    if (!eng.loaded) {
                        int ret = eng.load(model_dir, kTasks[0].task, &err);
                        if (ret != 0) {
                            busy = false;
                            std::string e = "{\"ev\":\"error\",\"qid\":" +
                                            std::to_string(qid) + ",\"msg\":\"" +
                                            json_escape(err) + "\"}\n";
                            ::send(cli, e.c_str(), e.size(), 0);
                            continue;
                        }
                    }
                    std::string objs;
                    int n_out = 0, ow = 0, oh = 0;
                    double pre = 0, npu = 0, post = 0;
                    double t0 = get_time_ms();
                    int ret;
                    std::string fmt = json_get_str(line, "fmt");
                    if (fmt == "rgb" || fmt == "rgba") {
                        // 摄像头环路：w×h 内容带裸数据（rgb=3 字节/px，rgba=4），
                        // 跳过 stbi，引擎端剥 alpha + 居中贴进 640×640
                        long rw = json_get_int(line, "w", eng.model_w);
                        long rh = json_get_int(line, "h", eng.model_h);
                        int bpp = (fmt == "rgba") ? 4 : 3;
                        if (rw <= 0 || rh <= 0 || rw > eng.model_w || rh > eng.model_h) {
                            busy = false;
                            std::string e = "{\"ev\":\"error\",\"qid\":" +
                                            std::to_string(qid) +
                                            ",\"msg\":\"raw 尺寸非法\"}\n";
                            ::send(cli, e.c_str(), e.size(), 0);
                            continue;
                        }
                        size_t need = (size_t)rw * rh * bpp;
                        FILE* f = fopen(img.c_str(), "rb");
                        if (!f) {
                            busy = false;
                            std::string e = "{\"ev\":\"error\",\"qid\":" +
                                            std::to_string(qid) +
                                            ",\"msg\":\"raw 文件读取失败\"}\n";
                            ::send(cli, e.c_str(), e.size(), 0);
                            continue;
                        }
                        eng.raw_buf.resize(need);
                        size_t got = fread(eng.raw_buf.data(), 1, need, f);
                        fclose(f);
                        if (got != need) {
                            busy = false;
                            std::string e = "{\"ev\":\"error\",\"qid\":" +
                                            std::to_string(qid) +
                                            ",\"msg\":\"raw 尺寸不符\"}\n";
                            ::send(cli, e.c_str(), e.size(), 0);
                            continue;
                        }
                        ret = eng.infer_ex(nullptr, eng.raw_buf.data(), (int)rw, (int)rh,
                                           bpp, conf, nms, &objs, &n_out, &ow, &oh,
                                           &pre, &npu, &post, &err);
                    } else {
                        ret = eng.infer(img.c_str(), conf, nms, &objs, &n_out, &ow,
                                        &oh, &pre, &npu, &post, &err);
                    }
                    double total = get_time_ms() - t0;
                    busy = false;
                    if (ret != 0) {
                        fprintf(stderr, "[yolo26] infer failed: %s\n", err.c_str());
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"" + json_escape(err) + "\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    std::string resp =
                        "{\"ev\":\"done\",\"qid\":" + std::to_string(qid) +
                        ",\"task\":\"" + eng.task + "\",\"w\":" + std::to_string(ow) +
                        ",\"h\":" + std::to_string(oh) + ",\"n\":" + std::to_string(n_out) +
                        ",\"ms\":{\"pre\":" + std::to_string((long)pre) +
                        ",\"npu\":" + std::to_string((long)npu) +
                        ",\"post\":" + std::to_string((long)post) +
                        ",\"total\":" + std::to_string((long)total) +
                        "},\"objs\":" + objs + "}\n";
                    ::send(cli, resp.c_str(), resp.size(), 0);
                } else if (cmd == "unload") {
                    if (!busy && eng.loaded) {
                        eng.release();
                        const char* r = "{\"ev\":\"done\",\"qid\":0}\n";
                        ::send(cli, r, strlen(r), 0);
                    }
                }
            }
        }
        ::close(cli);
    }

    ::close(fd);
    ::unlink(sock_path);
    return 0;
}
