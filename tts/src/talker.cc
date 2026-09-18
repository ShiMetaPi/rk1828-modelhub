#include <atomic>

#include "tokenizers_cpp.h"
#include "talker.h"
#include "qwen3tts_speaker_embed.h"

#define ALIGNED_SIZE(x, bytes) (((x) + (bytes - 1)) & (~(bytes - 1)))
#define QWEN3_TTS_RET_SUCCESS 0
#define QWEN3_TTS_RET_FAIL (-1)

#ifndef LOGD
#define LOGD(fmt, ...) printf("[Qwen3TTSTalker][D] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOGI
#define LOGI(fmt, ...) printf("[Qwen3TTSTalker][I] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(fmt, ...) printf("[Qwen3TTSTalker][E] " fmt "\n", ##__VA_ARGS__)
#endif

struct TextProjectionContext;
using TextProjectionHandle = TextProjectionContext*;

typedef struct
{
    TalkerCallback output_callback;
    void *output_userdata;

    tokenizers::Tokenizer* tokenizer;
    struct embedding_info text_embed_info;
    struct stat text_emb_st;
    struct embedding_info input_embed_info;
    struct stat input_emb_st;
    struct embedding_info codec_embeddings_info;
    struct stat codec_emb_st;    

    rknn3_context talker_rknn3_ctx;
    rknn3_session* talker_session;
    float16* talker_input_embeds;
    std::vector<float16>* trailing_text_hiddens;
    float16* tts_pad_embed;
    rknn3_tensor output_tensors[2];
    int n_output_tensors;
    int output_tensors_index[2];
    float16 **model_output;                         // 二级指针：按输出tensor数保存多路输出buffer（如logits、past_hidden）
    std::atomic<int> talker_generation_steps{0};

    rknn3_context code_predictor_rknn3_ctx;
    rknn3_session* code_predictor_session;
    int code_predictor_generation_steps;
    std::vector<int>* code_predictor_out_ids;

    TextProjectionHandle text_projection_handle;

    std::atomic<bool> talker_is_generating{false};
} Qwen3TTSTalkerContext;

struct TextProjectionContext {
    rknn3_context rknn_ctx = 0;
    rknn3_input_output_num io_num;
    rknn3_tensor* inputs = NULL;
    rknn3_tensor* outputs = NULL;
};

std::string Qwen3TTSTalker_LoadJsonFileAsString(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.fail()) {
        LOGE("Qwen3TTSTalker_LoadJsonFileAsString: Failed to open file %s", path.c_str());
        return "";
    }
    return ss.str();
}

int Qwen3TTSTalker_ReadEmbedFile(const std::string& embedding_path, int vocab_size, struct embedding_info* embed_info, struct stat* embed_st, int generation_steps) {
    struct embedding_info embedding_info;
    struct stat emb_st;
    memset(&embedding_info, 0x00, sizeof(embedding_info));

    embedding_info.fd = open(embedding_path.c_str(), O_RDONLY);
    if (embedding_info.fd == -1) {
        LOGE("Qwen3TTSTalker_ReadEmbedFile: failed to open embedding file: %s", embedding_path.c_str());
        return -1;
    }

    if (fstat(embedding_info.fd, &emb_st) == -1) {
        LOGE("Qwen3TTSTalker_ReadEmbedFile: failed to get embedding file size");
        close(embedding_info.fd);
        return -1;
    }

    embedding_info.embedding_data = (float16*)mmap(NULL, emb_st.st_size, PROT_READ, MAP_PRIVATE, embedding_info.fd, 0);
    if (embedding_info.embedding_data == MAP_FAILED) {
        LOGE("Qwen3TTSTalker_ReadEmbedFile: failed to mmap embedding file");
        close(embedding_info.fd);
        return -1;
    }

    embedding_info.vocab_size = vocab_size;
    embedding_info.embedding_dim = (emb_st.st_size / vocab_size / generation_steps) / sizeof(float16);
    *embed_info = embedding_info;
    *embed_st = emb_st;

    printf("Qwen3TTSTalker_ReadEmbedFile: embed_path %s  vocab_size %d embedding_dim: %d\n",
           embedding_path.c_str(), embedding_info.vocab_size, embedding_info.embedding_dim);
    return 0;
}

int Qwen3TTSTalker_ReleaseEmbedFile(struct embedding_info& embed_info, struct stat& embed_st) {
    if (embed_info.fd != -1) {
        if (embed_info.embedding_data != MAP_FAILED && embed_info.embedding_data != NULL) {
            munmap((void*)embed_info.embedding_data, embed_st.st_size);
            embed_info.embedding_data = NULL;
        }
        close(embed_info.fd);
        embed_info.fd = -1;
    }
    return 0;
}

bool Qwen3TTSTalker_ReadFP16Data(const std::string& filename, int hidden_size, float16* data, int& tokens_num) {
    std::ifstream inFile(filename, std::ios::binary);
    if (!inFile) {
        LOGE("Qwen3TTSTalker_ReadFP16Data: failed to open file: %s", filename.c_str());
        tokens_num = -1;
        return false;
    }

    inFile.seekg(0, std::ios::end);
    size_t file_size = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    size_t count = file_size / sizeof(float16);
    tokens_num = count / hidden_size;
    printf("Qwen3TTSTalker_ReadFP16Data: LOAD embedding tokens_num %d\n", tokens_num);

    if (file_size % sizeof(float16) != 0) {
        printf("Qwen3TTSTalker_ReadFP16Data: WARNING: file size is not a multiple of float16 size\n");
    }

    inFile.read(reinterpret_cast<char*>(data), file_size);
    if (!inFile) {
        LOGE("Qwen3TTSTalker_ReadFP16Data: failed to read file %s", filename.c_str());
        return false;
    }

    inFile.close();
    return true;
}

// =========================
// 工具函数：Embedding查表
// =========================
// 从 input_embed_info.embedding_data 中按 token_id 取出一条 embedding（dim长度）到 out_ptr
// 注意：这里是 talker.get_input_embeddings 对应的embedding表
void get_input_embedding(Qwen3TTSTalkerContext* ctx, int token_id, float16* out_ptr) {
    int dim = ctx->input_embed_info.embedding_dim;
    if (token_id < 0 || token_id >= ctx->input_embed_info.vocab_size)
        token_id = 0;
    float16* src = ctx->input_embed_info.embedding_data + (token_id * dim);
    memcpy(out_ptr, src, dim * sizeof(float16));
}

// =========================
// 工具函数：embedding逐元素相加
// =========================
// a = a + b（fp16中间转fp32计算再回写fp16）
void elementwise_add(float16* a, const float16* b, int dim) {
    for (int i = 0; i < dim; i++) {
        float f_a = fp16_to_fp32(a[i]);
        float f_b = fp16_to_fp32(b[i]);
        a[i] = fp32_to_fp16(f_a + f_b);
    }
}

static int code_predictor_tokenizer_callback(void *userdata, const char *text, int32_t text_len, int32_t *tokens, int32_t n_tokens_max) {
    return 0;
}

static int code_predictor_result_callback(void* userdata, RKLLMResult* result, LLMCallState state) {
    Qwen3TTSTalkerContext* context = (Qwen3TTSTalkerContext*)userdata;

    if (state == RKLLM_RUN_ERROR) {
        printf("\nCODE Error occurred during inference\n");
        return 0;
    } else if (state == RKLLM_RUN_FINISH) {
        printf("\n--------------------Finished--------------------\n");
        return 0;
    } else if (state == RKLLM_RUN_WAITING) {
        printf("\nWaiting for UTF-8 encoded character\n");
        return 0;
    } else if (state == RKLLM_RUN_MAX_NEW_TOKEN_REACHED) {
        printf("\n--------------Max new token reached-------------\n");
        return 0;
    } else if (state == RKLLM_RUN_STOP) {
        printf("\n-----------------------Stop---------------------\n");
        return 0;
    } else if (state == RKLLM_RUN_NORMAL) {
        // code predictor 每输出一个 token，这里计数 + 记录 token_id
        context->code_predictor_generation_steps += 1;
        printf("\n--------------------CODE PREDICTOR %d New Tokens--------------------\n", context->code_predictor_generation_steps);
        if (result->num_tokens > 1) {
            for (int i = 0; i < result->num_tokens; i++) {
                printf("%d \n", result->token_ids[i]);
            }
            printf("Warning: code predictor callback get more than 1 token\n");
        } else {
            printf("%d \n", result->token_ids[0]);
        }
        context->code_predictor_out_ids->push_back(result->token_ids[0]);
    }
    return 0;
}

