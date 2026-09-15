# RK1828 NPU LLM Demos

在 RK1828 板子（RK3588 主控 + NPU）上跑的两个 LLM demo。模型全在板端 NPU 上推理，浏览器就是操作界面，电脑上什么都不用装。

- **聊天** — MiniCPM5-2B，W4A16 / W8A16 两个量化版本在线切换，流式输出，聊多了自动把旧对话压缩成摘要接着聊 → [chat/](chat/)
- **视频问答** — Qwen2.5-VL-3B 多模态模型，接个 USB 摄像头，对着实时画面提问 → [vl/](vl/)

## 跑起来

先把模型部署到板子上（见 [models/](models/) 的说明），然后：

- 聊天：看 [chat/README.md](chat/README.md)，不用编译，两条命令
- 视频问答：看 [vl/README.md](vl/README.md)，在板子上编译一次，一分钟左右

性能和内存实测数据在 [bench/perf.md](bench/perf.md)。

## License

代码采用 MIT 协议；模型文件遵循各自的原始许可，见 [LICENSE](LICENSE) 末尾说明。
