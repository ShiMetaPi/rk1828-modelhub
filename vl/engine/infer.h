// infer.h — 推理后端接口。真实实现（Qwen2.5-VL）等模型包修复后接入；
// 当前用 MockInfer 点亮全链路（采集/协议/Web 都是真的，只有回答是假的）。
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vl {

struct AskRequest {
    int64_t qid = 0;
    std::string text;              // 用户问题（UTF-8）
    std::vector<uint8_t> jpeg;     // 提问时刻的最新帧（MJPG 帧）
};

struct InferStats {
    double vit_ms = 0;       // 视觉编码耗时
    double prefill_ms = 0;   // 文本+视觉 prefill
    double decode_ms = 0;    // 解码
    int tokens = 0;          // 生成 token 数
};

class InferEngine {
public:
    virtual ~InferEngine() {}
    virtual std::string name() = 0;
    virtual bool Init(std::string* err) = 0;
    // 同步执行一次问答；每生成一段就通过 on_token 流出（ UTF-8 文本片段）。
    // on_notice（可选）：非模型输出的系统提示（如"上下文已自动清空"），
    // 在 token 流开始前发出，Web 作为系统消息展示。
    // 返回 false 时 *err 为失败原因（会作为 error 事件发给 Web）。
    virtual bool Run(const AskRequest& req,
                     const std::function<void(const std::string&)>& on_token,
                     InferStats* stats, std::string* err,
                     const std::function<void(const std::string&)>& on_notice = {}) = 0;
    virtual void Reset() = 0;  // 清对话上下文
};

}  // namespace vl