static int code_predictor_embed_callback(void* userdata, int32_t* tokens, uint64_t num_tokens, void* embed, uint64_t len) {
    Qwen3TTSTalkerContext* context = (Qwen3TTSTalkerContext*)userdata;
    int generation_steps = context->code_predictor_generation_steps;
    struct embedding_info embed_info = context->codec_embeddings_info;

    // 校验：len 必须等于 num_tokens * dim * sizeof(fp16)
    if (len != num_tokens * embed_info.embedding_dim * sizeof(float16)) {
        LOGE("code_predictor_embed_callback: invalid embed buffer %d vs %d\n", len, num_tokens * embed_info.embedding_dim * sizeof(float16));
        return -1;
    }

    // codec_embeddings_info 是按“步”组织的：每一步有 vocab_size * dim
    size_t offset = (size_t)(generation_steps - 1) * embed_info.vocab_size * embed_info.embedding_dim;
    float16* current_embed_ptr = embed_info.embedding_data + offset;

    // 把 tokens 对应的 embedding 拷贝到 embed buffer
    for (int n = 0; n < (int)num_tokens; n++) {
        memcpy(
            (unsigned char*)embed + n * embed_info.embedding_dim * sizeof(float16),
            current_embed_ptr + tokens[n] * embed_info.embedding_dim,
            embed_info.embedding_dim * sizeof(float16)
        );
    }

    return 0;
}

static int code_predictor_generate(Qwen3TTSTalkerContext* context, float16* input_embeds, int input_tokens_num) {
    // LLM Input Tensor
    int n_inputs = 1;
    rknn3_llm_tensor tensor;
    memset(&tensor, 0, sizeof(rknn3_llm_tensor));

    // LLM Input
    tensor.name     = "input_embeds";
    tensor.prompt   = NULL;
    tensor.embed    = input_embeds;
    tensor.tokens   = NULL;
    tensor.n_tokens = input_tokens_num;
    tensor.enable_thinking = false;
    printf("code_predictor_generate: input tensor n_tokens=%lu\n", tensor.n_tokens);
    
    int ret;
    rknn3_llm_input inputs[n_inputs];
    rknn3_llm_infer_param llm_infer_param;
    memset(inputs, 0, sizeof(inputs));
    memset(&(llm_infer_param), 0, sizeof(llm_infer_param));

    // code predictor 离线模式：不保留历史kv_cache
    llm_infer_param.keep_history = 0;
    llm_infer_param.max_new_tokens = CODEC_MAX_NEW_TOKENS;

    // Set Input Data
    inputs[0].input_type = RKNN3_LLM_INPUT_EMBED;
    inputs[0].llm_input  = tensor;

    // Run
    LOGI("code_predictor_generate: rknn_session_run");
    context->code_predictor_generation_steps = 0;
    context->code_predictor_out_ids->clear();
    ret = rknn3_session_run(context->code_predictor_session, inputs, n_inputs, &llm_infer_param);
    if (ret < 0) {
        LOGE("code_predictor_generate: rknn_session_run fail! ret=%d", ret);
        return ret;
    }

    // Query State
    RKLLMRunState state = {0};
    ret = rknn3_session_query_state(context->code_predictor_session, &state);
    if (ret < 0) {
        LOGE("rknn_session_query_state fail! ret=%d", ret);
        return ret;
    }

    return 0;
}

static int talker_tokenizer_callback(void *userdata, const char *text, int32_t text_len, int32_t *tokens, int32_t n_tokens_max) {
    return 0;
}

// =========================
// talker 输出tensor回调（复制输出到 host buffer）
// =========================
// 把 output_tensors[i] 的 fp16 数据拷贝到 model_output[i]（也是fp16）
// 供后续 embed_callback 使用（例如 past_hidden）
static int talker_output_callback(void* userdata, rknn3_tensor* output_tensors, uint32_t n_output_tensors, LLMOutputCallbackState state) {
    printf("talker_output_callback: state = %d\n", state);

    Qwen3TTSTalkerContext* context = (Qwen3TTSTalkerContext*)userdata;

    float16** model_outputs = context->model_output;
    for (int i = 0; i < n_output_tensors; i++) {
        rknn3_tensor* tensor = &output_tensors[i];
        printf("talker_output_callback: 处理 Tensor [%d], Index = %d, Name = %s, Shape = [%d, %d, %d]\n", i, tensor->attr->index, tensor->attr->name, tensor->attr->shape[0], tensor->attr->shape[1], tensor->attr->shape[2]);

        // 获取当前 tensor 对应的目标缓冲区
        // 注意：这里假设 model_outputs 的顺序与 output_tensors 一一对应
        float16* dest_buffer = model_outputs[i];
        float16* src_ptr = (float16*)tensor->mem->virt_addr;
        if (dest_buffer == NULL) {
            printf("talker_output_callback: 第 %d 个输出缓冲区为空！\n", i);
            continue;
        }

        // 拷贝 fp16 数据到本地buffer（不做fp16->fp32转换）
        for (int j = 0; j < (int)tensor->attr->n_elems; j++) {
            dest_buffer[j] = src_ptr[j];
        }
    }

    return 0;
}

// =========================
// talker 结果回调（主要处理结束态）
// =========================
// 注意：这里在 FINISH 时会手动回调一次 output_callback(fake_codes) 用于通知外部“结束”
// 因为 eos 后不再触发 embed_callback，所以用这种方式让 pipeline 收尾
static int talker_result_callback(void* userdata, RKLLMResult* result, LLMCallState state) {
    Qwen3TTSTalkerContext* context = (Qwen3TTSTalkerContext*)userdata;

    if (state == RKLLM_RUN_ERROR) {
        printf("\nTALKER Error occurred during inference\n");
        context->talker_is_generating = false;
        return 0;
    } else if (state == RKLLM_RUN_FINISH) {
        printf("\n--------------------Finished-------------------- \n");
        context->talker_is_generating = false;
        context->talker_generation_steps = 0;
        std::vector<int>* fake_codes = new std::vector<int>();
        fake_codes->clear();
        if (context->output_callback) {
            context->output_callback(fake_codes, false, context->output_userdata);
        }
        return 0;
    } else if (state == RKLLM_RUN_WAITING) {
        printf("\nWaiting for UTF-8 encoded character\n");
        context->talker_is_generating = false;
        return 0;
    } else if (state == RKLLM_RUN_MAX_NEW_TOKEN_REACHED) {
        printf("\n--------------Max new token reached------------- \n");
        context->talker_is_generating = false;
        return 0;
    } else if (state == RKLLM_RUN_STOP) {
        printf("\n-----------------------Stop--------------------- \n");
        context->talker_is_generating = false;
        context->talker_generation_steps = 0;
        std::vector<int>* fake_codes = new std::vector<int>();
        fake_codes->clear();
        if (context->output_callback) {
            context->output_callback(fake_codes, false, context->output_userdata);
        }
        return 0;
    } else if (state == RKLLM_RUN_NORMAL) {
        int generation_step = context->talker_generation_steps.fetch_add(1) + 1;
        printf("\n--------------------TALKER %d New Tokens--------------------\n", generation_step);
        if (result->num_tokens > 1) {
            for (int i = 0; i < result->num_tokens; i++) {
                printf("%d \n", result->token_ids[i]);
            }
        } else {
            printf("%d \n", result->token_ids[0]);
        }
    }

    return 0;
}

