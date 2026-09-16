# 🔊 Qwen3-TTS 语音合成 Demo

![开发中](https://img.shields.io/badge/status-开发中-orange.svg)

Qwen3_TTS 12Hz 1.7B 模型已转 rknn（4 个 rknn + embeds + tokenizer），但 demo 形态还没定。

**候选形态**：
1. CLI：`./tts_engine model_dir "你好"` → 输出 `output.wav`
2. 独立 web demo：页面输入文本 → 后端生成 wav → 页面 `<audio>` 播放
3. 嵌入聊天 demo：chat_web 回答后接 TTS 输出

等 demo 形态定下来后再补：
- `deploy.sh` 真正可用（资产清单 + MD5）
- `tts_engine/` 源码 + `start.sh`
- `tts_web/` 页面（如果选 web）

模型目录结构（已转好，待部署）：
```
tts/
├── code_predictor/
├── embeds/
├── speech_decoder/
├── talker/
├── text_projector/
└── deploy.sh (占位)
```