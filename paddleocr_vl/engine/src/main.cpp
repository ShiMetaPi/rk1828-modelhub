// main.cpp — PaddleOCR-VL Unix-socket server
//
// Wraps the official rknn3-model-zoo demo's init_paddleocr_vl_model /
// inference_paddleocr_vl_model / release_paddleocr_vl_model into a long-lived
// service: load models once on first "infer" request, stream results over
// /tmp/ocr_engine.sock, release on "unload" or SIGTERM.
//
// Patterned after depth_engine.cc (same project) and the official paddleocr_vl
// CLI (main.cc). Image loading uses stb_image (single-header); token output is
// streamed per-token via the result_callback.

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "paddleocr_vl.h"
#include "image_utils.h"
#include "Tokenizer.h"
#include "rknn3_api.h"
#include "time_utils.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../../3rdparty/stb/stb_image.h"

// ── prompt normalization (matches official main.cc) ──────────────────
static const char* normalize_prompt(const char* prompt)
{
    const char* DEFAULT_PROMPT  = "OCR:";
    const char* TABLE_PROMPT    = "Table Recognition:";
    const char* CHART_PROMPT    = "Chart Recognition:";
    const char* FORMULA_PROMPT  = "Formula Recognition:";

    if (prompt == nullptr) return DEFAULT_PROMPT;
    if (strcmp(prompt, "table")   == 0) return TABLE_PROMPT;
    if (strcmp(prompt, "chart")   == 0) return CHART_PROMPT;
    if (strcmp(prompt, "formula") == 0) return FORMULA_PROMPT;
    return DEFAULT_PROMPT;
}

// ── sampling (matches official SAMPLE_PARAMS) ─────────────────────────
static const rknn3_sampling_params SAMPLE_PARAMS = {
    .top_k            = 1,
    .top_p            = 0.9f,
    .temperature      = 0.0f,
    .repeat_penalty   = 1.1f,
    .frequency_penalty= 0.0f,
    .presence_penalty = 0.0f,
};

#define MAX_CONTEXT_LEN 4096

// ── embedding (mmap of llm.embed.bin; mirrors official main.cc) ───────
struct embedding_info {
    int       fd = -1;
    float16* data = nullptr;
    int       vocab_size = 0;
    int       embedding_dim = 0;
};

// ── global model state (one engine per process) ───────────────────────
static rknn_app_context_t      g_app_ctx;
static Tokenizer*              g_tokenizer = nullptr;
static struct embedding_info   g_embed;
static struct stat             g_emb_st;
static rknn3_llm_param         g_params;
static rknn3_llm_multimodal_tensor g_tensor;
static RKLLMCallback           g_callback;
static image_buffer_t          g_src_image;
static float16*                g_vision_embeds = nullptr;
static float16*                g_img_embeds = nullptr;
static size_t                  g_vision_embed_elems = 1;
static size_t                  g_img_embed_elems = 1;
static bool                    g_loaded = false;
static int                     g_n_params = 1;
static int                     g_n_inputs = 1;
static rknn_perf_metrics_t     g_perf;

// streaming context — set before each inference, read by result_callback
static int                     g_stream_fd = -1;
static long                    g_stream_qid = 0;
static bool                    g_stream_errored = false;

// ── JSON helpers (copied from depth_engine.cc) ────────────────────────
static std::string json_get_str(const std::string& s, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return "";
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < s.size() && s[p] == ' ') ++p;
    if (p >= s.size() || s[p] != '"') return "";
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) ++p;
        out += s[p++];
    }
    return out;
}

static long json_get_int(const std::string& s, const char* key, long dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return dflt;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return dflt;
    return strtol(s.c_str() + p + 1, NULL, 10);
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if ((unsigned char)ch < 0x20) {
            char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", ch); out += buf;
        } else out += ch;
    }
    return out;
}

static void send_line(int fd, const std::string& s)
{
    std::string line = s + "\n";
    ::send(fd, line.data(), line.size(), 0);
}