// =========================
// talker embed 回调（每步为下一步提供输入embedding）
// =========================
// 逻辑：
// 1) tokens[0] 是上一时刻生成出的 token_id（或当前步需要的 token）
// 2) 用 token_id 查 input_embeddings，得到 last_id_hidden
// 3) 取 talker 输出的 past_hidden（你存到 model_output[1]）与 last_id_hidden 拼成 code_predictor 输入
// 4) code_predictor 生成 15 个 codec token，查 codec_embeddings 逐步叠加到 inputs_embeds
// 5) 再叠加当前步对应的文本特征（trailing_text_hiddens[step]），如果超出就叠加 pad_embed
// 6) 把 inputs_embeds 写回 embed 供 talker 下步使用
static int talker_embed_callback(void* userdata, int32_t* tokens, uint64_t num_tokens, void* embed, uint64_t len) {
    Qwen3TTSTalkerContext* context = (Qwen3TTSTalkerContext*)userdata;
    struct embedding_info input_embed_info = context->input_embed_info;
    struct embedding_info codec_embed_info = context->codec_embeddings_info;
    if (len != num_tokens * input_embed_info.embedding_dim * sizeof(float16)) {
        LOGE("invalid embed buffer %d vs %d", len, num_tokens * input_embed_info.embedding_dim * sizeof(float16));
        return -1;
    }
    int dim = input_embed_info.embedding_dim;

    // 1) token_id -> last_id_hidden
    std::vector<float16> last_id_hidden(dim);
    std::vector<float16> code_input_embeds(dim*2);
    get_input_embedding(context, tokens[0], last_id_hidden.data());  

    // 2) talker past_hidden + last_id_hidden -> code_predictor 输入
    float16* model_past_hidden_ptr = context->model_output[1];
    memcpy(code_input_embeds.data(), model_past_hidden_ptr, dim * sizeof(float16));
    memcpy(code_input_embeds.data() + dim, last_id_hidden.data(), dim * sizeof(float16));

    // 3) 运行 code_predictor 生成 codec token 序列
    code_predictor_generate(context, code_input_embeds.data(), 2);

    // 4) 将本次 audio_codes（1个主token + 15个codec token）同步回传给外部（decoder线程/模块）
    std::vector<int>* pred_codes = new std::vector<int>();
    pred_codes->push_back((int)tokens[0]);
    for (int i = 0; i < context->code_predictor_out_ids->size(); ++i) {
        pred_codes->push_back((int)(*context->code_predictor_out_ids)[i]);
    }
    if (context->output_callback) {
        context->output_callback(pred_codes, context->talker_is_generating, context->output_userdata);
    }

    // 5) 构造下一步输入 embedding：last_id_hidden + sum(codec_embeds[15]) + text_feature_or_pad
    std::vector<float16> inputs_embeds = last_id_hidden;
    for (int i = 0; i < 15; ++i) {
        int token_id = (*context->code_predictor_out_ids)[i];

        size_t offset = (size_t)(i) * codec_embed_info.vocab_size * codec_embed_info.embedding_dim;
        float16* current_embed_ptr = codec_embed_info.embedding_data + offset;
        const float16* current_code_embed = current_embed_ptr + (token_id * dim);

        elementwise_add(inputs_embeds.data(), current_code_embed, dim);
    }

    // 6) 叠加文本侧特征。无论 streaming 还是 non-streaming，这段 trailing 序列
    // 都在 prefill 阶段一次性准备好，生成过程中只读，不再做增量追加。
    int trailing_text_len = static_cast<int>(context->trailing_text_hiddens->size() / QWEN3_TTS_EMBED_DIM);
    int step = context->talker_generation_steps.load();
    if (step < trailing_text_len) {
        const float16* text_feat_ptr = context->trailing_text_hiddens->data() + ((step - 1) * dim);
        elementwise_add(inputs_embeds.data(), text_feat_ptr, dim);
    } else {
        elementwise_add(inputs_embeds.data(), context->tts_pad_embed, dim);
    }

    // 7) 写回 embed buffer
    memcpy((unsigned char*)embed, inputs_embeds.data(), dim * sizeof(float16));

    return 0;
}

// =========================
// talker_generate：异步启动 talker 推理
// =========================
static int talker_generate(Qwen3TTSTalkerContext* context, float16* input_embeds, int input_tokens_num) {
    // LLM Input Tensor
    int n_inputs = 1;
    rknn3_llm_tensor tensor;
    memset(&tensor, 0, sizeof(rknn3_llm_tensor));

    // LLM Input
    tensor.name     = "input_embeds";
    tensor.prompt   = NULL;
    tensor.embed    = input_embeds;
    tensor.tokens   = NULL;
    tensor.n_tokens = input_tokens_num;
    tensor.enable_thinking = false;
    printf("talker_generate: input tensor n_tokens=%lu\n", tensor.n_tokens);
    
    int ret;
    rknn3_llm_input inputs[n_inputs];
    rknn3_llm_infer_param llm_infer_param;
    memset(inputs, 0, sizeof(inputs));
    memset(&(llm_infer_param), 0, sizeof(llm_infer_param));

    // 离线模式，不需要保存kv_cache
    llm_infer_param.keep_history = 0;
    llm_infer_param.max_new_tokens = QWEN3_TTS_MAX_CONTEXT_LEN;

    // Set Input Data
    inputs[0].input_type = RKNN3_LLM_INPUT_EMBED;
    inputs[0].llm_input  = tensor;

    // Run
    LOGI("talker_generate: rknn_session_run");
    context->talker_is_generating = true;
    ret = rknn3_session_run_async(context->talker_session, inputs, n_inputs, &llm_infer_param);
    if (ret < 0) {
        LOGE("talker_generate: rknn_session_run fail! ret=%d", ret);
        context->talker_is_generating = false;
        return ret;
    }

    return 0;
}

static void text_projection_release(TextProjectionHandle handle) {
    if (handle == NULL) {
        return;
    }

    if (handle->inputs != NULL) {
        for (uint32_t i = 0; i < handle->io_num.n_input; ++i) {
            if (handle->inputs[i].mem != NULL) {
                rknn3_destroy_mem(handle->rknn_ctx, handle->inputs[i].mem);
            }
            free(handle->inputs[i].attr);
        }
        free(handle->inputs);
        handle->inputs = NULL;
    }

    if (handle->outputs != NULL) {
        for (uint32_t i = 0; i < handle->io_num.n_output; ++i) {
            if (handle->outputs[i].mem != NULL) {
                rknn3_destroy_mem(handle->rknn_ctx, handle->outputs[i].mem);
            }
            free(handle->outputs[i].attr);
        }
        free(handle->outputs);
        handle->outputs = NULL;
    }

    if (handle->rknn_ctx != 0) {
        rknn3_destroy(handle->rknn_ctx);
        handle->rknn_ctx = 0;
    }
    delete handle;
}

