// depth_engine.cc — Depth-Anything-V3 常驻推理服务
// 移植官方 rknn3-model-zoo examples/depth_anything_v3/cpp/main.cpp 的三段管线
//（local → global → head + 共享内部内存 + turbo 着色），
// 外壳换成 tts_demo 同款 Unix-socket 行 JSON 服务。
//
// 命令行: ./depth_engine <model_dir> [sock_path]
//   model_dir: 含 da3_base_{local,global,head}.{rknn,weight} 的目录
//   sock_path: Unix socket 路径（默认 /tmp/depth_engine.sock）
//
// 协议（一行一个 JSON）:
//   {"cmd":"ping"}
//     → {"ev":"pong","loaded":1,"busy":0,"views":1,"res":"280x280","load_ms":0}
//   {"cmd":"load","qid":N}
//     → {"ev":"done","qid":N,"views":V,"res":"WxH","load_ms":M} 或 {"ev":"error",...}
//   {"cmd":"infer","qid":N,"img":"<jpg路径>","out":"<png输出路径>"}
//     → {"ev":"done","qid":N,"png":"...","ms":{...},"dmin":..,"dmax":..} 或 {"ev":"error",...}
//       注意：DA3-BASE 输出是相对深度，dmin/dmax 无米制量纲，仅用于调试
//
// 单图 demo：不管模型导出时固定了几个 view（V 从 global 输入 shape 读出），
// local 只跑一次（同输入同输出），token 直接复制到 V 个槽位再进 global。

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "float16.h"
#include "rknn3_api.h"

static const char* kDefaultModelDir = "model";   // relative default; depth_engine.py passes model_dir explicitly
static const char* kDefaultSock = "/tmp/depth_engine.sock";

// ── 时间 ────────────────────────────────────────────────────────────
static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// ── 模型上下文（照抄官方 da3_model_ctx_t）───────────────────────────
typedef struct {
    rknn3_context ctx;
    uint32_t core_num;
    uint32_t core_mask;
    rknn3_input_output_num io_num;
    rknn3_tensor_attr* input_attrs;
    rknn3_tensor_attr* output_attrs;
    rknn3_tensor* inputs;
    rknn3_tensor* outputs;
} ModelCtx;

static size_t get_dtype_size(rknn3_tensor_type dtype)
{
    switch (dtype) {
    case RKNN3_TENSOR_FLOAT32:
    case RKNN3_TENSOR_INT32:
    case RKNN3_TENSOR_UINT32:
        return 4;
    case RKNN3_TENSOR_FLOAT16:
    case RKNN3_TENSOR_INT16:
    case RKNN3_TENSOR_UINT16:
    case RKNN3_TENSOR_BFLOAT16:
        return 2;
    case RKNN3_TENSOR_INT8:
    case RKNN3_TENSOR_UINT8:
    case RKNN3_TENSOR_BOOL:
        return 1;
    case RKNN3_TENSOR_INT64:
    case RKNN3_TENSOR_UINT64:
        return 8;
    default:
        return 0;
    }
}

static size_t get_tensor_bytes(const rknn3_tensor_attr* attr)
{
    return (size_t)attr->n_elems * get_dtype_size(attr->dtype);
}

static void print_tensor_attr(const rknn3_tensor_attr* attr)
{
    uint32_t i;
    printf("[depth] Tensor: index=%u name=%s dims=[", attr->index, attr->name);
    for (i = 0; i < attr->n_dims; ++i)
        printf("%u%s", attr->shape[i], i + 1 == attr->n_dims ? "" : ",");
    printf("] n_elems=%u dtype=%s layout=%s\n", attr->n_elems,
           rknn3_get_type_string(attr->dtype), rknn3_get_layout_string(attr->layout));
}

static char* make_weight_path(const char* model_path)
{
    const char* suffix = ".rknn";
    size_t path_len = strlen(model_path);
    size_t suffix_len = strlen(suffix);
    char* weight_path = (char*)malloc(path_len + 8);
    if (weight_path == NULL)
        return NULL;
    strcpy(weight_path, model_path);
    if (path_len >= suffix_len && strcmp(model_path + path_len - suffix_len, suffix) == 0)
        strcpy(weight_path + path_len - suffix_len, ".weight");
    else
        strcat(weight_path, ".weight");
    return weight_path;
}

static void model_release(ModelCtx* model)
{
    uint32_t i;
    if (model == NULL)
        return;
    if (model->ctx != 0) {
        for (i = 0; i < model->io_num.n_input; ++i)
            if (model->inputs != NULL && model->inputs[i].mem != NULL)
                rknn3_destroy_mem(model->ctx, model->inputs[i].mem);
        for (i = 0; i < model->io_num.n_output; ++i)
            if (model->outputs != NULL && model->outputs[i].mem != NULL)
                rknn3_destroy_mem(model->ctx, model->outputs[i].mem);
        rknn3_destroy(model->ctx);
    }
    free(model->input_attrs);
    free(model->output_attrs);
    free(model->inputs);
    free(model->outputs);
    memset(model, 0, sizeof(*model));
}