// ── streaming token callback ──────────────────────────────────────────
static int result_callback(void* userdata, RKLLMResult* result, LLMCallState state)
{
    (void)userdata;
    if (g_stream_fd < 0) return 0;

    if (state == RKLLM_RUN_ERROR) {
        send_line(g_stream_fd,
            std::string("{\"ev\":\"error\",\"qid\":") + std::to_string(g_stream_qid) +
            ",\"msg\":\"llm runtime error\"}");
        g_stream_errored = true;
        return 0;
    }
    if (state == RKLLM_RUN_NORMAL) {
        std::string piece;
        if (result->num_tokens == 1) {
            piece = g_tokenizer->TokenToPiece(result->token_ids[0]);
        } else {
            piece = g_tokenizer->Decode(result->token_ids, result->num_tokens);
        }
        send_line(g_stream_fd,
            std::string("{\"ev\":\"token\",\"qid\":") + std::to_string(g_stream_qid) +
            ",\"text\":\"" + json_escape(piece) + "\"}");
        return 0;
    }
    // FINISH / MAX_NEW_TOKEN_REACHED / STOP / WAITING — all mean "end of stream"
    // The caller emits the final done event after inference_paddleocr_vl_model
    // returns; nothing to do here.
    return 0;
}

static int tokenizer_callback(void* userdata, const char* text, int32_t text_len,
                              int32_t* tokens, int32_t n_tokens_max)
{
    Tokenizer* tk = (Tokenizer*)userdata;
    int n = tk->Tokenize(text, text_len, tokens, n_tokens_max);
    if (n <= 0) {
        fprintf(stderr, "[ocr] tokenizer failed (text_len=%d)\n", text_len);
        return -1;
    }
    return n;
}

static int embed_callback(void* userdata, int32_t* tokens, uint64_t num_tokens,
                          void* embed, uint64_t len)
{
    struct embedding_info* e = (struct embedding_info*)userdata;
    if (len != num_tokens * e->embedding_dim * sizeof(float16)) {
        fprintf(stderr, "[ocr] invalid embed buffer\n");
        return -1;
    }
    for (uint64_t n = 0; n < num_tokens; n++) {
        memcpy((unsigned char*)embed + n * e->embedding_dim * sizeof(float16),
               e->data + tokens[n] * e->embedding_dim,
               e->embedding_dim * sizeof(float16));
    }
    return 0;
}

// ── image load via stb (RGB888, any input size; vision handles resize) ─
static int load_image_stb(const char* path, image_buffer_t* img)
{
    int w, h, c;
    unsigned char* data = stbi_load(path, &w, &h, &c, 3); // force 3 channels
    if (!data) {
        fprintf(stderr, "[ocr] stbi_load failed for %s\n", path);
        return -1;
    }
    img->virt_addr     = data;
    img->width         = w;
    img->height        = h;
    img->width_stride  = w;
    img->height_stride = h;
    img->format        = IMAGE_FORMAT_RGB888;
    img->size          = w * h * 3;
    img->fd            = -1;
    return 0;
}

