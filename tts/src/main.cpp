// main.cpp — Qwen3-TTS socket 服务（移植官方 rknn3-model-zoo demo 的 decoder 逻辑）
// 命令行: ./tts_engine <model_dir> [sock_path]
//   model_dir: 模型根目录（平铺布局，见 deploy.sh）
//   sock_path: Unix socket 路径（默认 /tmp/tts_engine.sock）
//
// 命令协议 (JSON, \n 分隔):
//   {"cmd": "speak", "text": "你好", "qid": 1}
//   {"cmd": "ping"}
//
// 响应 (JSON, \n 分隔):
//   {"ev": "done", "qid": 1, "wav": "/tmp/tts_out_1.wav"}
//   {"ev": "error", "qid": 1, "msg": "..."}
//   {"ev": "pong"}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "speech_decoder.h"
#include "spk_encoder.h"
#include "talker.h"

namespace {

const int kFeatSize = 16;
const int kChunkSize = 25;
const int kLeftContextSize = 25;
const int kWindowLen = kChunkSize + kLeftContextSize;
const int kTotalUpsample = 1920;
const int kSampleRate = 24000;

const char* kDefaultSock = "/tmp/tts_engine.sock";
const char* kDefaultModelDir = "model";   // relative default; tts_engine.py passes model_dir explicitly

void write_wav(const std::string& filename, const std::vector<float>& audio, int sample_rate) {
    int num_channels = 1;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    int block_align = num_channels * (bits_per_sample / 8);

    // float32 → int16 PCM。用最通用的 16-bit 整数格式，避免浏览器/播放器
    // 对 float32 + 非标准采样率的解码兼容性问题（板子 Chromium 软渲染易崩）。
    std::vector<int16_t> pcm(audio.size());
    for (size_t i = 0; i < audio.size(); ++i) {
        float v = audio[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = static_cast<int16_t>(v * 32767.0f);
    }
    int data_size = static_cast<int>(pcm.size() * sizeof(int16_t));
    int chunk_size = 36 + data_size;

    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        fprintf(stderr, "[tts] failed to write wav: %s\n", filename.c_str());
        return;
    }

    ofs.write("RIFF", 4);
    ofs.write(reinterpret_cast<const char*>(&chunk_size), 4);
    ofs.write("WAVE", 4);

    ofs.write("fmt ", 4);
    int subchunk1_size = 16;
    ofs.write(reinterpret_cast<const char*>(&subchunk1_size), 4);
    int audio_format = 1;  // PCM
    ofs.write(reinterpret_cast<const char*>(&audio_format), 2);
    ofs.write(reinterpret_cast<const char*>(&num_channels), 2);
    ofs.write(reinterpret_cast<const char*>(&sample_rate), 4);
    ofs.write(reinterpret_cast<const char*>(&byte_rate), 4);
    ofs.write(reinterpret_cast<const char*>(&block_align), 2);
    ofs.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

    ofs.write("data", 4);
    ofs.write(reinterpret_cast<const char*>(&data_size), 4);
    ofs.write(reinterpret_cast<const char*>(pcm.data()), data_size);
}

// 把 [length, 16] 的 codes 转置成 [16, length]（speech_decoder 输入格式）
std::vector<int32_t> transpose_lx16_to_16xl(const std::vector<int32_t>& src_lx16, int length) {
    std::vector<int32_t> dst_16xl(kFeatSize * length, 0);
    for (int i = 0; i < length; ++i) {
        for (int j = 0; j < kFeatSize; ++j) {
            dst_16xl[j * length + i] = src_lx16[i * kFeatSize + j];
        }
    }
    return dst_16xl;
}

struct DemoState {
    Qwen3TTSSpeechDecoder* decoder = nullptr;
    std::vector<int32_t> pending_codes_lx16;
    std::vector<int32_t> prev_context_lx16;
    std::vector<float> audio_buffer;
    std::thread decoder_thread;
    std::mutex mutex;
    std::condition_variable token_cond;
    std::condition_variable cond;
    std::string output_path;
    bool stop_requested = false;
    bool talker_done = false;
    bool finished = false;
    bool failed = false;
};

bool decode_one_window(DemoState* state, const std::vector<int32_t>& window_frames_lx16,
                       int context_size, int take_new) {
    std::vector<int32_t> window_frames_16xl = transpose_lx16_to_16xl(window_frames_lx16, kWindowLen);
    std::vector<float> audio_values;
    if (state->decoder->Decode(window_frames_16xl, &audio_values) != 0) {
        fprintf(stderr, "[tts] speech decoder failed\n");
        return false;
    }

    size_t start = std::min(static_cast<size_t>(context_size * kTotalUpsample), audio_values.size());
    size_t end = std::min(start + static_cast<size_t>(take_new * kTotalUpsample), audio_values.size());
    if (start >= end) {
        fprintf(stderr, "[tts] invalid decoder crop range\n");
        return false;
    }

    state->audio_buffer.insert(state->audio_buffer.end(), audio_values.begin() + start, audio_values.begin() + end);
    write_wav(state->output_path, state->audio_buffer, kSampleRate);
    return true;
}

void decoder_thread_entry(DemoState* state) {
    while (true) {
        std::vector<int32_t> window_frames_lx16(kWindowLen * kFeatSize, 0);
        int context_size = 0;
        int take_new = 0;

        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->token_cond.wait(lock, [state]() {
                int pending_frames = static_cast<int>(state->pending_codes_lx16.size() / kFeatSize);
                bool is_first = state->prev_context_lx16.empty();
                int need_new = is_first ? kWindowLen : kChunkSize;
                if (state->stop_requested) return true;
                if (state->talker_done) return true;
                return pending_frames >= need_new;
            });

            int pending_frames = static_cast<int>(state->pending_codes_lx16.size() / kFeatSize);
            if (state->stop_requested && pending_frames <= 0) break;
            if (state->talker_done && pending_frames <= 0) {
                state->prev_context_lx16.clear();
                state->finished = true;
                state->cond.notify_one();
                break;
            }

            bool is_first = state->prev_context_lx16.empty();
            context_size = is_first ? 0 : kLeftContextSize;
            int need_new = is_first ? kWindowLen : kChunkSize;

            if (pending_frames >= need_new) {
                take_new = need_new;
            } else if (state->talker_done && pending_frames > 0) {
                take_new = pending_frames;
            } else if (state->stop_requested && pending_frames > 0) {
                take_new = pending_frames;
            } else {
                continue;
            }

            if (context_size > 0) {
                std::memcpy(window_frames_lx16.data(), state->prev_context_lx16.data(),
                            sizeof(int32_t) * context_size * kFeatSize);
            }
            std::memcpy(window_frames_lx16.data() + context_size * kFeatSize,
                        state->pending_codes_lx16.data(),
                        sizeof(int32_t) * take_new * kFeatSize);

            int valid_len = context_size + take_new;
            if (valid_len >= kLeftContextSize) {
                int ctx_start = (valid_len - kLeftContextSize) * kFeatSize;
                state->prev_context_lx16.resize(kLeftContextSize * kFeatSize);
                std::memcpy(state->prev_context_lx16.data(),
                            window_frames_lx16.data() + ctx_start,
                            sizeof(int32_t) * kLeftContextSize * kFeatSize);
            } else {
                state->prev_context_lx16.clear();
            }

            state->pending_codes_lx16.erase(
                state->pending_codes_lx16.begin(),
                state->pending_codes_lx16.begin() + take_new * kFeatSize);
        }

        if (!decode_one_window(state, window_frames_lx16, context_size, take_new)) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            state->finished = true;
            state->cond.notify_one();
            break;
        }
    }
}