static int model_init(ModelCtx* model, const char* model_path)
{
    rknn3_config config;
    uint32_t i;
    char* weight_path;
    int ret;

    memset(model, 0, sizeof(*model));
    weight_path = make_weight_path(model_path);
    if (weight_path == NULL)
        return -1;

    ret = rknn3_init(&model->ctx, NULL);
    if (ret != RKNN3_SUCCESS) {
        printf("[depth] rknn3_init failed ret=%d\n", ret);
        goto fail;
    }
    ret = rknn3_load_model_from_path(model->ctx, model_path, weight_path);
    if (ret != RKNN3_SUCCESS) {
        printf("[depth] load_model failed ret=%d model=%s\n", ret, model_path);
        goto fail;
    }
    ret = rknn3_query(model->ctx, RKNN3_QUERY_CORE_NUMBER, &model->core_num, sizeof(model->core_num));
    if (ret != RKNN3_SUCCESS || model->core_num == 0 || model->core_num > 32) {
        printf("[depth] QUERY_CORE_NUMBER failed ret=%d core_num=%u\n", ret, model->core_num);
        if (ret == RKNN3_SUCCESS)
            ret = -1;
        goto fail;
    }
    model->core_mask =
        model->core_num == 32 ? UINT32_MAX : (uint32_t)((1ULL << model->core_num) - 1ULL);
    printf("[depth] model=%s core_num=%u core_mask=0x%x\n", model_path, model->core_num,
           model->core_mask);

    memset(&config, 0, sizeof(config));
    config.run_core_mask = model->core_mask;
    config.user_mem_internal = 1;  // 三模型共享内部内存，省 NPU 内存
    ret = rknn3_model_init(model->ctx, &config);
    if (ret != RKNN3_SUCCESS) {
        printf("[depth] model_init failed ret=%d model=%s\n", ret, model_path);
        goto fail;
    }
    ret = rknn3_query(model->ctx, RKNN3_QUERY_IN_OUT_NUM, &model->io_num, sizeof(model->io_num));
    if (ret != RKNN3_SUCCESS) {
        printf("[depth] QUERY_IN_OUT_NUM failed ret=%d\n", ret);
        goto fail;
    }

    model->input_attrs = (rknn3_tensor_attr*)calloc(model->io_num.n_input, sizeof(*model->input_attrs));
    model->output_attrs = (rknn3_tensor_attr*)calloc(model->io_num.n_output, sizeof(*model->output_attrs));
    model->inputs = (rknn3_tensor*)calloc(model->io_num.n_input, sizeof(*model->inputs));
    model->outputs = (rknn3_tensor*)calloc(model->io_num.n_output, sizeof(*model->outputs));
    if (model->input_attrs == NULL || model->output_attrs == NULL || model->inputs == NULL ||
        model->outputs == NULL) {
        ret = -1;
        goto fail;
    }

    printf("[depth] model=%s n_input=%u n_output=%u\n", model_path, model->io_num.n_input,
           model->io_num.n_output);
    for (i = 0; i < model->io_num.n_input; ++i) {
        model->input_attrs[i].index = i;
        ret = rknn3_query(model->ctx, RKNN3_QUERY_INPUT_ATTR, &model->input_attrs[i],
                          sizeof(model->input_attrs[i]));
        if (ret != RKNN3_SUCCESS) {
            printf("[depth] QUERY_INPUT_ATTR[%u] failed ret=%d\n", i, ret);
            goto fail;
        }
        print_tensor_attr(&model->input_attrs[i]);
        model->inputs[i].attr = &model->input_attrs[i];
        model->inputs[i].mem = rknn3_create_mem(model->ctx, model->input_attrs[i].aligned_size,
                                                model->input_attrs[i].core_id,
                                                RKNN3_FLAG_MEMORY_CACHEABLE);
        if (model->inputs[i].mem == NULL) {
            printf("[depth] create_mem input[%u] failed\n", i);
            ret = -1;
            goto fail;
        }
    }
    for (i = 0; i < model->io_num.n_output; ++i) {
        model->output_attrs[i].index = i;
        ret = rknn3_query(model->ctx, RKNN3_QUERY_OUTPUT_ATTR, &model->output_attrs[i],
                          sizeof(model->output_attrs[i]));
        if (ret != RKNN3_SUCCESS) {
            printf("[depth] QUERY_OUTPUT_ATTR[%u] failed ret=%d\n", i, ret);
            goto fail;
        }
        print_tensor_attr(&model->output_attrs[i]);
        model->outputs[i].attr = &model->output_attrs[i];
        model->outputs[i].mem = rknn3_create_mem(model->ctx, model->output_attrs[i].aligned_size,
                                                 model->output_attrs[i].core_id,
                                                 RKNN3_FLAG_MEMORY_CACHEABLE);
        if (model->outputs[i].mem == NULL) {
            printf("[depth] create_mem output[%u] failed\n", i);
            ret = -1;
            goto fail;
        }
    }
    free(weight_path);
    return RKNN3_SUCCESS;

fail:
    free(weight_path);
    model_release(model);
    return ret;
}

// ── 三模型共享内部内存（照抄官方 da3_internal_mem_pool_t）────────────
typedef struct {
    rknn3_context owner_ctx;
    uint32_t n_mems;
    rknn3_tensor_mem** mems;
} InternalMemPool;

static void internal_pool_release(InternalMemPool* pool)
{
    uint32_t i;
    if (pool == NULL)
        return;
    for (i = 0; i < pool->n_mems; ++i)
        if (pool->mems != NULL && pool->mems[i] != NULL)
            rknn3_destroy_mem(pool->owner_ctx, pool->mems[i]);
    free(pool->mems);
    memset(pool, 0, sizeof(*pool));
}