// ── model lifecycle ───────────────────────────────────────────────────
static int load_model(const char* model_dir, uint32_t vision_core, uint32_t mlpar_core,
                      uint32_t llm_core, std::string* err)
{
    char p[512];
    auto build = [&](const char* sub, const char* name) {
        snprintf(p, sizeof p, "%s/%s/%s", model_dir, sub, name);
        return std::string(p);
    };
    std::string vision_rknn  = build("vision", "PaddleOCR-vision.rknn");
    std::string vision_w     = build("vision", "PaddleOCR-vision.weight");
    std::string pos_embed    = build("vision", "position_embedding_model.bin");
    std::string llm_rknn     = build("llm",    "PaddleOCR-llm.rknn");
    std::string llm_w        = build("llm",    "PaddleOCR-llm.weight");
    std::string tokenizer_p  = build("llm",    "PaddleOCR-llm.tokenizer.gguf");
    std::string embed_p      = build("llm",    "PaddleOCR-llm.embed.bin");
    std::string mlpar_rknn   = build("vision", "PaddleOCR-vision-mlp_AR.rknn");
    std::string mlpar_w      = build("vision", "PaddleOCR-vision-mlp_AR.weight");

    memset(&g_app_ctx, 0, sizeof(g_app_ctx));
    g_app_ctx.model_width  = 504;
    g_app_ctx.model_height = 504;

    // tokenizer
    g_tokenizer = new Tokenizer(TOKENIZER_BACKEND_LLAMA, tokenizer_p.c_str());
    if (!g_tokenizer) { *err = "tokenizer load failed"; return -1; }
    VocabInfo vocab;
    g_tokenizer->GetVocabInfo(&vocab);

    // embed mmap
    g_embed.fd = ::open(embed_p.c_str(), O_RDONLY);
    if (g_embed.fd == -1) { *err = "open embed.bin failed"; return -1; }
    if (fstat(g_embed.fd, &g_emb_st) == -1) { *err = "fstat embed.bin failed"; return -1; }
    g_embed.data = (float16*)mmap(nullptr, g_emb_st.st_size, PROT_READ, MAP_PRIVATE, g_embed.fd, 0);
    if (g_embed.data == MAP_FAILED) { *err = "mmap embed.bin failed"; return -1; }
    g_embed.vocab_size    = vocab.vocab_size;
    g_embed.embedding_dim = (int)((g_emb_st.st_size / vocab.vocab_size) / sizeof(float16));

    // params
    memset(&g_params, 0, sizeof(g_params));
    g_params.logits_name             = (char*)"logits";
    g_params.max_context_len         = MAX_CONTEXT_LEN;
    g_params.sampling_param          = SAMPLE_PARAMS;
    g_params.vocab_info.vocab_size     = vocab.vocab_size;
    g_params.vocab_info.n_special_eos_id = vocab.n_special_eos_id;
    g_params.vocab_info.n_special_bos_id = vocab.n_special_bos_id;
    memcpy(g_params.vocab_info.special_eos_id, vocab.special_eos_id, sizeof(vocab.special_eos_id));
    memcpy(g_params.vocab_info.special_bos_id, vocab.special_bos_id, sizeof(vocab.special_bos_id));
    g_params.vocab_info.linefeed_id    = vocab.linefeed_id;

    // callback (tokenizer/embed use obj; result_callback uses globals)
    memset(&g_callback, 0, sizeof(g_callback));
    g_callback.result_callback    = result_callback;
    g_callback.result_userdata    = g_tokenizer;
    g_callback.tokenizer_callback = tokenizer_callback;
    g_callback.tokenizer_userdata = g_tokenizer;
    g_callback.embed_callback     = embed_callback;
    g_callback.embed_userdata     = &g_embed;

    int ret = init_paddleocr_vl_model(&g_app_ctx,
        llm_rknn.c_str(), llm_w.c_str(),
        vision_rknn.c_str(), vision_w.c_str(), pos_embed.c_str(),
        mlpar_rknn.c_str(), mlpar_w.c_str(),
        &g_params, g_n_params, g_callback,
        vision_core, mlpar_core, llm_core);
    if (ret != 0) { *err = "init_paddleocr_vl_model failed"; return -1; }

    // pre-allocate the two embed buffers (sized from queried model output shapes)
    g_vision_embed_elems = 1;
    for (size_t i = 0; i < g_app_ctx.vision.embeds_ndims; i++)
        g_vision_embed_elems *= g_app_ctx.vision.embeds_shape[i];
    g_vision_embeds = (float16*)malloc(g_vision_embed_elems * sizeof(float16));

    g_img_embed_elems = 1;
    for (size_t i = 0; i < g_app_ctx.mlpar.embeds_ndims; i++)
        g_img_embed_elems *= g_app_ctx.mlpar.embeds_shape[i];
    g_img_embeds = (float16*)malloc(g_img_embed_elems * sizeof(float16));

    g_loaded = true;
    return 0;
}

static void unload_model()
{
    if (!g_loaded) return;
    release_paddleocr_vl_model(&g_app_ctx);
    if (g_embed.data && g_embed.data != MAP_FAILED) munmap(g_embed.data, g_emb_st.st_size);
    if (g_embed.fd != -1) close(g_embed.fd);
    if (g_tokenizer) delete g_tokenizer;
    if (g_vision_embeds) free(g_vision_embeds);
    if (g_img_embeds) free(g_img_embeds);
    g_tokenizer = nullptr;
    g_vision_embeds = g_img_embeds = nullptr;
    g_embed.data = nullptr;
    g_embed.fd = -1;
    g_loaded = false;
}