// 当前正在合成的请求 state（串行处理，单请求在飞，全局指针安全）
static DemoState* g_current_state = nullptr;

void talker_callback(std::vector<int>* tokens, bool talker_is_generating, void* userdata) {
    (void)userdata;
    DemoState* state = g_current_state;
    if (state == nullptr) return;
    std::unique_ptr<std::vector<int> > owned_tokens(tokens);

    std::lock_guard<std::mutex> lock(state->mutex);

    if (tokens != NULL && !tokens->empty()) {
        if (tokens->size() != kFeatSize) {
            fprintf(stderr, "[tts] unexpected token group size: %zu\n", tokens->size());
            state->failed = true;
            state->finished = true;
            state->cond.notify_one();
            return;
        }
        for (size_t i = 0; i < tokens->size(); ++i) {
            state->pending_codes_lx16.push_back((*tokens)[i]);
        }
    } else if (!talker_is_generating) {
        state->talker_done = true;
    }
    state->token_cond.notify_one();
}

// ── 最简 JSON 解析 ────────────────────────────────────────────────────
struct Msg {
    std::string cmd;
    std::string text;
    std::string instruct;
    std::string speaker;
    std::string pcm;    // extract：24kHz mono float32 PCM 文件路径
    std::string name;   // extract：音色名（存 voices/{name}.npy）
    int qid = 0;
};

