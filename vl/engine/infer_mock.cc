// infer_mock.cc — 假推理：真实延时分布（视觉 200ms + prefill 300ms + 逐段 decode），
// 回答内容标注 mock 并回显请求参数，方便确认"问题文本/帧数据"全链路无损到达后端。
#include "infer_mock.h"

#include <stdio.h>
#include <unistd.h>

namespace vl {
namespace {

std::vector<std::string> SplitChunks(const std::string& s, size_t n) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        // 不拆开 UTF-8 多字节字符
        size_t len = 0, take = 0;
        while (take < n && i + len < s.size()) {
            unsigned char c = s[i + len];
            size_t step = (c < 0x80) ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (i + len + step > s.size()) break;
            len += step;
            ++take;
        }
        if (len == 0) break;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

}  // namespace

bool MockInfer::Init(std::string*) { return true; }

bool MockInfer::Run(const AskRequest& req,
                    const std::function<void(const std::string&)>& on_token,
                    InferStats* stats, std::string*,
                    const std::function<void(const std::string&)>&) {
    usleep(200 * 1000);  // 模拟视觉编码
    stats->vit_ms = 213.0;
    usleep(300 * 1000);  // 模拟 prefill
    stats->prefill_ms = 289.0;

    char head[256];
    snprintf(head, sizeof(head),
             "[mock 第%d轮] 收到问题（%zu 字节），帧 %zu 字节。回答：",
             turns_ + 1, req.text.size(), req.jpeg.size());
    std::string answer = std::string(head) +
        "这是一条来自假推理引擎的回复，用来验证采集、协议和页面链路。"
        "等 Qwen2.5-VL 模型包修复后，同样的数据通路会接入真实模型："
        "视觉编码器读入提问时刻的最新帧，语言模型结合对话历史逐字生成回答。";
    turns_++;

    auto chunks = SplitChunks(answer, 4);
    for (size_t i = 0; i < chunks.size(); ++i) {
        on_token(chunks[i]);
        usleep(30 * 1000);  // 模拟 decode 87 tok/s 的节奏
    }
    stats->decode_ms = (double)chunks.size() * 30;
    stats->tokens = (int)chunks.size();
    return true;
}

}  // namespace vl