// ── per-request infer ─────────────────────────────────────────────────
static void do_infer(int fd, long qid, const char* img_path, const char* prompt_mode)
{
    if (!g_loaded) {
        send_line(fd, std::string("{\"ev\":\"error\",\"qid\":") + std::to_string(qid) +
            ",\"msg\":\"model not loaded\"}");
        return;
    }

    // load image
    memset(&g_src_image, 0, sizeof(g_src_image));
    if (load_image_stb(img_path, &g_src_image) != 0) {
        send_line(fd, std::string("{\"ev\":\"error\",\"qid\":") + std::to_string(qid) +
            ",\"msg\":\"load image failed\"}");
        return;
    }

    // build multimodal tensor (matches official main.cc)
    memset(&g_tensor, 0, sizeof(g_tensor));
    g_tensor.name          = (char*)"input_embeds";
    g_tensor.prompt        = normalize_prompt(prompt_mode);
    g_tensor.image.image_embed = g_img_embeds;
    if (g_app_ctx.mlpar.embeds_ndims == 2) {
        g_tensor.image.n_image_tokens = g_app_ctx.mlpar.embeds_shape[0];
        g_tensor.image.n_image        = 1;
    } else {
        g_tensor.image.n_image_tokens = g_app_ctx.mlpar.embeds_shape[1];
        g_tensor.image.n_image        = g_app_ctx.mlpar.embeds_shape[0];
    }
    g_tensor.image.image_width  = g_app_ctx.vision.model_width;
    g_tensor.image.image_height = g_app_ctx.vision.model_height;
    g_tensor.image.image_start   = "<|IMAGE_START|>";
    g_tensor.image.image_end     = "<|IMAGE_END|>";
    g_tensor.image.image_content = "<|IMAGE_PLACEHOLDER|>";
    g_tensor.enable_thinking     = false;

    // set stream sink
    g_stream_fd     = fd;
    g_stream_qid    = qid;
    g_stream_errored= false;

    int64_t t0 = getCurrentTimeUs();
    memset(&g_perf, 0, sizeof(g_perf));
    int ret = inference_paddleocr_vl_model(&g_app_ctx, &g_src_image,
        g_vision_embeds, g_img_embeds, g_tensor, g_n_inputs, &g_perf);
    int64_t total_us = getCurrentTimeUs() - t0;

    stbi_image_free(g_src_image.virt_addr);
    g_src_image.virt_addr = nullptr;

    g_stream_fd = -1;

    if (ret != 0 || g_stream_errored) {
        send_line(fd, std::string("{\"ev\":\"error\",\"qid\":") + std::to_string(qid) +
            ",\"msg\":\"inference failed\"}");
        return;
    }

    char resp[512];
    snprintf(resp, sizeof resp,
        "{\"ev\":\"done\",\"qid\":%ld,\"tokens\":%d,"
        "\"vision_ms\":%.0f,\"llm_ms\":%.0f,\"total_ms\":%.0f}\n",
        qid, g_perf.n_decode_tokens,
        g_perf.vision_latency / 1000.0,
        (double)(g_perf.llm_end_time - g_perf.llm_start_time) / 1000.0,
        (double)total_us / 1000.0);
    ::send(fd, resp, strlen(resp), 0);
}