static int text_projection_init(TextProjectionHandle* out_handle, const std::string& model_dir, const char* device_id) {
    if (out_handle == NULL) {
        return QWEN3_TTS_RET_FAIL;
    }

    TextProjectionContext* handle = new TextProjectionContext();
    int ret = QWEN3_TTS_RET_FAIL;

    rknn3_config config;
    memset(&config, 0, sizeof(config));
    config.run_core_mask = 0x01;

    rknn3_init_extend init_extend;
    memset(&init_extend, 0, sizeof(init_extend));
    if (device_id != NULL && device_id[0] != '\0') {
        init_extend.device_id = const_cast<char*>(device_id);
    }

    ret = rknn3_init(&handle->rknn_ctx, &init_extend);
    if (ret < 0) {
        LOGE("text_projection_init: rknn3_init failed, ret=%d", ret);
        delete handle;
        return QWEN3_TTS_RET_FAIL;
    }

    std::string model_path = model_dir + "/text_projection.rknn";
    std::string weight_path = model_dir + "/text_projection.weight";
    ret = rknn3_load_model_from_path(handle->rknn_ctx, model_path.c_str(), weight_path.c_str());
    if (ret < 0) {
        LOGE("text_projection_init: rknn3_load_model_from_path failed, ret=%d", ret);
        text_projection_release(handle);
        return QWEN3_TTS_RET_FAIL;
    }

    ret = rknn3_model_init(handle->rknn_ctx, &config);
    if (ret < 0) {
        LOGE("text_projection_init: rknn3_model_init failed, ret=%d", ret);
        text_projection_release(handle);
        return QWEN3_TTS_RET_FAIL;
    }

    ret = rknn3_query(handle->rknn_ctx, RKNN3_QUERY_IN_OUT_NUM, &handle->io_num, sizeof(handle->io_num));
    if (ret < 0) {
        LOGE("text_projection_init: rknn3_query io_num failed, ret=%d", ret);
        text_projection_release(handle);
        return QWEN3_TTS_RET_FAIL;
    }

    handle->inputs = (rknn3_tensor*)calloc(handle->io_num.n_input, sizeof(rknn3_tensor));
    handle->outputs = (rknn3_tensor*)calloc(handle->io_num.n_output, sizeof(rknn3_tensor));
    if (handle->inputs == NULL || handle->outputs == NULL) {
        LOGE("text_projection_init: failed to allocate tensor arrays");
        text_projection_release(handle);
        return QWEN3_TTS_RET_FAIL;
    }

    for (uint32_t i = 0; i < handle->io_num.n_input; ++i) {
        rknn3_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        ret = rknn3_query(handle->rknn_ctx, RKNN3_QUERY_INPUT_ATTR, &attr, sizeof(attr));
        if (ret < 0) {
            LOGE("text_projection_init: query input attr failed, ret=%d", ret);
            text_projection_release(handle);
            return QWEN3_TTS_RET_FAIL;
        }
        handle->inputs[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        memcpy(handle->inputs[i].attr, &attr, sizeof(attr));
        handle->inputs[i].mem = rknn3_create_mem(handle->rknn_ctx, attr.aligned_size, attr.core_id, RKNN3_FLAG_MEMORY_CACHEABLE);
        if (handle->inputs[i].mem == NULL) {
            LOGE("text_projection_init: create input mem failed, index=%d", i);
            text_projection_release(handle);
            return QWEN3_TTS_RET_FAIL;
        }
    }

    for (uint32_t i = 0; i < handle->io_num.n_output; ++i) {
        rknn3_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        ret = rknn3_query(handle->rknn_ctx, RKNN3_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (ret < 0) {
            LOGE("text_projection_init: query output attr failed, ret=%d", ret);
            text_projection_release(handle);
            return QWEN3_TTS_RET_FAIL;
        }
        handle->outputs[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        memcpy(handle->outputs[i].attr, &attr, sizeof(attr));
        handle->outputs[i].mem = rknn3_create_mem(handle->rknn_ctx, attr.aligned_size, attr.core_id, RKNN3_FLAG_MEMORY_CACHEABLE);
        if (handle->outputs[i].mem == NULL) {
            LOGE("text_projection_init: create output mem failed, index=%d", i);
            text_projection_release(handle);
            return QWEN3_TTS_RET_FAIL;
        }
    }

    *out_handle = handle;
    return QWEN3_TTS_RET_SUCCESS;
}

// =========================
// text_projection_inference：把长度固定为512的输入送入 text_projection
// =========================
static int text_projection_inference(TextProjectionHandle text_projection_handle, float16* input_data, float16* output_buffer) {
    if (text_projection_handle == NULL) {
        return QWEN3_TTS_RET_FAIL;
    }

    int input_size = text_projection_handle->inputs[0].attr->shape[0] *
                     text_projection_handle->inputs[0].attr->shape[1] *
                     text_projection_handle->inputs[0].attr->shape[2];
    if (text_projection_handle->inputs[0].mem == NULL ||
        text_projection_handle->outputs[0].mem == NULL ||
        text_projection_handle->inputs[0].mem->virt_addr == NULL ||
        text_projection_handle->outputs[0].mem->virt_addr == NULL) {
        LOGE("text_projection_inference: invalid input/output memory");
        return QWEN3_TTS_RET_FAIL;
    }

    memcpy(text_projection_handle->inputs[0].mem->virt_addr, input_data, input_size * sizeof(float16));

    for (uint32_t i = 0; i < text_projection_handle->io_num.n_input; ++i) {
        int ret = rknn3_mem_sync(text_projection_handle->rknn_ctx, text_projection_handle->inputs[i].mem, RKNN3_MEMORY_SYNC_TO_DEVICE);
        if (ret != RKNN3_SUCCESS) {
            LOGE("text_projection_inference: rknn3_mem_sync input failed, ret=%d", ret);
            return QWEN3_TTS_RET_FAIL;
        }
    }

    int ret = rknn3_run(
        text_projection_handle->rknn_ctx,
        text_projection_handle->inputs,
        text_projection_handle->io_num.n_input,
        text_projection_handle->outputs,
        text_projection_handle->io_num.n_output);
    if (ret != RKNN3_SUCCESS) {
        LOGE("text_projection_inference: rknn3_run failed, ret=%d", ret);
        return QWEN3_TTS_RET_FAIL;
    }

    for (uint32_t i = 0; i < text_projection_handle->io_num.n_output; ++i) {
        ret = rknn3_mem_sync(text_projection_handle->rknn_ctx, text_projection_handle->outputs[i].mem, RKNN3_MEMORY_SYNC_FROM_DEVICE);
        if (ret != RKNN3_SUCCESS) {
            LOGE("text_projection_inference: rknn3_mem_sync output failed, ret=%d", ret);
            return QWEN3_TTS_RET_FAIL;
        }
    }

    memcpy(
        output_buffer,
        text_projection_handle->outputs[0].mem->virt_addr,
        text_projection_handle->outputs[0].attr->n_elems * sizeof(float16));
    return QWEN3_TTS_RET_SUCCESS;
}

// =========================
// text_projection_chunked_inference：支持任意长度seq，按512分块推理并拼接
// =========================
// 这里你采用：每块把有效长度拷入512缓冲，其余padding为0，然后推理，最后只取有效长度部分输出
static std::vector<float16> text_projection_chunked_inference(Qwen3TTSTalkerContext* ctx, const float16* input_embed, int seq_len) {                                                          
    int dim = ctx->text_embed_info.embedding_dim;
    int out_dim = QWEN3_TTS_EMBED_DIM;
    const int CHUNK_SIZE = 512; // 模型固定的输入长度
    
    // 预分配结果空间，避免多次内存重分配
    std::vector<float16> final_result(seq_len * out_dim);
    
    // 临时缓冲区：用于存放每次送入模型的 512 长度数据（含 Padding）
    std::vector<float16> chunk_input(CHUNK_SIZE * dim);
    // 临时缓冲区：用于存放模型输出的 512 长度数据
    std::vector<float16> chunk_output_full(CHUNK_SIZE * out_dim);

    for (int i = 0; i < seq_len; i += CHUNK_SIZE) {
        // 1. 计算当前块的有效长度
        int current_valid_len = std::min(CHUNK_SIZE, seq_len - i);

        // 2. 准备输入：清零并拷贝有效数据 (实现自动 Padding)
        memset(chunk_input.data(), 0, chunk_input.size() * sizeof(float16));
        memcpy(chunk_input.data(), input_embed + (i * dim), current_valid_len * dim * sizeof(float16));

        // 3. 执行核心推理 (输入 512, 输出 512)
        int ret = text_projection_inference(ctx->text_projection_handle, chunk_input.data(), chunk_output_full.data());
        if (ret != 0) {
            LOGE("text_projection_chunked_inference： inference failed at chunk starting at %d", i);
            return {};
        }

        // 4. 缝合结果：只拷贝有效长度部分到最终结果容器中
        memcpy(final_result.data() + (i * out_dim), chunk_output_full.data(), current_valid_len * out_dim * sizeof(float16));
    }

    return final_result;
}

// =========================
// get_projected_text_embeds：text_embed查表后送入 projection
// =========================
std::vector<float16> get_projected_text_embeds(Qwen3TTSTalkerContext* ctx, const std::vector<int>& ids) {
    int dim = ctx->text_embed_info.embedding_dim;
    std::vector<float16> raw(ids.size() * dim);
    for (size_t i = 0; i < ids.size(); i++) {
        float16* src = ctx->text_embed_info.embedding_data + (ids[i] * dim);
        memcpy(raw.data() + (i * dim), src, dim * sizeof(float16));
    }

    return text_projection_chunked_inference(ctx, raw.data(), (int)ids.size());
}

// =========================
// process_prefill_embedding：构造当前请求的一次性 prefill 输入并直接启动生成
static int process_prefill_embedding(
    Qwen3TTSTalkerContext* module_ctx,
    const std::string& input_text,
    const std::string& input_instruct,      // CustomVoice 模型需要的输入
    const std::string& input_language,
    const std::string& input_speaker,       // 默认的 speaker
    const std::string& ref_speaker,         // VoiceClone 生成的 speaker
    int non_streaming_mode,
    int& num_tokens
) {
    printf("input_text: %s\n", input_text.c_str());
    printf("input_instruct: %s\n", input_instruct.c_str());
    printf("input_language: %s\n", input_language.c_str());
    printf("input_speaker: %s\n", input_speaker.c_str());
    printf("ref_speaker: %s\n", ref_speaker.c_str());
    printf("non_streaming_mode: %d\n", non_streaming_mode);
    if (input_text.empty()) {
        LOGE("process_prefill_embedding: input_text is empty");
        return -1;
    }

    int dim = QWEN3_TTS_EMBED_DIM;

    // 构造与 python(_build_assistant_text) 一致的模板，然后tokenize
    // 注意：这里 input_ids 的布局固定依赖模板；后续 content ids 都按 [3:-5] 规则提取
    std::string assist_text = "<|im_start|>assistant\n" + input_text + "<|im_end|>\n<|im_start|>assistant\n";
    std::vector<int> input_ids = module_ctx->tokenizer->Encode(assist_text);

    // ===== instruct 部分（如果有）先走 text_projection =====
    std::vector<float16> instruct_projected_embeds;
    if (input_instruct != "") {
        // instruct embedding
        // <|im_start|>user\n{instruct}<|im_end|>
        std::string instruct_text = "<|im_start|>user\n" + input_instruct + "<|im_end|>\n";
        std::vector<int> instruct_ids = module_ctx->tokenizer->Encode(instruct_text);
        int seq_len = (int)instruct_ids.size();

        std::vector<float16> all_raw_embeds(seq_len * dim);
        for (int i = 0; i < seq_len; i++) {
            int tid = instruct_ids[i];
            if (tid >= module_ctx->text_embed_info.vocab_size)
                tid = 0;

            float16* src = module_ctx->text_embed_info.embedding_data + (tid * dim);
            memcpy(all_raw_embeds.data() + (i * dim), src, dim * sizeof(float16));
        }

        // 调用text_projection分段推理
        instruct_projected_embeds = text_projection_chunked_inference(module_ctx, all_raw_embeds.data(), seq_len);
    }

    // ===== speaker embedding（配置声色 or voice clone）=====
    std::vector<float16> speaker_embed;
    std::vector<float16> voice_clone_spk_embeds;

    // 默认的 speaker
    std::string input_speaker_lower = input_speaker;
    std::transform(input_speaker_lower.begin(), input_speaker_lower.end(), input_speaker_lower.begin(), [](unsigned char c) { return std::tolower(c); });

    // VoiceClone 生成的 speaker
    std::string ref_speaker_lower = ref_speaker;
    std::transform(ref_speaker_lower.begin(), ref_speaker_lower.end(), ref_speaker_lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ref_speaker_lower != "") {
        auto it = speaker_embeddings.find(ref_speaker_lower);
        if (it != speaker_embeddings.end()) {
            std::vector<float> speaker_embed_fp = it->second;
            for (int ei = 0; ei < dim; ei++) {
                voice_clone_spk_embeds.push_back(fp32_to_fp16(speaker_embed_fp[ei]));
            }
        }
    }

    bool is_config_speaker = false;
    if (voice_clone_spk_embeds.empty()) {
        // qwen3_tts config定义的声色
        if (input_speaker_lower != "") {
            auto it = spk_id.find(input_speaker_lower);
            if (it == spk_id.end()) {
                LOGI("Speaker %s not found in spk_id map", input_speaker_lower.c_str());
            } else {
                int id = it->second;
                speaker_embed.resize(dim);
                float16* src = module_ctx->input_embed_info.embedding_data + (id * dim);
                memcpy(speaker_embed.data(), src, dim * sizeof(float16));
                is_config_speaker = true;
            }
        }
    } else {
        // TODO: 自定义声色 ref_audio ref_text
        speaker_embed = voice_clone_spk_embeds;
        is_config_speaker = false;
    }

    // ===== language id（含方言覆盖）=====
    std::string input_language_lower = input_language;
    std::transform(input_language_lower.begin(), input_language_lower.end(), input_language_lower.begin(), [](unsigned char c) { return std::tolower(c); });
    
    // 处理语言embedding
    int language_id = -1;
    if (input_language_lower != "auto") {
        auto it = codec_language_id.find(input_language_lower);
        if (it != codec_language_id.end()) {
            language_id = it->second;
        }
    }
    // 方言覆盖逻辑
    auto it_dialect = spk_is_dialect.find(input_speaker_lower);
    if ((input_language_lower == "auto" || input_language_lower == "chinese") && is_config_speaker && speaker_embed.size() > 0 && it_dialect != spk_is_dialect.end() && it_dialect->second != "false") {
        std::string dialect = it_dialect->second;
        auto it_dialect_id = codec_language_id.find(dialect);
        if (it_dialect_id != codec_language_id.end()) {
            language_id = it_dialect_id->second;
        }
    }

    // ===== 基础控制token（BOS/EOS/PAD）的 text_projection 结果 =====
    std::vector<int> base_ctrl_ids = {TTS_BOS_TOKEN_ID, TTS_EOS_TOKEN_ID, TTS_PAD_TOKEN_ID};
    std::vector<float16> base_ctrl_projected = get_projected_text_embeds(module_ctx, base_ctrl_ids);
    float16* tts_bos_ptr = base_ctrl_projected.data();
    float16* tts_eos_ptr = base_ctrl_projected.data() + dim;
    float16* tts_pad_ptr = base_ctrl_projected.data() + 2 * dim;
    if (module_ctx->tts_pad_embed == NULL) {
        module_ctx->tts_pad_embed = (float16*)malloc(dim * sizeof(float16));
        memcpy(module_ctx->tts_pad_embed, tts_pad_ptr, dim * sizeof(float16));
    }

    // ===== codec侧输入embedding（控制token + 可选speaker + pad/bos）=====
    std::vector<float16> codec_input_embedding;
    std::vector<int> prefill_0_ids;     // codec_input_embedding_0
    if (language_id == -1) {
        prefill_0_ids = { CODEC_NOTHINK_ID, CODEC_THINK_BOS_ID, CODEC_THINK_EOS_ID };
    } else {
        prefill_0_ids = { CODEC_THINK_ID, CODEC_THINK_BOS_ID, language_id, CODEC_THINK_EOS_ID };
    }
    for (int tid : prefill_0_ids) {
        std::vector<float16> tmp(dim);
        get_input_embedding(module_ctx, tid, tmp.data());
        codec_input_embedding.insert(codec_input_embedding.end(), tmp.begin(), tmp.end());
    }

    // speaker_embed (如果存在则插入中间)
    if (!speaker_embed.empty()) {
        codec_input_embedding.insert(codec_input_embedding.end(), speaker_embed.begin(), speaker_embed.end());
    }

    // codec_input_embedding_1: [codec_pad_id, codec_bos_id]
    int prefill_1_ids[] = { CODEC_PAD_ID, CODEC_BOS_ID };
    for (int tid : prefill_1_ids) {
        std::vector<float16> tmp(dim);
        get_input_embedding(module_ctx, tid, tmp.data());
        codec_input_embedding.insert(codec_input_embedding.end(), tmp.begin(), tmp.end());
    }
    int codec_total_len = (int)(codec_input_embedding.size() / dim);

    // ===== 组装最终 prefill 输入 embedding =====
    std::vector<float16> final_embeds;
    std::vector<float16> trailing_text_hiddens;

    // 1) instruct（如果有）最前面
    if (!instruct_projected_embeds.empty()) {
        final_embeds.insert(final_embeds.end(), instruct_projected_embeds.begin(), instruct_projected_embeds.end());
    }

    // 2) role header（<|im_start|>assistant\n）对应的 token ids（固定为 151644, 77091, 198）
    std::vector<int> role_ids = {151644, 77091, 198};
    std::vector<float16> role_projected = get_projected_text_embeds(module_ctx, role_ids);
    final_embeds.insert(final_embeds.end(), role_projected.begin(), role_projected.end());

    // 3) fused header：把 text侧(tpad或tbos) + codec侧(除最后一项) 相加
    for (int i = 0; i < codec_total_len - 1; i++) {
        std::vector<float16> fused_node(dim);
        // 文本侧选择: 前 N-1 个用 pad, 最后一个用 bos
        float16* current_text_ptr = (i < codec_total_len - 2) ? tts_pad_ptr : tts_bos_ptr;
        memcpy(fused_node.data(), current_text_ptr, dim * sizeof(float16));
        
        // 叠加音频侧
        elementwise_add(fused_node.data(), codec_input_embedding.data() + (i * dim), dim);
        final_embeds.insert(final_embeds.end(), fused_node.begin(), fused_node.end());
    }

    // ===== streaming / non-streaming 分支 =====
    if (non_streaming_mode == 1) {
        // Non-streaming Content 核心内容叠加
        // Python: [text_projection(input_id[3:-5]) + tts_eos] + codec_pad_embeddings
        // 注意：python input_ids[3:-5] 是实际要读的文字内容
        std::vector<int> content_ids;
        for (size_t i = 3; i < input_ids.size() - 5; i++) {
            content_ids.push_back(input_ids[i]);
        }

        // 获取文本投影
        std::vector<float16> content_projected = get_projected_text_embeds(module_ctx, content_ids);
        // 拼接 tts_eos
        content_projected.insert(content_projected.end(), tts_eos_ptr, tts_eos_ptr + dim);

        // 叠加音频侧的 codec_pad (数量等于 content 长度 + 1)
        int content_len_plus_eos = (int)content_ids.size() + 1;
        std::vector<float16> codec_pad_vec(dim);
        get_input_embedding(module_ctx, CODEC_PAD_ID, codec_pad_vec.data());

        for (int i = 0; i < content_len_plus_eos; i++) {
            elementwise_add(content_projected.data() + (i * dim), codec_pad_vec.data(), dim);
        }
        final_embeds.insert(final_embeds.end(), content_projected.begin(), content_projected.end());

        // 序列结尾 (Trailing / First Token 逻辑)
        // Python: tts_pad_embed + codec_bos_id
        std::vector<float16> tail_fused(dim);
        memcpy(tail_fused.data(), tts_pad_ptr, dim * sizeof(float16));

        std::vector<float16> codec_bos_vec(dim);
        get_input_embedding(module_ctx, CODEC_BOS_ID, codec_bos_vec.data());

        elementwise_add(tail_fused.data(), codec_bos_vec.data(), dim);
        final_embeds.insert(final_embeds.end(), tail_fused.begin(), tail_fused.end());
        trailing_text_hiddens.insert(trailing_text_hiddens.end(), tts_pad_ptr, tts_pad_ptr + dim);
    } else {
        // 流式：prefill只用“第一个内容token”（input_ids[3]）与 codec_last 相加作为入口；
        // trailing_text_hiddens 保存剩余内容（input_ids[4:-5]）+ eos，供后续每步叠加
        std::vector<int> first_text_id = { input_ids[3] };
        std::vector<float16> first_text_projected = get_projected_text_embeds(module_ctx, first_text_id);
        float16* codec_last_ptr = codec_input_embedding.data() + (codec_total_len - 1) * dim;
        elementwise_add(first_text_projected.data(), codec_last_ptr, dim);
        final_embeds.insert(final_embeds.end(), first_text_projected.begin(), first_text_projected.end());    
        
        // 提取 (input_id[:, 4:-5], tts_eos_embed) 转换为 embedding 放入 trailing_text_hidden
        std::vector<int> trailing_ids;
        for (size_t i = 4; i < input_ids.size() - 5; i++) {
            trailing_ids.push_back(input_ids[i]);
        }
        
        if (!trailing_ids.empty()) {
            trailing_text_hiddens = get_projected_text_embeds(module_ctx, trailing_ids);
        }
        trailing_text_hiddens.insert(trailing_text_hiddens.end(), tts_eos_ptr, tts_eos_ptr + dim);
    }

    if (module_ctx->talker_is_generating.load()) {
        LOGE("process_prefill_embedding: talker is busy");
        return -1;
    }

    free(module_ctx->talker_input_embeds);
    module_ctx->talker_input_embeds = NULL;
    module_ctx->talker_generation_steps = 0;
    module_ctx->trailing_text_hiddens->assign(trailing_text_hiddens.begin(), trailing_text_hiddens.end());

    num_tokens = (int)(final_embeds.size() / dim);

    module_ctx->talker_input_embeds = (float16*)malloc(final_embeds.size() * sizeof(float16));
    memcpy(module_ctx->talker_input_embeds, final_embeds.data(), final_embeds.size() * sizeof(float16));

    return talker_generate(module_ctx, module_ctx->talker_input_embeds, num_tokens);
}

static int Qwen3TTSTalker_OnModuleInit(Qwen3TTSTalkerContext* module_ctx, const Qwen3TTSTalkerConfig& config) {
    if (module_ctx == NULL) {
        return QWEN3_TTS_RET_FAIL;
    }

    if (config.callback == NULL) {
        LOGE("Qwen3TTSTalker_OnModuleInit: callback is NULL");
        return QWEN3_TTS_RET_FAIL;
    }
    if (config.model_dir.empty()) {
        LOGE("Qwen3TTSTalker_OnModuleInit: model_dir is empty");
        return QWEN3_TTS_RET_FAIL;
    }

    module_ctx->output_callback = config.callback;
    module_ctx->output_userdata = config.userdata;

    const std::string& model_dir = config.model_dir;
    std::string tokenizer_json = Qwen3TTSTalker_LoadJsonFileAsString(model_dir + "/tokenizer.json");
    auto tokenizer = tokenizers::Tokenizer::FromBlobJSON(tokenizer_json);
    module_ctx->tokenizer = tokenizer.release();
    if (module_ctx->tokenizer == NULL) {
        LOGE("Qwen3TTSTalker_OnModuleInit: failed to load tokenizer");
        return QWEN3_TTS_RET_FAIL;
    }

    std::string text_embed_path = model_dir + "/talker_text_embed.fp16.bin";
    std::string input_embed_path = model_dir + "/talker_input_embed.fp16.bin";
    std::string codec_embed_path = model_dir + "/codec_embed.fp16.bin";
    if (Qwen3TTSTalker_ReadEmbedFile(text_embed_path, TEXT_VOCAB_SIZE, &module_ctx->text_embed_info, &module_ctx->text_emb_st, 1) != 0 ||
        Qwen3TTSTalker_ReadEmbedFile(input_embed_path, TALKER_VOCAB_SIZE, &module_ctx->input_embed_info, &module_ctx->input_emb_st, 1) != 0 ||
        Qwen3TTSTalker_ReadEmbedFile(codec_embed_path, CODEC_VOCAB_SIZE, &module_ctx->codec_embeddings_info, &module_ctx->codec_emb_st, 15) != 0) {
        return QWEN3_TTS_RET_FAIL;
    }

    const char* device_id = config.device_id.empty() ? NULL : config.device_id.c_str();
    if (text_projection_init(&module_ctx->text_projection_handle, model_dir, device_id) != QWEN3_TTS_RET_SUCCESS) {
        LOGE("Qwen3TTSTalker_OnModuleInit: text_projection_init failed");
        return QWEN3_TTS_RET_FAIL;
    }

    rknn3_devices devs;
    memset(&devs, 0, sizeof(devs));
    int ret_rknn = rknn3_find_devices(&devs);
    if (ret_rknn != RKNN3_SUCCESS) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn3_find_devices failed, ret = %d", ret_rknn);
        return QWEN3_TTS_RET_FAIL;
    }

    if (devs.n_devices == 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: No devices found!");
        return QWEN3_TTS_RET_FAIL;
    }

    rknn3_context talker_rknn3_ctx = 0;
    rknn3_session* talker_session = NULL;
    rknn3_config talker_config;
    memset(&talker_config, 0, sizeof(talker_config));
    talker_config.run_core_mask = 0xff;
    int rknn3_ret;

    rknn3_init_extend init_extend;
    memset(&init_extend, 0, sizeof(rknn3_init_extend));
    if (device_id != NULL) {
        init_extend.device_id = const_cast<char*>(device_id);
    }

    rknn3_ret = rknn3_init(&talker_rknn3_ctx, &init_extend);
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn_init fail for talker_rknn3_ctx, ret=%d", rknn3_ret);
        return QWEN3_TTS_RET_FAIL;
    }

    std::string talker_model_path = model_dir + "/talker.rknn";
    std::string talker_model_weight_path = model_dir + "/talker.weight";
    rknn3_ret = rknn3_load_model_from_path(talker_rknn3_ctx, talker_model_path.c_str(), talker_model_weight_path.c_str());
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn_load_model for talker_rknn3_ctx failed, ret=%d", rknn3_ret);
        return QWEN3_TTS_RET_FAIL;
    }

    rknn3_ret = rknn3_model_init(talker_rknn3_ctx, &talker_config);
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn_model_init for talker_rknn3_ctx failed, ret=%d", rknn3_ret);
        return QWEN3_TTS_RET_FAIL;
    }

    int talker_n_params = 1;
    rknn3_llm_param talker_params;
    memset(&talker_params, 0, sizeof(rknn3_llm_param));

    RKLLMCallback talker_callback;
    memset(&talker_callback, 0, sizeof(RKLLMCallback));

    talker_params.logits_name = (char*)"logits";
    talker_params.max_context_len = QWEN3_TTS_MAX_CONTEXT_LEN;
    talker_params.sampling_param = TALKER_SAMPLE_PARAMS;
    talker_params.vocab_info.vocab_size = TALKER_VOCAB_SIZE;
    talker_params.vocab_info.n_special_eos_id = 1;
    talker_params.vocab_info.special_eos_id[0] = CODEC_EOS_TOKEN_ID;

    module_ctx->n_output_tensors = 2;
    module_ctx->output_tensors_index[0] = 0;
    module_ctx->output_tensors_index[1] = 1;
    module_ctx->model_output = NULL;
    memset(module_ctx->output_tensors, 0, sizeof(module_ctx->output_tensors));

    module_ctx->model_output = (float16**)malloc(sizeof(float16*) * module_ctx->n_output_tensors);
    for (int i = 0; i < module_ctx->n_output_tensors; i++) {
        module_ctx->output_tensors[i].attr = (rknn3_tensor_attr*)malloc(sizeof(rknn3_tensor_attr));
        module_ctx->output_tensors[i].attr->index = module_ctx->output_tensors_index[i];
        rknn3_ret = rknn3_query(talker_rknn3_ctx, RKNN3_QUERY_OUTPUT_ATTR, module_ctx->output_tensors[i].attr, sizeof(rknn3_tensor_attr));
        if (rknn3_ret < 0) {
            LOGE("Qwen3TTSTalker_OnModuleInit: rknn_query for talker_rknn3_ctx fail, ret=%d", rknn3_ret);
            return QWEN3_TTS_RET_FAIL;
        }

        module_ctx->output_tensors[i].mem = rknn3_create_mem(
            talker_rknn3_ctx,
            module_ctx->output_tensors[i].attr->aligned_size,
            module_ctx->output_tensors[i].attr->core_id,
            RKNN3_FLAG_MEMORY_CACHEABLE);
        module_ctx->model_output[i] = (float16*)malloc(module_ctx->output_tensors[i].attr->n_elems * sizeof(float16));
        if (!module_ctx->model_output[i]) {
            LOGE("Qwen3TTSTalker_OnModuleInit: failed to allocate memory for talker_rknn3_ctx model output!");
            return QWEN3_TTS_RET_FAIL;
        }
    }

    talker_callback.result_callback = talker_result_callback;
    talker_callback.result_userdata = module_ctx;
    talker_callback.tokenizer_callback = talker_tokenizer_callback;
    talker_callback.tokenizer_userdata = module_ctx;
    talker_callback.embed_callback = talker_embed_callback;
    talker_callback.embed_userdata = module_ctx;
    talker_callback.output_callback = talker_output_callback;
    talker_callback.output_userdata = module_ctx;
    talker_callback.output_tensors = module_ctx->output_tensors;
    talker_callback.n_output_tensors = module_ctx->n_output_tensors;

    talker_session = rknn3_session_init(talker_rknn3_ctx, &talker_params, talker_n_params);
    if (!talker_session) {
        LOGE("Qwen3TTSTalker_OnModuleInit: failed to initialize talker_rknn3_ctx session");
        return QWEN3_TTS_RET_FAIL;
    }

    rknn3_ret = rknn3_session_set_callback(talker_session, &(talker_callback));
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: failed to set talker_session callback");
        return QWEN3_TTS_RET_FAIL;
    }

    module_ctx->talker_rknn3_ctx = talker_rknn3_ctx;
    module_ctx->talker_session = talker_session;
    module_ctx->talker_input_embeds = NULL;
    module_ctx->tts_pad_embed = NULL;
    module_ctx->talker_generation_steps = 0;
    module_ctx->trailing_text_hiddens = new std::vector<float16>();

    rknn3_context code_predictor_rknn3_ctx = 0;
    rknn3_session* code_predictor_session = NULL;
    rknn3_config code_predictor_config;
    memset(&code_predictor_config, 0, sizeof(code_predictor_config));
    code_predictor_config.run_core_mask = 0xff;

    rknn3_ret = rknn3_init(&code_predictor_rknn3_ctx, &init_extend);
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn_init fail for code_predictor_rknn3_ctx, ret=%d", rknn3_ret);
        return QWEN3_TTS_RET_FAIL;
    }

    std::string code_predictor_model_path = model_dir + "/code_predictor.rknn";
    std::string code_predictor_model_weight_path = model_dir + "/code_predictor.weight";
    rknn3_ret = rknn3_load_model_from_path(code_predictor_rknn3_ctx, code_predictor_model_path.c_str(), code_predictor_model_weight_path.c_str());
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn_load_model failed for code_predictor_rknn3_ctx, ret=%d", rknn3_ret);
        return QWEN3_TTS_RET_FAIL;
    }

    rknn3_ret = rknn3_model_init(code_predictor_rknn3_ctx, &code_predictor_config);
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: rknn_model_init failed for code_predictor_rknn3_ctx, ret=%d", rknn3_ret);
        return QWEN3_TTS_RET_FAIL;
    }

    int code_predictor_n_params = 1;
    rknn3_llm_param code_predictor_params;
    memset(&code_predictor_params, 0, sizeof(rknn3_llm_param));
    RKLLMCallback code_predictor_callback;
    memset(&code_predictor_callback, 0, sizeof(RKLLMCallback));

    code_predictor_params.logits_name = (char*)"logits";
    code_predictor_params.max_context_len = CODEC_MAX_CONTEXT_LEN;
    code_predictor_params.sampling_param = CODE_PREDICTOR_SAMPLE_PARAMS;
    code_predictor_params.vocab_info.vocab_size = CODEC_VOCAB_SIZE;
    code_predictor_params.vocab_info.n_special_eos_id = 1;
    code_predictor_params.vocab_info.special_eos_id[0] = CODEC_EOS_TOKEN_ID;

    code_predictor_callback.result_callback = code_predictor_result_callback;
    code_predictor_callback.result_userdata = module_ctx;
    code_predictor_callback.tokenizer_callback = code_predictor_tokenizer_callback;
    code_predictor_callback.tokenizer_userdata = module_ctx;
    code_predictor_callback.embed_callback = code_predictor_embed_callback;
    code_predictor_callback.embed_userdata = module_ctx;

    code_predictor_session = rknn3_session_init(code_predictor_rknn3_ctx, &code_predictor_params, code_predictor_n_params);
    if (!code_predictor_session) {
        LOGE("Qwen3TTSTalker_OnModuleInit: failed to initialize code_predictor_rknn3_ctx session");
        return QWEN3_TTS_RET_FAIL;
    }

    rknn3_ret = rknn3_session_set_callback(code_predictor_session, &(code_predictor_callback));
    if (rknn3_ret < 0) {
        LOGE("Qwen3TTSTalker_OnModuleInit: failed to set callback for code_predictor_rknn3_ctx");
        return QWEN3_TTS_RET_FAIL;
    }

    module_ctx->code_predictor_rknn3_ctx = code_predictor_rknn3_ctx;
    module_ctx->code_predictor_session = code_predictor_session;
    module_ctx->code_predictor_generation_steps = 0;
    module_ctx->code_predictor_out_ids = new std::vector<int>();
    module_ctx->talker_is_generating = false;

    return QWEN3_TTS_RET_SUCCESS;
}