static int internal_pool_share(ModelCtx** models, uint32_t n_models, InternalMemPool* pool)
{
    rknn3_core_mem_size** model_sizes = NULL;
    rknn3_tensor_mem*** bindings = NULL;
    rknn3_tensor_mem* mem_by_core[32] = {0};
    uint64_t max_size_by_core[32] = {0};
    uint32_t model_index, core_index, core_id, pool_index = 0;
    int ret = -1;

    memset(pool, 0, sizeof(*pool));
    model_sizes = (rknn3_core_mem_size**)calloc(n_models, sizeof(*model_sizes));
    bindings = (rknn3_tensor_mem***)calloc(n_models, sizeof(*bindings));
    if (model_sizes == NULL || bindings == NULL)
        goto out;

    for (model_index = 0; model_index < n_models; ++model_index) {
        ModelCtx* model = models[model_index];
        model_sizes[model_index] =
            (rknn3_core_mem_size*)calloc(model->core_num, sizeof(*model_sizes[model_index]));
        bindings[model_index] =
            (rknn3_tensor_mem**)calloc(model->core_num, sizeof(*bindings[model_index]));
        if (model_sizes[model_index] == NULL || bindings[model_index] == NULL)
            goto out;
        ret = rknn3_query(model->ctx, RKNN3_QUERY_CORE_MEM_SIZE, model_sizes[model_index],
                          sizeof(*model_sizes[model_index]) * model->core_num);
        if (ret != RKNN3_SUCCESS) {
            printf("[depth] QUERY_CORE_MEM_SIZE model[%u] failed ret=%d\n", model_index, ret);
            goto out;
        }
        for (core_index = 0; core_index < model->core_num; ++core_index) {
            core_id = model_sizes[model_index][core_index].core_id;
            if (core_id >= 32) {
                printf("[depth] invalid internal memory core_id=%u\n", core_id);
                ret = -1;
                goto out;
            }
            if (model_sizes[model_index][core_index].internal_size > max_size_by_core[core_id])
                max_size_by_core[core_id] = model_sizes[model_index][core_index].internal_size;
        }
    }

    for (core_id = 0; core_id < 32; ++core_id)
        if (max_size_by_core[core_id] != 0)
            ++pool->n_mems;
    pool->owner_ctx = models[0]->ctx;
    pool->mems = (rknn3_tensor_mem**)calloc(pool->n_mems, sizeof(*pool->mems));
    if (pool->mems == NULL) {
        ret = -1;
        goto out;
    }
    for (core_id = 0; core_id < 32; ++core_id) {
        if (max_size_by_core[core_id] == 0)
            continue;
        mem_by_core[core_id] = rknn3_create_mem(pool->owner_ctx, max_size_by_core[core_id], core_id,
                                                RKNN3_FLAG_MEMORY_CACHEABLE);
        if (mem_by_core[core_id] == NULL) {
            printf("[depth] create shared internal mem core=%u failed\n", core_id);
            ret = -1;
            goto out;
        }
        pool->mems[pool_index++] = mem_by_core[core_id];
        printf("[depth] shared internal memory core=%u size=%llu\n", core_id,
               (unsigned long long)max_size_by_core[core_id]);
    }

    for (model_index = 0; model_index < n_models; ++model_index) {
        ModelCtx* model = models[model_index];
        for (core_index = 0; core_index < model->core_num; ++core_index) {
            core_id = model_sizes[model_index][core_index].core_id;
            bindings[model_index][core_index] = mem_by_core[core_id];
        }
        ret = rknn3_set_internal_mem(model->ctx, bindings[model_index], model->core_num);
        if (ret != RKNN3_SUCCESS) {
            printf("[depth] set_internal_mem model[%u] failed ret=%d\n", model_index, ret);
            goto out;
        }
    }
    ret = RKNN3_SUCCESS;

out:
    if (model_sizes != NULL)
        for (model_index = 0; model_index < n_models; ++model_index)
            free(model_sizes[model_index]);
    if (bindings != NULL)
        for (model_index = 0; model_index < n_models; ++model_index)
            free(bindings[model_index]);
    free(model_sizes);
    free(bindings);
    if (ret != RKNN3_SUCCESS)
        internal_pool_release(pool);
    return ret;
}

// ── 推理执行（照抄官方 da3_model_run，含计时）────────────────────────
typedef struct {
    const void* data;
    rknn3_tensor_type dtype;
    size_t n_elems;
} HostInput;

static int copy_input_to_tensor(const HostInput* input, const rknn3_tensor_attr* attr,
                                rknn3_tensor_mem* mem)
{
    size_t bytes;
    uint32_t i;

    if (input->data == NULL || mem == NULL || mem->virt_addr == NULL ||
        input->n_elems != attr->n_elems) {
        printf("[depth] invalid input elems got=%zu expected=%u\n", input->n_elems, attr->n_elems);
        return -1;
    }
    bytes = get_tensor_bytes(attr);
    if (bytes == 0 || bytes > mem->size) {
        printf("[depth] invalid input bytes=%zu mem_size=%llu\n", bytes,
               (unsigned long long)mem->size);
        return -1;
    }
    if (input->dtype == attr->dtype) {
        memcpy(mem->virt_addr, input->data, bytes);
        return RKNN3_SUCCESS;
    }
    if (input->dtype == RKNN3_TENSOR_FLOAT32 && attr->dtype == RKNN3_TENSOR_FLOAT16) {
        const float* src = (const float*)input->data;
        float16* dst = (float16*)mem->virt_addr;
        for (i = 0; i < attr->n_elems; ++i)
            dst[i] = fp32_to_fp16(src[i]);
        return RKNN3_SUCCESS;
    }
    if (input->dtype == RKNN3_TENSOR_FLOAT16 && attr->dtype == RKNN3_TENSOR_FLOAT32) {
        const float16* src = (const float16*)input->data;
        float* dst = (float*)mem->virt_addr;
        for (i = 0; i < attr->n_elems; ++i)
            dst[i] = fp16_to_fp32(src[i]);
        return RKNN3_SUCCESS;
    }
    printf("[depth] unsupported input dtype conversion\n");
    return -1;
}

