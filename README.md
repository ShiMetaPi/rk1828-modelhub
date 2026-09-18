<div align="center">

# RK1828 ModelHub

**一块 RK1828 NPU 板卡，多个大模型 demo**

模型全部在板端 NPU 上推理，浏览器就是操作界面。

![Platform](https://img.shields.io/badge/RK1828-NPU%20%C2%B7%20RKNN-1e293b.svg)

</div>

## Demo 列表

每个 demo 一个目录，模型、脚本、文档都自带，想玩哪个就进哪个：

<table>
<tr>
<td width="50%" align="center" valign="top">

### 💬 聊天

<img src="docs/img/chat_demo.png" alt="聊天 demo" width="420">

MiniCPM5-2B，W4A16 / W8A16 在线切换，流式输出，上下文自动摘要续命。

实测 **105 token/s**（W4） / **70 token/s**（W8）

**[→ chat/README.md](chat/README.md)**

</td>
<td width="50%" align="center" valign="top">

### 📷 视频问答

<img src="docs/img/vl_demo.png" alt="视频问答 demo" width="420">

Qwen2.5-VL-3B 多模态，接 USB 摄像头，对着实时画面提问。

问一句 **1 秒内**出答案

**[→ vl/README.md](vl/README.md)**

</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">

### 🔊 语音合成

<img src="docs/img/tts_demo.png" alt="语音合成 demo" width="420">

Qwen3-TTS-12Hz-1.7B，输入文字合成语音，自然语言控语气 + 9 音色切换。

一句 **1~4 秒**出语音

**[→ tts/README.md](tts/README.md)**

</td>
<td width="50%" align="center" valign="top">

### 📝 语音字幕

<img src="docs/img/asr_demo.png" alt="语音字幕 demo" width="420">

Qwen3-ASR，麦克风录音或上传音频，转写过程逐轮流式出字幕。

15 秒音频 **几秒**转完

**[→ asr/README.md](asr/README.md)**

</td>
</tr>
</table>

## 怎么跑

四个 demo 套路一样，就三步：**进目录 → 拉模型 → 启动**。

```bash
cd <demo 目录>                                         # chat / vl / tts / asr
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh deploy.sh     # 只拉这个 demo 需要的模型
sh start.sh                                            # 起网页服务
```

模型从 GitHub Releases 下载，`deploy.sh` 带断点续传和 MD5 校验，传到一半断了重跑就行；启动脚本会自己探测缺什么依赖，缺了直接告诉你。

浏览器打开对应端口：聊天 **8089** · 视频问答 **8080** · 语音合成 **8088** · 语音字幕 **8090**。具体步骤、模型大小、板上路径都在各自的 README 里，点进去照着做就行。

## 需要什么

| 东西 | 说明 |
|---|---|
| RK1828 NPU 板卡 | 5120MB 独立内存池；测试环境是 RK3588 EVB10 载板 + Debian 12 |
| USB 摄像头 | 只有视频问答 demo 需要，UVC 协议、支持 MJPG 1080p |
| 一台电脑 | 能 SSH 到板子、能开浏览器就行（Windows / Linux / Mac 都行） |

板子的软件要求：聊天 demo 要系统自带 `rkllm3-server`；视频问答和语音合成要在板上现编译一次 C++ 引擎（`g++` + RKNN3 运行库）；语音字幕直接用官方预编译二进制，零编译。启动脚本会自动探测这些依赖，缺什么直接告诉你。

## 仓库

GitHub 和 Gitee 同步更新：

- GitHub：<https://github.com/ShiMetaPi/rk1828-modelhub>
- Gitee：<https://gitee.com/ShiMetaPi_0/rk1828-modelhub>（国内 clone 快；模型只在 GitHub Releases，`deploy.sh` 支持 `BASE_URL` 或 `HTTPS_PROXY` 加速）