static int Qwen3TTSTalker_OnModuleRelease(Qwen3TTSTalkerContext* module_ctx) {
    if (module_ctx == NULL) {
        LOGE("Qwen3TTSTalker_OnModuleRelease: module_ctx is NULL");
        return QWEN3_TTS_RET_FAIL;
    }

    if (module_ctx->talker_session != NULL && module_ctx->talker_is_generating.load()) {
        rknn3_session_stop(module_ctx->talker_session);
        module_ctx->talker_is_generating = false;
    }

    delete module_ctx->tokenizer;
    Qwen3TTSTalker_ReleaseEmbedFile(module_ctx->text_embed_info, module_ctx->text_emb_st);
    Qwen3TTSTalker_ReleaseEmbedFile(module_ctx->input_embed_info, module_ctx->input_emb_st);
    Qwen3TTSTalker_ReleaseEmbedFile(module_ctx->codec_embeddings_info, module_ctx->codec_emb_st);

    text_projection_release(module_ctx->text_projection_handle);
    module_ctx->text_projection_handle = NULL;

    for (int i = 0; i < module_ctx->n_output_tensors; ++i) {
        if (module_ctx->output_tensors[i].mem) {
            rknn3_destroy_mem(module_ctx->talker_rknn3_ctx, module_ctx->output_tensors[i].mem);
            module_ctx->output_tensors[i].mem = NULL;
        }
        if (module_ctx->output_tensors[i].attr) {
            free(module_ctx->output_tensors[i].attr);
            module_ctx->output_tensors[i].attr = NULL;
        }
    }

    if (module_ctx->model_output != NULL) {
        for (int i = 0; i < module_ctx->n_output_tensors; ++i) {
            free(module_ctx->model_output[i]);
        }
        free(module_ctx->model_output);
        module_ctx->model_output = NULL;
    }

    if (module_ctx->talker_session != NULL) {
        rknn3_session_destroy(module_ctx->talker_session);
        module_ctx->talker_session = NULL;
    }
    if (module_ctx->talker_rknn3_ctx != 0) {
        rknn3_destroy(module_ctx->talker_rknn3_ctx);
        module_ctx->talker_rknn3_ctx = 0;
    }

    if (module_ctx->code_predictor_session != NULL) {
        rknn3_session_destroy(module_ctx->code_predictor_session);
        module_ctx->code_predictor_session = NULL;
    }
    if (module_ctx->code_predictor_rknn3_ctx != 0) {
        rknn3_destroy(module_ctx->code_predictor_rknn3_ctx);
        module_ctx->code_predictor_rknn3_ctx = 0;
    }
    delete module_ctx->code_predictor_out_ids;

    free(module_ctx->talker_input_embeds);
    delete module_ctx->trailing_text_hiddens;
    module_ctx->trailing_text_hiddens = NULL;
    free(module_ctx->tts_pad_embed);

    return QWEN3_TTS_RET_SUCCESS;
}