static int model_run(ModelCtx* model, const HostInput* host_inputs, uint32_t n_inputs,
                     double* copy_ms, double* exec_ms)
{
    uint32_t i;
    int ret;
    double t0;

    if (n_inputs != model->io_num.n_input) {
        printf("[depth] input count mismatch got=%u expected=%u\n", n_inputs,
               model->io_num.n_input);
        return -1;
    }
    for (i = 0; i < n_inputs; ++i) {
        t0 = get_time_ms();
        ret = copy_input_to_tensor(&host_inputs[i], &model->input_attrs[i], model->inputs[i].mem);
        if (ret != RKNN3_SUCCESS)
            return ret;
        *copy_ms += get_time_ms() - t0;

        ret = rknn3_mem_sync(model->ctx, model->inputs[i].mem, RKNN3_MEMORY_SYNC_TO_DEVICE);
        if (ret != RKNN3_SUCCESS) {
            printf("[depth] mem_sync input[%u] failed ret=%d\n", i, ret);
            return ret;
        }
    }

    t0 = get_time_ms();
    ret = rknn3_run(model->ctx, model->inputs, model->io_num.n_input, model->outputs,
                    model->io_num.n_output);
    *exec_ms += get_time_ms() - t0;
    if (ret != RKNN3_SUCCESS) {
        printf("[depth] rknn3_run failed ret=%d\n", ret);
        return ret;
    }

    for (i = 0; i < model->io_num.n_output; ++i) {
        ret = rknn3_mem_sync(model->ctx, model->outputs[i].mem, RKNN3_MEMORY_SYNC_FROM_DEVICE);
        if (ret != RKNN3_SUCCESS) {
            printf("[depth] mem_sync output[%u] failed ret=%d\n", i, ret);
            return ret;
        }
    }
    return RKNN3_SUCCESS;
}

static int copy_output_to_fp32(const ModelCtx* model, uint32_t index, float* dst, size_t dst_elems)
{
    const rknn3_tensor_attr* attr;
    const void* src;
    uint32_t i;

    if (index >= model->io_num.n_output || dst == NULL)
        return -1;
    attr = &model->output_attrs[index];
    src = model->outputs[index].mem->virt_addr;
    if (src == NULL || dst_elems != attr->n_elems)
        return -1;
    if (attr->dtype == RKNN3_TENSOR_FLOAT32) {
        memcpy(dst, src, dst_elems * sizeof(float));
        return RKNN3_SUCCESS;
    }
    if (attr->dtype == RKNN3_TENSOR_FLOAT16) {
        const float16* src_fp16 = (const float16*)src;
        for (i = 0; i < attr->n_elems; ++i)
            dst[i] = fp16_to_fp32(src_fp16[i]);
        return RKNN3_SUCCESS;
    }
    printf("[depth] unsupported output dtype\n");
    return -1;
}

static int copy_output_to_host(const ModelCtx* model, uint32_t index, void** dst, size_t* bytes)
{
    size_t output_bytes;
    void* output;

    if (index >= model->io_num.n_output || dst == NULL || bytes == NULL)
        return -1;
    output_bytes = get_tensor_bytes(&model->output_attrs[index]);
    if (output_bytes == 0 || output_bytes > model->outputs[index].mem->size)
        return -1;
    output = malloc(output_bytes);
    if (output == NULL)
        return -1;
    memcpy(output, model->outputs[index].mem->virt_addr, output_bytes);
    *dst = output;
    *bytes = output_bytes;
    return RKNN3_SUCCESS;
}

// ── 图像预处理：JPEG 解码 + 双线性缩放到模型分辨率（替代 RGA）──────
static void resize_rgb_bilinear(const uint8_t* src, int src_w, int src_h, uint8_t* dst, int dst_w,
                                int dst_h)
{
    int x, y;
    // 累加分布（区域平均，抗锯齿；宽度比不是整数倍时比简单双线性更稳）
    for (y = 0; y < dst_h; ++y) {
        double sy0 = (double)y * src_h / dst_h;
        double sy1 = (double)(y + 1) * src_h / dst_h;
        int iy0 = (int)sy0;
        int iy1 = (int)ceil(sy1);
        if (iy1 > src_h)
            iy1 = src_h;
        for (x = 0; x < dst_w; ++x) {
            double sx0 = (double)x * src_w / dst_w;
            double sx1 = (double)(x + 1) * src_w / dst_w;
            int ix0 = (int)sx0;
            int ix1 = (int)ceil(sx1);
            if (ix1 > src_w)
                ix1 = src_w;
            int r = 0, g = 0, b = 0, n = 0, xx, yy;
            for (yy = iy0; yy < iy1; ++yy) {
                for (xx = ix0; xx < ix1; ++xx) {
                    const uint8_t* p = src + ((size_t)yy * src_w + xx) * 3;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++n;
                }
            }
            if (n == 0) {  // 放大场景：落到源图单像素
                const uint8_t* p =
                    src + ((size_t)(iy0 < src_h ? iy0 : src_h - 1) * src_w +
                           (ix0 < src_w ? ix0 : src_w - 1)) * 3;
                dst[((size_t)y * dst_w + x) * 3 + 0] = p[0];
                dst[((size_t)y * dst_w + x) * 3 + 1] = p[1];
                dst[((size_t)y * dst_w + x) * 3 + 2] = p[2];
            } else {
                dst[((size_t)y * dst_w + x) * 3 + 0] = (uint8_t)((r + n / 2) / n);
                dst[((size_t)y * dst_w + x) * 3 + 1] = (uint8_t)((g + n / 2) / n);
                dst[((size_t)y * dst_w + x) * 3 + 2] = (uint8_t)((b + n / 2) / n);
            }
        }
    }
}

