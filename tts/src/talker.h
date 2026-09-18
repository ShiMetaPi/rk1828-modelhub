#ifndef QWEN3_TTS_TALKER_H_
#define QWEN3_TTS_TALKER_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <map>
#include <string>

#include "rknn3_api.h"
#include "float16.h"

#define CODEC_MAX_CONTEXT_LEN 32
#define CODEC_MAX_NEW_TOKENS 15
#define QWEN3_TALKER_QUANT_GROUP 32
#define QWEN3_TTS_MAX_CONTEXT_LEN 2048
#define MAX_TRAILING_TOKENS 1024
#define QWEN3_TTS_EMBED_DIM 2048
#define TEXT_PROJECT_EMBED_DIM 2048
#define TEXT_VOCAB_SIZE 151936
#define TALKER_VOCAB_SIZE 3072
#define CODEC_VOCAB_SIZE 2048
#define TTS_BOS_TOKEN_ID 151672
#define TTS_EOS_TOKEN_ID 151673
#define TTS_PAD_TOKEN_ID 151671
#define CODEC_BOS_ID 2149
#define CODEC_EOS_TOKEN_ID 2150
#define CODEC_THINK_ID 2154
#define CODEC_NOTHINK_ID 2155
#define CODEC_PAD_ID 2148
#define CODEC_THINK_BOS_ID 2156
#define CODEC_THINK_EOS_ID 2157

typedef void (*TalkerCallback)(std::vector<int>* tokens, bool talker_is_generating, void* userdata);

struct embedding_info {
    int fd = -1;
    float16* embedding_data = NULL;
    int embedding_dim = 0;
    int vocab_size = 0;
};

const rknn3_sampling_params TALKER_SAMPLE_PARAMS = {
    .top_k = 50,
    .top_p = 1,
    .temperature = 0.9f,
    .repeat_penalty = 1.05f,
    .frequency_penalty = 0.0f,
    .presence_penalty = 0.0f
};

const rknn3_sampling_params CODE_PREDICTOR_SAMPLE_PARAMS = {
    .top_k = 50,
    .top_p = 1,
    .temperature = 0.9f,
    .repeat_penalty = 1.0f,
    .frequency_penalty = 0.0f,
    .presence_penalty = 0.0f
};

const std::unordered_map<std::string, int> spk_id = {
    {"serena", 3066}, {"vivian", 3065}, {"uncle_fu", 3010},
    {"ryan", 3061}, {"aiden", 2861}, {"ono_anna", 2873},
    {"sohee", 2864}, {"eric", 2875}, {"dylan", 2878}
};

const std::unordered_map<std::string, int> codec_language_id = {
    {"chinese", 2055}, {"english", 2050}, {"german", 2053}, {"italian", 2070},
    {"portuguese", 2071}, {"spanish", 2054}, {"japanese", 2058}, {"korean", 2064},
    {"french", 2061}, {"russian", 2069}, {"beijing_dialect", 2074}, {"sichuan_dialect", 2062}
};

static std::map<std::string, std::string> spk_is_dialect = {
    {"serena", "false"}, {"vivian", "false"}, {"uncle_fu", "false"},
    {"ryan", "false"}, {"aiden", "false"}, {"ono_anna", "false"},
    {"sohee", "false"}, {"eric", "sichuan_dialect"}, {"dylan", "beijing_dialect"}
};

struct Qwen3TTSTalkerConfig {
    std::string model_dir;
    std::string device_id;
    TalkerCallback callback = NULL;
    void* userdata = NULL;
};

struct Qwen3TTSTalkerRequest {
    std::string text;
    std::string instruct;
    std::string language = "auto";
    std::string speaker;
    std::string ref_speaker = "girl_base";
    int non_streaming_mode = 0;
};

class Qwen3TTSTalker {
public:
    Qwen3TTSTalker();
    ~Qwen3TTSTalker();

    int Init(const Qwen3TTSTalkerConfig& config);
    int Process(const Qwen3TTSTalkerRequest& request);
    int Abort();
    void Release();

private:
    struct Impl;
    Impl* impl_;
};

#endif  // QWEN3_TTS_TALKER_H_