// ── main: socket loop ─────────────────────────────────────────────────
int main(int argc, char** argv)
{
    const char* model_dir  = (argc > 1) ? argv[1] : "model";
    const char* sock_path  = (argc > 2) ? argv[2] : "/tmp/ocr_engine.sock";
    uint32_t vision_core = (argc > 3) ? strtoul(argv[3], nullptr, 16) : 0xff;
    uint32_t mlpar_core  = (argc > 4) ? strtoul(argv[4], nullptr, 16) : 0xff;
    uint32_t llm_core    = (argc > 5) ? strtoul(argv[5], nullptr, 16) : 0xff;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[ocr] model_dir=%s sock=%s cores vision/mlpar/llm=%x/%x/%x\n",
            model_dir, sock_path, vision_core, mlpar_core, llm_core);

    ::unlink(sock_path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (::listen(fd, 4) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[ocr] listening on %s\n", sock_path);

    while (true) {
        int cli = ::accept(fd, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }
        fprintf(stderr, "[ocr] client connected\n");

        std::string buf;
        char tmp[4096];
        while (true) {
            pollfd p{cli, POLLIN, 0};
            int r = ::poll(&p, 1, 500);
            if (r < 0) break;
            if (r == 0) continue;
            int n = ::recv(cli, tmp, sizeof(tmp) - 1, 0);
            if (n <= 0) break;
            tmp[n] = '\0';
            buf += tmp;

            while (true) {
                size_t pos = buf.find('\n');
                if (pos == std::string::npos) break;
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);

                std::string cmd = json_get_str(line, "cmd");
                long        qid = json_get_int(line, "qid", 0);
                fprintf(stderr, "[ocr] cmd=%s qid=%ld\n", cmd.c_str(), qid);

                if (cmd == "ping") {
                    char resp[256];
                    snprintf(resp, sizeof resp,
                        "{\"ev\":\"pong\",\"loaded\":%d}\n", g_loaded ? 1 : 0);
                    ::send(cli, resp, strlen(resp), 0);
                } else if (cmd == "load") {
                    if (g_loaded) {
                        send_line(cli, std::string("{\"ev\":\"done\",\"qid\":") +
                            std::to_string(qid) + ",\"loaded\":1}");
                        continue;
                    }
                    std::string err;
                    int64_t t0 = getCurrentTimeUs();
                    int ret = load_model(model_dir, vision_core, mlpar_core, llm_core, &err);
                    int64_t ms = (getCurrentTimeUs() - t0) / 1000;
                    if (ret != 0) {
                        fprintf(stderr, "[ocr] load failed: %s\n", err.c_str());
                        send_line(cli, std::string("{\"ev\":\"error\",\"qid\":") +
                            std::to_string(qid) + ",\"msg\":\"" + json_escape(err) + "\"}");
                    } else {
                        fprintf(stderr, "[ocr] loaded in %lld ms\n", (long long)ms);
                        char resp[128];
                        snprintf(resp, sizeof resp,
                            "{\"ev\":\"done\",\"qid\":%ld,\"loaded\":1,\"load_ms\":%lld}\n",
                            qid, (long long)ms);
                        ::send(cli, resp, strlen(resp), 0);
                    }
                } else if (cmd == "infer") {
                    std::string img = json_get_str(line, "img");
                    std::string mode= json_get_str(line, "prompt");
                    if (img.empty()) {
                        send_line(cli, std::string("{\"ev\":\"error\",\"qid\":") +
                            std::to_string(qid) + ",\"msg\":\"img missing\"}");
                        continue;
                    }
                    if (!g_loaded) {
                        // auto-load on first infer (web layer usually sends load first,
                        // but this is a safe fallback)
                        std::string err;
                        if (load_model(model_dir, vision_core, mlpar_core, llm_core, &err) != 0) {
                            send_line(cli, std::string("{\"ev\":\"error\",\"qid\":") +
                                std::to_string(qid) + ",\"msg\":\"" + json_escape(err) + "\"}");
                            continue;
                        }
                    }
                    do_infer(cli, qid, img.c_str(), mode.c_str());
                } else if (cmd == "unload") {
                    unload_model();
                    send_line(cli, std::string("{\"ev\":\"done\",\"qid\":") +
                        std::to_string(qid) + ",\"loaded\":0}");
                } else {
                    send_line(cli, std::string("{\"ev\":\"error\",\"qid\":") +
                        std::to_string(qid) + ",\"msg\":\"unknown cmd\"}");
                }
            }
        }
        ::close(cli);
        fprintf(stderr, "[ocr] client disconnected\n");
    }

    unload_model();
    ::close(fd);
    ::unlink(sock_path);
    return 0;
}