// ── 深度可视化（照抄官方：有效区裁剪 + 逆深度 2%~98% 分位 + turbo）──
static void turbo_color(float value, uint8_t* rgb)
{
    float x, x2, x3, x4, x5, red, green, blue;

    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;
    x = value;
    x2 = x * x;
    x3 = x2 * x;
    x4 = x3 * x;
    x5 = x4 * x;
    red = 0.13572138f + 4.61539260f * x - 42.66032258f * x2 + 132.13108234f * x3 -
          152.94239396f * x4 + 59.28637943f * x5;
    green = 0.09140261f + 2.19418839f * x + 4.84296658f * x2 - 14.18503333f * x3 +
            4.27729857f * x4 + 2.82956604f * x5;
    blue = 0.10667330f + 12.64194608f * x - 60.58204836f * x2 + 110.36276771f * x3 -
           89.90310912f * x4 + 27.34824973f * x5;
    red = fminf(fmaxf(red, 0.0f), 1.0f);
    green = fminf(fmaxf(green, 0.0f), 1.0f);
    blue = fminf(fmaxf(blue, 0.0f), 1.0f);
    rgb[0] = (uint8_t)(red * 255.0f + 0.5f);
    rgb[1] = (uint8_t)(green * 255.0f + 0.5f);
    rgb[2] = (uint8_t)(blue * 255.0f + 0.5f);
}

static void build_valid_mask(const uint8_t* rgb, uint32_t height, uint32_t width, uint8_t* mask)
{
    const uint32_t dark_threshold = 16;
    const uint32_t min_column_pixels = height / 20 > 0 ? height / 20 : 1;
    const uint32_t min_row_pixels = width / 20 > 0 ? width / 20 : 1;
    uint32_t left = 0, right = width, top = 0, bottom = height, x, y;

    while (left < right) {
        uint32_t non_black = 0;
        for (y = 0; y < height; ++y) {
            const uint8_t* pixel = rgb + ((size_t)y * width + left) * 3;
            if (pixel[0] > dark_threshold || pixel[1] > dark_threshold ||
                pixel[2] > dark_threshold)
                ++non_black;
        }
        if (non_black >= min_column_pixels)
            break;
        ++left;
    }
    while (right > left) {
        uint32_t non_black = 0;
        x = right - 1;
        for (y = 0; y < height; ++y) {
            const uint8_t* pixel = rgb + ((size_t)y * width + x) * 3;
            if (pixel[0] > dark_threshold || pixel[1] > dark_threshold ||
                pixel[2] > dark_threshold)
                ++non_black;
        }
        if (non_black >= min_row_pixels)
            break;
        --right;
    }
    while (top < bottom) {
        uint32_t non_black = 0;
        for (x = 0; x < width; ++x) {
            const uint8_t* pixel = rgb + ((size_t)top * width + x) * 3;
            if (pixel[0] > dark_threshold || pixel[1] > dark_threshold ||
                pixel[2] > dark_threshold)
                ++non_black;
        }
        if (non_black >= min_row_pixels)
            break;
        ++top;
    }
    while (bottom > top) {
        uint32_t non_black = 0;
        y = bottom - 1;
        for (x = 0; x < width; ++x) {
            const uint8_t* pixel = rgb + ((size_t)y * width + x) * 3;
            if (pixel[0] > dark_threshold || pixel[1] > dark_threshold ||
                pixel[2] > dark_threshold)
                ++non_black;
        }
        if (non_black >= min_row_pixels)
            break;
        --bottom;
    }

    if (right - left < width / 2 || bottom - top < height / 2) {
        left = 0;
        right = width;
        top = 0;
        bottom = height;
    }
    memset(mask, 0, (size_t)height * width);
    for (y = top; y < bottom; ++y)
        memset(mask + (size_t)y * width + left, 1, right - left);
}

static int compare_float(const void* lhs, const void* rhs)
{
    float a = *(const float*)lhs;
    float b = *(const float*)rhs;
    return (a > b) - (a < b);
}

// view0 深度 → turbo PNG；顺带输出有效深度的 min/max（米）
static int save_depth_png(const char* out_path, const float* depth, const uint8_t* mask,
                          uint32_t height, uint32_t width, float* dmin_out, float* dmax_out)
{
    const size_t pixels = (size_t)height * width;
    float* valid_values = (float*)malloc(pixels * sizeof(float));
    uint8_t* rgb = (uint8_t*)malloc(pixels * 3);
    size_t valid_count = 0, i;
    float lower, upper, dmin = FLT_MAX, dmax = -FLT_MAX;
    int ret = -1;

    if (valid_values == NULL || rgb == NULL)
        goto out;

    for (i = 0; i < pixels; ++i) {
        if (mask[i] && depth[i] > 0.0f && isfinite(depth[i])) {
            valid_values[valid_count++] = 1.0f / depth[i];
            if (depth[i] < dmin)
                dmin = depth[i];
            if (depth[i] > dmax)
                dmax = depth[i];
        }
    }
    if (valid_count <= 10) {
        printf("[depth] too few valid pixels\n");
        goto out;
    }
    qsort(valid_values, valid_count, sizeof(float), compare_float);
    lower = valid_values[(size_t)(0.02 * (valid_count - 1))];
    upper = valid_values[(size_t)(0.98 * (valid_count - 1))];
    if (upper <= lower)
        upper = lower + 1e-6f;

    for (i = 0; i < pixels; ++i) {
        if (mask[i] && depth[i] > 0.0f && isfinite(depth[i])) {
            float inverse_depth = 1.0f / depth[i];
            float normalized = (inverse_depth - lower) / (upper - lower);
            turbo_color(normalized, rgb + i * 3);
        } else {
            memset(rgb + i * 3, 0, 3);
        }
    }
    if (!stbi_write_png(out_path, (int)width, (int)height, 3, rgb, (int)width * 3)) {
        printf("[depth] write png failed path=%s\n", out_path);
        goto out;
    }
    *dmin_out = dmin;
    *dmax_out = dmax;
    ret = 0;

out:
    free(valid_values);
    free(rgb);
    return ret;
}

