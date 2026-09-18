#ifndef QWEN3_TTS_SPEECH_DECODER_H_
#define QWEN3_TTS_SPEECH_DECODER_H_

#include <stdint.h>

#include <string>
#include <vector>

class Qwen3TTSSpeechDecoder {
public:
    struct Config {
        std::string model_dir;
        std::string device_id;
        std::string model_name = "speech_decoder";
    };

    Qwen3TTSSpeechDecoder();
    ~Qwen3TTSSpeechDecoder();

    int Init(const Config& config);
    int Decode(const std::vector<int32_t>& codes, std::vector<float>* audio_values);
    void Release();

private:
    struct Impl;
    Impl* impl_;
};

#endif  // QWEN3_TTS_SPEECH_DECODER_H_