// 音色名合法（小写字母/数字/_/-，1~24 位）且不含路径字符
static bool valid_voice_name(const std::string& raw, std::string& out) {
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    if (s.empty() || s.size() > 24) return false;
    for (char c : s) {
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    out = s;
    return true;
}

static Msg ParseMsg(const std::string& line) {
    Msg m;
    size_t p;
    if ((p = line.find("\"cmd\"")) != std::string::npos) {
        size_t q = line.find("\"", p + 5);
        size_t r = line.find("\"", q + 1);
        if (q != std::string::npos && r != std::string::npos)
            m.cmd = line.substr(q + 1, r - q - 1);
    }
    if ((p = line.find("\"text\"")) != std::string::npos) {
        size_t q = line.find("\"", p + 6);
        size_t r = line.find("\"", q + 1);
        if (q != std::string::npos && r != std::string::npos)
            m.text = line.substr(q + 1, r - q - 1);
    }
    if ((p = line.find("\"instruct\"")) != std::string::npos) {
        size_t q = line.find("\"", p + 10);
        size_t r = line.find("\"", q + 1);
        if (q != std::string::npos && r != std::string::npos)
            m.instruct = line.substr(q + 1, r - q - 1);
    }
    if ((p = line.find("\"speaker\"")) != std::string::npos) {
        size_t q = line.find("\"", p + 9);
        size_t r = line.find("\"", q + 1);
        if (q != std::string::npos && r != std::string::npos)
            m.speaker = line.substr(q + 1, r - q - 1);
    }
    if ((p = line.find("\"pcm\"")) != std::string::npos) {
        size_t q = line.find("\"", p + 5);
        size_t r = line.find("\"", q + 1);
        if (q != std::string::npos && r != std::string::npos)
            m.pcm = line.substr(q + 1, r - q - 1);
    }
    if ((p = line.find("\"name\"")) != std::string::npos) {
        size_t q = line.find("\"", p + 6);
        size_t r = line.find("\"", q + 1);
        if (q != std::string::npos && r != std::string::npos)
            m.name = line.substr(q + 1, r - q - 1);
    }
    if ((p = line.find("\"qid\"")) != std::string::npos) {
        // "qid" 是 5 个字符，其后是冒号 ':'，数字从 p+6 开始
        m.qid = atoi(line.c_str() + p + 6);
    }
    return m;
}

static std::string JsonDone(int qid, const std::string& wav) {
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"ev\":\"done\",\"qid\":%d,\"wav\":\"%s\"}\n", qid, wav.c_str());
    return std::string(buf);
}

static std::string JsonError(int qid, const std::string& msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"ev\":\"error\",\"qid\":%d,\"msg\":\"%s\"}\n", qid, msg.c_str());
    return std::string(buf);
}

static std::string JsonPong() {
    return std::string("{\"ev\":\"pong\"}\n");
}