// ── 推理器：持有三模型 + 管线状态 ────────────────────────────────────
struct Pipeline {
    ModelCtx local_m, global_m, head_m;
    InternalMemPool pool;
    bool loaded = false;
    int views = 0;
    uint32_t img_h = 0, img_w = 0;
    uint32_t tokens_per_view = 0, hidden = 0;
    uint32_t feature_tokens = 0, feature_hidden = 0;
    uint32_t feature_count = 0;
    size_t local_in_elems = 0, local_out_elems = 0;
    size_t global_in_elems = 0, head_in_elems = 0, out_elems = 0;
    // 常驻工作缓冲
    uint8_t* frame_rgb = NULL;    // img_h*img_w*3 模型分辨率
    uint8_t* valid_mask = NULL;   // img_h*img_w
    float* local_out = NULL;      // [tokens,hidden]
    float* tokens = NULL;         // [views*tokens,hidden]
    void** feats = NULL;          // feature_count 路 fp16 原始 buffer
    float* depth = NULL;
    float* conf = NULL;
    double load_ms = 0;

    ~Pipeline() { unload(); }

    void unload()
    {
        uint32_t i;
        model_release(&head_m);
        model_release(&global_m);
        internal_pool_release(&pool);
        model_release(&local_m);
        free(frame_rgb);
        free(valid_mask);
        free(local_out);
        free(tokens);
        if (feats != NULL)
            for (i = 0; i < feature_count; ++i)
                free(feats[i]);
        free(feats);
        free(depth);
        free(conf);
        frame_rgb = NULL;
        valid_mask = NULL;
        local_out = NULL;
        tokens = NULL;
        feats = NULL;
        depth = conf = NULL;
        loaded = false;
        views = 0;
    }

    // 加载三模型 + 共享内部内存 + 校验接口 + 分配工作缓冲
    int load(const char* model_dir, std::string* err)
    {
        char path[512];
        double t0 = get_time_ms();
        int ret;

        if (loaded)
            return 0;

        snprintf(path, sizeof(path), "%s/da3_base_local.rknn", model_dir);
        ret = model_init(&local_m, path);
        if (ret != RKNN3_SUCCESS) {
            *err = "local 模型加载失败（检查 model 目录）";
            goto fail;
        }
        snprintf(path, sizeof(path), "%s/da3_base_global.rknn", model_dir);
        ret = model_init(&global_m, path);
        if (ret != RKNN3_SUCCESS) {
            *err = "global 模型加载失败";
            goto fail;
        }
        snprintf(path, sizeof(path), "%s/da3_base_head.rknn", model_dir);
        ret = model_init(&head_m, path);
        if (ret != RKNN3_SUCCESS) {
            *err = "head 模型加载失败";
            goto fail;
        }

        {
            ModelCtx* models[] = {&local_m, &global_m, &head_m};
            ret = internal_pool_share(models, 3, &pool);
            if (ret != RKNN3_SUCCESS) {
                *err = "共享内部内存初始化失败";
                goto fail;
            }
        }

        // 接口校验（同官方 main.cpp）
        if (local_m.io_num.n_input != 1 || local_m.io_num.n_output != 1 ||
            local_m.input_attrs[0].n_dims != 4 || local_m.input_attrs[0].shape[0] != 1 ||
            local_m.input_attrs[0].layout != RKNN3_TENSOR_NHWC ||
            local_m.input_attrs[0].dtype != RKNN3_TENSOR_UINT8 ||
            local_m.output_attrs[0].n_dims != 3 || local_m.output_attrs[0].shape[0] != 1) {
            *err = "local 接口不符（需 UINT8 NHWC [1,H,W,3] → [1,L,D]）";
            ret = -1;
            goto fail;
        }
        img_h = local_m.input_attrs[0].shape[1];
        img_w = local_m.input_attrs[0].shape[2];
        if (local_m.input_attrs[0].shape[3] != 3) {
            *err = "local 输入通道数不是 3";
            ret = -1;
            goto fail;
        }
        tokens_per_view = local_m.output_attrs[0].shape[1];
        hidden = local_m.output_attrs[0].shape[2];
        local_in_elems = local_m.input_attrs[0].n_elems;
        local_out_elems = local_m.output_attrs[0].n_elems;

        if (global_m.io_num.n_input != 1 || global_m.input_attrs[0].n_dims != 4 ||
            global_m.input_attrs[0].shape[1] != tokens_per_view ||
            global_m.input_attrs[0].shape[2] != 1 ||
            global_m.input_attrs[0].shape[3] != hidden ||
            global_m.input_attrs[0].n_dims != 4) {
            *err = "global 输入接口与 local 输出不匹配";
            ret = -1;
            goto fail;
        }
        views = (int)global_m.input_attrs[0].shape[0];
        global_in_elems = global_m.input_attrs[0].n_elems;
        if (views < 1 || views > 10) {
            *err = "global view 数异常（" + std::to_string(views) + "）";
            ret = -1;
            goto fail;
        }
        feature_count = global_m.io_num.n_output;
        if (feature_count == 0 || global_m.output_attrs[0].n_dims != 4 ||
            global_m.output_attrs[0].shape[0] != (uint32_t)views ||
            global_m.output_attrs[0].shape[2] != 1) {
            *err = "global 输出接口异常";
            ret = -1;
            goto fail;
        }
        feature_tokens = global_m.output_attrs[0].shape[1];
        feature_hidden = global_m.output_attrs[0].shape[3];
        head_in_elems = global_m.output_attrs[0].n_elems;

        if (head_m.io_num.n_input != feature_count || head_m.io_num.n_output != 2) {
            *err = "head 输入/输出数量与 global 不匹配";
            ret = -1;
            goto fail;
        }
        if (head_m.output_attrs[0].n_dims != 4 || head_m.output_attrs[0].shape[0] != 1 ||
            head_m.output_attrs[0].shape[1] != (uint32_t)views ||
            head_m.output_attrs[0].shape[2] != img_h || head_m.output_attrs[0].shape[3] != img_w) {
            *err = "head 输出需为 [1,V,H,W]";
            ret = -1;
            goto fail;
        }
        out_elems = head_m.output_attrs[0].n_elems;

        // 工作缓冲
        frame_rgb = (uint8_t*)malloc(local_in_elems);
        valid_mask = (uint8_t*)malloc((size_t)img_h * img_w);
        local_out = (float*)malloc(local_out_elems * sizeof(float));
        tokens = (float*)malloc(global_in_elems * sizeof(float));
        feats = (void**)calloc(feature_count, sizeof(*feats));
        depth = (float*)malloc(out_elems * sizeof(float));
        conf = (float*)malloc(out_elems * sizeof(float));
        if (frame_rgb == NULL || valid_mask == NULL || local_out == NULL || tokens == NULL ||
            feats == NULL || depth == NULL || conf == NULL) {
            *err = "工作缓冲分配失败";
            ret = -1;
            goto fail;
        }

        load_ms = get_time_ms() - t0;
        loaded = true;
        printf("[depth] 管线就绪 views=%d res=%ux%u tokens=%u hidden=%u feats=%u load=%.0fms\n",
               views, img_w, img_h, tokens_per_view, hidden, feature_count, load_ms);
        if (views > 1)
            printf("[depth] 注：模型固定 %d view，单图 demo 复用同一帧的 token\n", views);
        return 0;

    fail:
        unload();
        return ret;
    }

