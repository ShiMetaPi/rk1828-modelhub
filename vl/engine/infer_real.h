// infer_real.h — Qwen2.5-VL-3B 真实推理后端（rknn3 API）。
// 预处理/多模态格式照抄官方参考实现（airockchip rknn-llm multimodal demo +
// HF Qwen2-VL pixel_values 排布）：expand2square 灰底 letterbox → 392×392 →
// CLIP 归一化 → temporal×2 → merge 序 patch 化 [784,1176] fp16 → vision.rknn
// → [196,2048] fp16 → LLM session MULTIMODAL 输入。
#pragma once
#include "infer.h"

namespace vl {

class RealInfer : public InferEngine {
public:
    explicit RealInfer(const std::string& model_dir) : model_dir_(model_dir) {}
    ~RealInfer() override;

    std::string name() override { return "qwen2.5-vl-3b"; }
    bool Init(std::string* err) override;
    bool Run(const AskRequest& req,
             const std::function<void(const std::string&)>& on_token,
             InferStats* stats, std::string* err,
             const std::function<void(const std::string&)>& on_notice = {}) override;
    void Reset() override;

    // 输入 180° 翻转（服务模式摄像头物理倒置时开；CLI 读正立图片必须关）。
    // 须在 Init 前调用。
    void SetFlip180(bool f) { flip180_ = f; }

private:
    class Impl;
    Impl* impl_ = nullptr;
    std::string model_dir_;
    bool flip180_ = false;
};

}  // namespace vl