static std::string JsonVoiceSaved(int qid, const std::string& name) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ev\":\"done\",\"qid\":%d,\"name\":\"%s\"}\n", qid, name.c_str());
    return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
    const char* model_dir = (argc > 1) ? argv[1] : kDefaultModelDir;
    const char* sock_path = (argc > 2) ? argv[2] : kDefaultSock;

    fprintf(stderr, "[tts] model_dir=%s sock=%s\n", model_dir, sock_path);

    // 常驻 speech_decoder（加载一次，多个 speak 复用）
    Qwen3TTSSpeechDecoder decoder;
    {
        Qwen3TTSSpeechDecoder::Config dec_cfg;
        dec_cfg.model_dir = model_dir;
        if (decoder.Init(dec_cfg) != 0) {
            fprintf(stderr, "[tts] speech_decoder init failed\n");
            return -1;
        }
        fprintf(stderr, "[tts] speech_decoder ready\n");
    }

    // 常驻 talker（加载一次，多个 speak 复用）
    Qwen3TTSTalker talker;
    {
        Qwen3TTSTalkerConfig talker_cfg;
        talker_cfg.model_dir = model_dir;
        talker_cfg.callback = talker_callback;
        talker_cfg.userdata = nullptr;  // 实际用全局 g_current_state
        if (talker.Init(talker_cfg) != 0) {
            fprintf(stderr, "[tts] talker init failed\n");
            return -1;
        }
        fprintf(stderr, "[tts] talker ready\n");
    }

    // 板端声音克隆提取器（文件缺失则降级不可用，不影响 TTS 主流程）
    SpkEncoder spk_encoder;
    if (spk_encoder.Init(model_dir) == 0) {
        fprintf(stderr, "[tts] spk encoder ready（板端提取可用）\n");
    } else {
        fprintf(stderr, "[tts] spk encoder 不可用，extract 命令会报错（不影响合成）\n");
    }

    // Unix socket
    ::unlink(sock_path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (::listen(fd, 1) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[tts] listening on %s\n", sock_path);

    while (true) {
        int cli = ::accept(fd, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }
        fprintf(stderr, "[tts] client connected fd=%d\n", cli);

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

                Msg m = ParseMsg(line);
                fprintf(stderr, "[tts] cmd=%s qid=%d\n", m.cmd.c_str(), m.qid);

                if (m.cmd == "speak") {
                    if (m.text.empty()) {
                        std::string err = JsonError(m.qid, "empty text");
                        ::send(cli, err.c_str(), err.size(), 0);
                        continue;
                    }

                    // 每个 speak 请求独立的 DemoState + decoder 线程
                    DemoState state;
                    state.decoder = &decoder;
                    char wav_path[128];
                    snprintf(wav_path, sizeof(wav_path), "/tmp/tts_out_%d.wav", m.qid);
                    state.output_path = wav_path;

                    state.decoder_thread = std::thread(decoder_thread_entry, &state);

                    // 绑定本次请求的 state 到全局指针，再启动生成
                    g_current_state = &state;

                    // 构造请求
                    Qwen3TTSTalkerRequest request;
                    request.text = m.text;
                    request.language = "auto";
                    request.instruct = m.instruct;
                    if (!m.speaker.empty()) {
                        // 音色名同时走两个字段，talker 内按 克隆表→voices/*.npy→spk_id 顺序解析：
                        // 预置 9 音色（serena/vivian/...）落在 spk_id，克隆音色（girl_base、
                        // 上传的自定义 .npy）走 voice_clone 路径
                        request.speaker = m.speaker;
                        request.ref_speaker = m.speaker;
                    } else {
                        request.ref_speaker = "girl_base";
                    }

                    if (talker.Process(request) != 0) {
                        g_current_state = nullptr;
                        fprintf(stderr, "[tts] talker process failed\n");
                        state.stop_requested = true;
                        state.token_cond.notify_one();
                        if (state.decoder_thread.joinable()) state.decoder_thread.join();
                        std::string err = JsonError(m.qid, "process failed");
                        ::send(cli, err.c_str(), err.size(), 0);
                        continue;
                    }

                    // 等待合成完成
                    {
                        std::unique_lock<std::mutex> lock(state.mutex);
                        state.cond.wait(lock, [&state]() { return state.finished; });
                    }

                    {
                        std::lock_guard<std::mutex> lock(state.mutex);
                        state.stop_requested = true;
                        state.token_cond.notify_one();
                    }
                    if (state.decoder_thread.joinable()) state.decoder_thread.join();
                    g_current_state = nullptr;

                    std::string resp = state.failed
                        ? JsonError(m.qid, "decode failed")
                        : JsonDone(m.qid, state.output_path);
                    fprintf(stderr, "[tts] speak done: failed=%d samples=%zu\n",
                            state.failed, state.audio_buffer.size());
                    ::send(cli, resp.c_str(), resp.size(), 0);
                } else if (m.cmd == "extract") {
                    // 板端声音克隆：PCM(24kHz mono float32 文件) → 2048 维向量 → voices/{name}.npy
                    std::string name;
                    if (!spk_encoder.Ready()) {
                        std::string err = JsonError(m.qid, "spk encoder not ready（缺 spk_embed.rknn，见 tools/HANDOFF_export.md）");
                        ::send(cli, err.c_str(), err.size(), 0);
                        continue;
                    }
                    if (!valid_voice_name(m.name, name)) {
                        std::string err = JsonError(m.qid, "bad voice name（小写字母/数字/_/-，1~24 位）");
                        ::send(cli, err.c_str(), err.size(), 0);
                        continue;
                    }

                    // 读 PCM 文件（web 层落盘的 24kHz mono float32 原始数据）
                    std::vector<float> pcm;
                    {
                        std::ifstream pf(m.pcm, std::ios::binary);
                        if (!pf) {
                            std::string err = JsonError(m.qid, "pcm file not found");
                            ::send(cli, err.c_str(), err.size(), 0);
                            continue;
                        }
                        pf.seekg(0, std::ios::end);
                        size_t bytes = (size_t)pf.tellg();
                        if (bytes < 4 || bytes % 4 != 0 || bytes / 4 < kSampleRate) {  // 至少 1 秒
                            std::string err = JsonError(m.qid, "pcm file bad（float32 且至少 1 秒）");
                            ::send(cli, err.c_str(), err.size(), 0);
                            continue;
                        }
                        pcm.resize(bytes / 4);
                        pf.seekg(0, std::ios::beg);
                        pf.read(reinterpret_cast<char*>(pcm.data()), bytes);
                    }

                    float embed[2048];
                    if (spk_encoder.Extract(pcm.data(), (int)pcm.size(), embed) != 0) {
                        std::string err = JsonError(m.qid, "extract failed");
                        ::send(cli, err.c_str(), err.size(), 0);
                        continue;
                    }

                    std::string voices_dir = std::string(model_dir) + "/voices";
                    ::mkdir(voices_dir.c_str(), 0755);  // 已存在则忽略
                    std::string npy_path = voices_dir + "/" + name + ".npy";
                    if (SpkEncoder_SaveNpy(npy_path, embed, 2048) != 0) {
                        std::string err = JsonError(m.qid, "save npy failed");
                        ::send(cli, err.c_str(), err.size(), 0);
                        continue;
                    }
                    fprintf(stderr, "[tts] 板端提取音色 %s 完成（|v|=%.2f）\n",
                            name.c_str(),
                            [&embed]() {
                                float s = 0.f;
                                for (int i = 0; i < 2048; ++i) s += embed[i] * embed[i];
                                return sqrtf(s);
                            }());
                    std::string resp = JsonVoiceSaved(m.qid, name);
                    ::send(cli, resp.c_str(), resp.size(), 0);
                } else if (m.cmd == "ping") {
                    std::string pong = JsonPong();
                    ::send(cli, pong.c_str(), pong.size(), 0);
                }
            }
        }
        ::close(cli);
        fprintf(stderr, "[tts] client disconnected\n");
    }

    ::close(fd);
    ::unlink(sock_path);
    return 0;
}