    // img_path(JPEG/PNG) → out_path(PNG)；返回各段耗时
    int infer(const char* img_path, const char* out_path, std::string* err,
              double* pre_ms, double* local_ms, double* global_ms, double* head_ms,
              double* post_ms, float* dmin, float* dmax)
    {
        double t0;
        int ret;
        uint32_t i;
        int w, h, c;

        *pre_ms = *local_ms = *global_ms = *head_ms = *post_ms = 0;

        if (!loaded) {
            *err = "模型未加载";
            return -1;
        }

        // 1. 解码 + 缩放
        t0 = get_time_ms();
        {
            uint8_t* img = stbi_load(img_path, &w, &h, &c, 3);
            if (img == NULL) {
                *err = std::string("图片解码失败: ") + stbi_failure_reason();
                return -1;
            }
            if (w < 8 || h < 8) {
                stbi_image_free(img);
                *err = "图片太小（<8px）";
                return -1;
            }
            resize_rgb_bilinear(img, w, h, frame_rgb, (int)img_w, (int)img_h);
            stbi_image_free(img);
        }
        build_valid_mask(frame_rgb, img_h, img_w, valid_mask);
        *pre_ms = get_time_ms() - t0;

        // 2. local：跑一次，token 复制到 V 个槽位（同输入同输出）
        t0 = get_time_ms();
        {
            HostInput in{frame_rgb, RKNN3_TENSOR_UINT8, local_in_elems};
            double copy_ms = 0;
            ret = model_run(&local_m, &in, 1, &copy_ms, local_ms);
            if (ret != RKNN3_SUCCESS) {
                *err = "local 推理失败";
                return ret;
            }
        }
        ret = copy_output_to_fp32(&local_m, 0, local_out, local_out_elems);
        if (ret != RKNN3_SUCCESS) {
            *err = "local 输出读取失败";
            return ret;
        }
        for (i = 0; i < (uint32_t)views; ++i)
            memcpy(tokens + (size_t)i * local_out_elems, local_out,
                   local_out_elems * sizeof(float));
        *local_ms = get_time_ms() - t0;

        // 3. global
        t0 = get_time_ms();
        {
            HostInput in{tokens, RKNN3_TENSOR_FLOAT32, global_in_elems};
            double copy_ms = 0;
            ret = model_run(&global_m, &in, 1, &copy_ms, global_ms);
            if (ret != RKNN3_SUCCESS) {
                *err = "global 推理失败";
                return ret;
            }
        }
        for (i = 0; i < feature_count; ++i) {
            size_t bytes = 0;
            free(feats[i]);
            feats[i] = NULL;
            ret = copy_output_to_host(&global_m, i, &feats[i], &bytes);
            if (ret != RKNN3_SUCCESS || bytes != head_in_elems * sizeof(float16)) {
                *err = "global 输出读取失败";
                return -1;
            }
        }
        *global_ms = get_time_ms() - t0;

        // 4. head
        t0 = get_time_ms();
        {
            HostInput* ins = (HostInput*)calloc(feature_count, sizeof(*ins));
            double copy_ms = 0;
            if (ins == NULL) {
                *err = "内存不足";
                return -1;
            }
            for (i = 0; i < feature_count; ++i) {
                ins[i].data = feats[i];
                ins[i].dtype = RKNN3_TENSOR_FLOAT16;
                ins[i].n_elems = head_in_elems;
            }
            ret = model_run(&head_m, ins, feature_count, &copy_ms, head_ms);
            free(ins);
            if (ret != RKNN3_SUCCESS) {
                *err = "head 推理失败";
                return ret;
            }
        }
        ret = copy_output_to_fp32(&head_m, 0, depth, out_elems);
        if (ret != RKNN3_SUCCESS) {
            *err = "depth 输出读取失败";
            return ret;
        }
        // conf 读取但不使用（保留接口完整性）
        copy_output_to_fp32(&head_m, 1, conf, out_elems);
        *head_ms = get_time_ms() - t0;

        // 5. view0 → turbo PNG
        t0 = get_time_ms();
        ret = save_depth_png(out_path, depth, valid_mask, img_h, img_w, dmin, dmax);
        if (ret != 0) {
            *err = "深度图生成失败";
            return ret;
        }
        *post_ms = get_time_ms() - t0;
        return 0;
    }
};

