<div align="center">

# RK1828 ModelHub

[中文](README.md) | **English**

All models infer on the on-board NPU; the browser is the only UI.

![Platform](https://img.shields.io/badge/RK1828-NPU%20%C2%B7%20RKNN-1e293b.svg)

</div>

## Overview

A collection of large-model demos running on the **RK1828 NPU board**, covering speech, vision, document parsing, and other common scenarios. All models infer locally on the on-board NPU; the browser is the only UI, and daily use needs no command line.

- **Hardware**: RK1828 NPU plus RK3588; details at <https://www.shimetapi.cn/>.
- **Positioning**: a one-click-deploy demo suite for the board — `sh deploy.sh` pulls the models, `sh start.sh` starts the service; works out of the box.
- **Model conversion**: models are first converted to `.rknn` with [RKNN-Toolkit3](https://github.com/airockchip/rknn-toolkit3), then ported into C++ engines following the official [rknn3-model-zoo](https://github.com/airockchip/rknn3-model-zoo) examples and run on the RKNPU.

## Demos

Covering LLM, VL, TTS and more — one-click deploy; details below:

<table>
<tr>
<td width="50%" align="center" valign="top">

### 💬 Chat

<img src="docs/img/chat_demo.png" alt="Chat demo" width="420">

MiniCPM5-2B, W4A16 / W8A16 switching on the fly, streaming output.

**105 token/s** (W4) / **70 token/s** (W8) measured

**[→ chat/README_EN.md](chat/README_EN.md)**

</td>
<td width="50%" align="center" valign="top">

### 📷 Video Q&A

<img src="docs/img/vl_demo.png" alt="Video QA demo" width="420">

Qwen2.5-VL-3B multimodal, USB camera, ask about the live feed.

Answers in **under 1 second**

**[→ vl/README_EN.md](vl/README_EN.md)**

</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">

### 🔊 Text-to-Speech

<img src="docs/img/tts_demo.png" alt="TTS demo" width="420">

Qwen3-TTS-12Hz-1.7B, text to speech, natural-language emotion control + 9 voices.

**1–4 s** per sentence

**[→ tts/README_EN.md](tts/README_EN.md)**

</td>
<td width="50%" align="center" valign="top">

### 📝 Speech Subtitles

<img src="docs/img/asr_demo.png" alt="ASR demo" width="420">

Qwen3-ASR, record or upload audio, subtitles stream out turn by turn.

A 15 s clip transcribed in **a few seconds**

**[→ asr/README_EN.md](asr/README_EN.md)**

</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">

### 📏 Depth Camera

<img src="docs/img/depth_demo.png" alt="Depth camera demo" width="420">

Depth-Anything-V3, one shot gives "original vs depth map", near-red far-blue.

**0.2 s** per depth map

**[→ depth/README_EN.md](depth/README_EN.md)**

</td>
<td width="50%" align="center" valign="top">

### 🎯 Real-time Vision

<img src="docs/img/yolo26_pose.png" alt="Real-time vision demo" width="420">

YOLO26n **detect / segment / pose** in three tabs, one-click switching.

Real-time **14 FPS** (12–15 ms per NPU frame)

**[→ yolo26/README_EN.md](yolo26/README_EN.md)**

</td>
</tr>
<tr>
<td width="50%" align="center" valign="top">

### 📄 Document Parsing

<img src="docs/img/ocr_text.png" alt="Document parsing demo" width="420">

PaddleOCR-VL multimodal OCR, text / table / chart / formula in four modes, tables and charts restored to structured data.

**1–2 s** per result

**[→ paddleocr_vl/README_EN.md](paddleocr_vl/README_EN.md)**

</td>
<td width="50%" align="center" valign="middle">

to be continued...

</td>
</tr>
</table>

## How to Run

All seven demos follow the same four steps: **get code → enter directory → pull models → start**.

```bash
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git   # or Gitee (faster in China)
cd rk1828-modelhub/<demo dir>                              # chat / vl / tts / asr / depth / yolo26 / paddleocr_vl
sh deploy.sh                                                # pull only this demo's models
sh start.sh                                                 # start the web service
```

Models are downloaded from GitHub Releases; `deploy.sh` does resumable transfer + MD5 checks, so rerun after an interrupted download. The start script probes missing dependencies and tells you directly. If the board downloads slowly or has no network, download the models on a PC first and copy them into the demo's `model/` — see each demo's README ("Slow on the board? Download on a PC first").

Open the browser at the corresponding port: Chat **8089** · Video QA **8080** · TTS **8088** · Speech Subtitles **8090** · Depth Camera **8091** · Real-time Vision **8092** · Document Parsing **8093**. Steps, model sizes, and on-board paths are in each README.

## Requirements

| Item | Notes |
|---|---|
| RK1828 NPU board | 5120 MB dedicated memory pool; test environment is an RK3588 EVB10 carrier + Debian 12 |
| USB camera | required by Video QA, Depth Camera, and Real-time Vision; UVC, MJPG 1080p; Depth Camera and Real-time Vision also accept image upload without a camera |
| PC (optional) | only for the first install: SSH file transfer, deploy.sh / start.sh; day-to-day use needs no PC — operate from the board's browser. If model download is slow, download on a PC then push to the board and run deploy |

Software requirements: Chat needs the system `rkllm3-server`; Video QA, TTS, Depth Camera, Real-time Vision, and Document Parsing compile a C++ engine once on the board (`g++`/`cmake` + RKNN3 runtime); Speech Subtitles uses the official prebuilt binary, zero compilation. The start script probes all of these and tells you what's missing.

## Repositories

GitHub and Gitee are kept in sync:

- GitHub: <https://github.com/ShiMetaPi/rk1828-modelhub>
- Gitee: <https://gitee.com/ShiMetaPi_0/rk1828-modelhub> (faster clone in China; models are on GitHub Releases only — `deploy.sh` supports `BASE_URL` or `HTTPS_PROXY` to speed up)

## Models

All models ship via GitHub Releases; `deploy.sh` pulls them with one click (resumable + MD5):

- **[Our Releases](https://github.com/ShiMetaPi/rk1828-modelhub/releases)** (recommended) — one-click deploy; one `models-xxx` tag per demo.
- **[rknn3-model-zoo](https://github.com/airockchip/rknn3-model-zoo)** — Rockchip's official model zoo, the source of most demos here; its README includes **Baidu Netdisk** download links, faster for large models in China.
