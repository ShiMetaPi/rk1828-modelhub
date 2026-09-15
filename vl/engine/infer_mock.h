// infer_mock.h — 假推理后端：固定延迟 + 预置回答逐段流出，参数与真实后端完全一致
#pragma once
#include "infer.h"

namespace vl {

class MockInfer : public InferEngine {
public:
    std::string name() override { return "mock"; }
    bool Init(std::string* err) override;
    bool Run(const AskRequest& req,
             const std::function<void(const std::string&)>& on_token,
             InferStats* stats, std::string* err,
             const std::function<void(const std::string&)>& on_notice = {}) override;
    void Reset() override { turns_ = 0; }

private:
    int turns_ = 0;  // 第几轮提问（模拟多轮上下文存在）
};

}  // namespace vl