// ── 迷你 JSON：扁平对象里的字符串/整数字段提取（协议自用，不通用）────
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

// ── main：socket 服务 ────────────────────────────────────────────────
int main(int argc, char** argv)
{
    const char* model_dir = (argc > 1) ? argv[1] : kDefaultModelDir;
    const char* sock_path = (argc > 2) ? argv[2] : kDefaultSock;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[depth] model_dir=%s sock=%s\n", model_dir, sock_path);

    static Pipeline pipe;
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
    fprintf(stderr, "[depth] listening on %s\n", sock_path);

    while (true) {
        int cli = ::accept(fd, nullptr, nullptr);
        if (cli < 0) {
            perror("accept");
            continue;
        }
        fprintf(stderr, "[depth] client connected\n");

        std::string buf;
        char tmp[4096];
        while (true) {
            pollfd p{cli, POLLIN, 0};
            int r = ::poll(&p, 1, 500);
            if (r < 0)
                break;
            if (r == 0)
                continue;
            int n = ::recv(cli, tmp, sizeof(tmp) - 1, 0);
            if (n <= 0)
                break;
            tmp[n] = '\0';
            buf += tmp;

            while (true) {
                size_t pos = buf.find('\n');
                if (pos == std::string::npos)
                    break;
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);

                std::string cmd = json_get_str(line, "cmd");
                long qid = json_get_int(line, "qid", 0);
                fprintf(stderr, "[depth] cmd=%s qid=%ld\n", cmd.c_str(), qid);

                if (cmd == "ping") {
                    char resp[256];
                    snprintf(resp, sizeof(resp),
                             "{\"ev\":\"pong\",\"loaded\":%d,\"busy\":%d,\"views\":%d,"
                             "\"res\":\"%ux%u\",\"load_ms\":%.0f}\n",
                             pipe.loaded ? 1 : 0, busy ? 1 : 0, pipe.views, pipe.img_w, pipe.img_h,
                             pipe.load_ms);
                    ::send(cli, resp, strlen(resp), 0);
                } else if (cmd == "load") {
                    if (busy) {
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"engine busy\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    if (pipe.loaded) {
                        std::string resp = "{\"ev\":\"done\",\"qid\":" + std::to_string(qid) +
                                           ",\"views\":" + std::to_string(pipe.views) +
                                           ",\"load_ms\":0}\n";
                        ::send(cli, resp.c_str(), resp.size(), 0);
                        continue;
                    }
                    busy = true;
                    std::string err;
                    double t0 = get_time_ms();
                    int ret = pipe.load(model_dir, &err);
                    busy = false;
                    if (ret != 0) {
                        fprintf(stderr, "[depth] load failed: %s (%.0fms)\n", err.c_str(),
                                get_time_ms() - t0);
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"" + json_escape(err) + "\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    std::string resp =
                        "{\"ev\":\"done\",\"qid\":" + std::to_string(qid) +
                        ",\"views\":" + std::to_string(pipe.views) +
                        ",\"res\":\"" + std::to_string(pipe.img_w) + "x" +
                        std::to_string(pipe.img_h) + "\"" +
                        ",\"load_ms\":" + std::to_string((long)pipe.load_ms) + "}\n";
                    ::send(cli, resp.c_str(), resp.size(), 0);
                } else if (cmd == "infer") {
                    std::string img = json_get_str(line, "img");
                    std::string out = json_get_str(line, "out");
                    if (img.empty() || out.empty()) {
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"img/out missing\"}\n";
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
                    double pre, lms, gms, hms, post, t0 = get_time_ms();
                    float dmin = 0, dmax = 0;
                    // 没加载就先加载（兜底；正常路径 web 层会先发 load）
                    if (!pipe.loaded) {
                        int ret = pipe.load(model_dir, &err);
                        if (ret != 0) {
                            busy = false;
                            std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                            ",\"msg\":\"" + json_escape(err) + "\"}\n";
                            ::send(cli, e.c_str(), e.size(), 0);
                            continue;
                        }
                    }
                    int ret = pipe.infer(img.c_str(), out.c_str(), &err, &pre, &lms, &gms, &hms,
                                         &post, &dmin, &dmax);
                    double total = get_time_ms() - t0;
                    busy = false;
                    if (ret != 0) {
                        fprintf(stderr, "[depth] infer failed: %s\n", err.c_str());
                        std::string e = "{\"ev\":\"error\",\"qid\":" + std::to_string(qid) +
                                        ",\"msg\":\"" + json_escape(err) + "\"}\n";
                        ::send(cli, e.c_str(), e.size(), 0);
                        continue;
                    }
                    char resp[512];
                    snprintf(resp, sizeof(resp),
                             "{\"ev\":\"done\",\"qid\":%ld,\"png\":\"%s\","
                             "\"ms\":{\"pre\":%.0f,\"local\":%.0f,\"global\":%.0f,"
                             "\"head\":%.0f,\"post\":%.0f,\"total\":%.0f},"
                             "\"dmin\":%.3f,\"dmax\":%.3f,\"views\":%d}\n",
                             qid, out.c_str(), pre, lms, gms, hms, post, total, dmin, dmax,
                             pipe.views);
                    ::send(cli, resp, strlen(resp), 0);
                }
            }
        }
        ::close(cli);
        fprintf(stderr, "[depth] client disconnected\n");
    }

    ::close(fd);
    ::unlink(sock_path);
    return 0;
}
