// spk_encoder.h — Qwen3-TTS 声音克隆的板端说话人编码器
//
// 24kHz 单声道 PCM（截断/补零到 10s）→ log-mel(937x128) → spk_embed.rknn
// → 2048 维说话人向量 → 存 voices/{名字}.npy 给 talker 的克隆路径用。
//
// 预处理与 transformers 侧 EcapaTdnnFeatureExtractor 逐位对齐：
//   reflect pad 384×2 → STFT(n_fft=1024, hop=256, 周期 Hann, center=False)
//   → |spec| → mel 滤波器组（librosa Slaney 128×513，从数据文件加载）
//   → log(clamp(min=1e-5))
// 模型文件（model_dir 下，由 tools/export_spk_encoder.py + RKNN 转换产出）：
//   spk_embed.rknn / spk_embed.weight / spk_mel_128x513.f32
// 缺文件时 Init 返回非 0（功能降级不可用，不影响 TTS 主流程）。
#ifndef SPK_ENCODER_H
#define SPK_ENCODER_H

#include <string>

class SpkEncoder {
public:
    SpkEncoder();
    ~SpkEncoder();

    int Init(const std::string& model_dir, const char* device_id = NULL);
    bool Ready() const;

    // pcm：24kHz 单声道，任意长度（内部截断/补零到 10 秒）；out：2048 个 float
    int Extract(const float* pcm, int n_samples, float* out);

private:
    struct Impl;
    Impl* impl_;
};

// 把 2048 维向量写成 numpy v1 .npy（dtype '<f4'），talker/web 两侧同格式
int SpkEncoder_SaveNpy(const std::string& path, const float* v, int dim);

#endif  // SPK_ENCODER_H