static int Qwen3TTSTalker_OnModuleProcess(Qwen3TTSTalkerContext* module_ctx, const Qwen3TTSTalkerRequest& request) {
    if (module_ctx == NULL) {
        LOGE("Qwen3TTSTalker_OnModuleProcess: module_ctx is NULL");
        return QWEN3_TTS_RET_FAIL;
    }

    const std::string& input_text = request.text;
    const std::string& input_instruct = request.instruct;
    const std::string& input_language = request.language;
    const std::string& input_speaker = request.speaker;
    const std::string& ref_speaker = request.ref_speaker;
    int non_streaming_mode = request.non_streaming_mode;

    int tokens_num = 0;
    return process_prefill_embedding(
        module_ctx,
        input_text,
        input_instruct,
        input_language,
        input_speaker,
        ref_speaker,
        non_streaming_mode,
        tokens_num);
}

struct Qwen3TTSTalker::Impl {
    Qwen3TTSTalkerContext* ctx = NULL;
};

Qwen3TTSTalker::Qwen3TTSTalker() : impl_(new Impl()) {}

Qwen3TTSTalker::~Qwen3TTSTalker() {
    Release();
    delete impl_;
    impl_ = NULL;
}

int Qwen3TTSTalker::Init(const Qwen3TTSTalkerConfig& config) {
    Release();
    if (impl_ == NULL) {
        impl_ = new Impl();
    }

    impl_->ctx = new Qwen3TTSTalkerContext();
    if (Qwen3TTSTalker_OnModuleInit(impl_->ctx, config) != QWEN3_TTS_RET_SUCCESS) {
        Release();
        return QWEN3_TTS_RET_FAIL;
    }
    return QWEN3_TTS_RET_SUCCESS;
}

int Qwen3TTSTalker::Process(const Qwen3TTSTalkerRequest& request) {
    if (impl_ == NULL || impl_->ctx == NULL) {
        LOGE("Process: talker is not initialized");
        return QWEN3_TTS_RET_FAIL;
    }
    return Qwen3TTSTalker_OnModuleProcess(impl_->ctx, request);
}

void Qwen3TTSTalker::Release() {
    if (impl_ == NULL || impl_->ctx == NULL) {
        return;
    }

    Qwen3TTSTalker_OnModuleRelease(impl_->ctx);
    delete impl_->ctx;
    impl_->ctx = NULL;
}
