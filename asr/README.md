[← 返回总览](../README.md)

# 📝 Qwen3-ASR 语音字幕

![端口](https://img.shields.io/badge/%E7%AB%AF%E5%8F%A3-8090-0e7490.svg)
![官方二进制](https://img.shields.io/badge/%E5%AE%98%E6%96%B9%E4%BA%8C%E8%BF%9B%E5%88%B6%20%C2%B7%20%E6%97%A0%E9%9C%80%E7%BC%96%E8%AF%91-1e293b.svg)
![流式字幕](https://img.shields.io/badge/%E8%BE%B9%E8%BD%AC%E8%BE%B9%E5%87%BA%E5%AD%97%20%C2%B7%20%E9%BB%91%E5%AD%97%E5%B7%B2%E7%A1%AE%E8%AE%A4%20%E7%81%B0%E5%AD%97%E8%BF%98%E5%9C%A8%E6%94%B9-6366f1.svg)
![双模式](https://img.shields.io/badge/%E9%BA%A6%E5%85%8B%E9%A3%8E%E5%BD%95%E9%9F%B3%20%2B%20%E4%B8%8A%E4%BC%A0%E9%9F%B3%E9%A2%91-8b5cf6.svg)

对着麦克风说一段话，停止后**字幕像打字一样逐轮流出来**：已经确定的句子是黑色正文、固定不动越来越长；正在识别的那半句先用灰色草稿显示，随语音不断修正、改准了才「转正」变黑。也可以上传音频文件（wav/mp3/flac/m4a…），效果相同。

<img src="../docs/img/asr_demo.png" alt="语音字幕 demo 界面" width="560">

整条管线在 NPU 上跑：音频编码器 + Qwen3-0.6B 语言模型（8 核 core_mask）。网页壳把官方 `rknn_qwen3_asr_demo_online` 二进制的逐轮输出（`commit_add_text` / `unfix_text`）解析成 SSE 事件推给浏览器，**二进制和运行库直接用官方包里的，板上零编译**。

## 准备

- 本目录自带官方 demo 包的运行件：`rknn_qwen3_asr_demo_online`（二进制）、`lib/`（librknn3_api 等）、`mel_128_filters.txt`（特征滤波器，必须在运行目录下）
- 模型放 `model/` 子目录（运行本目录下的 `deploy.sh` 一键拉取、校验）：6 个文件约 2.4GB

| 东西 | 板子上的位置 |
|---|---|
| 模型（6 个：encoder_online 和 llm 的 rknn+weight，tokenizer.gguf，embed.bin） | `<demo 目录>/model/` |

## 跑起来

```bash
# 传到板子
ssh root@<板子IP> "mkdir -p /root/asr_demo"
scp -r asr/* root@<板子IP>:/root/asr_demo/

# 拉模型（在板上，约 2.4GB；断点续传 + MD5 校验，可重复跑）
ssh root@<板子IP> "GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh /root/asr_demo/deploy.sh"

# 启动
ssh root@<板子IP> "sh /root/asr_demo/start.sh"
```

运行后板子浏览器自动打开页面；也可手动开 **http://<板子IP>:8090**。

- **麦克风录音**：点红圆点开始（实时波形 + 计时），再点停止 → 自动解码提交 → 字幕流出。需要浏览器安全上下文：板上 chromium（127.0.0.1）没问题；从 PC 用裸 IP 访问时麦克风被禁，用上传模式
- **上传音频**：点虚线框或把文件拖进去（wav / mp3 / flac / ogg / m4a，浏览器里解码成 16kHz 单声道再提交）

## 转写速度

实测（15 秒英文测试音频，16 轮）：每轮芯片级 **audio ~33ms · ttft ~70ms**，整段几秒出完；首轮前要加载模型——**冷启动约 1 分钟，之后约 15 秒**（权重留在 page cache）。单段音频上限 17 分钟。

## 模型什么时候加载？

跟其他 demo 一个逻辑：页面开着服务才活着（每 5s 心跳保活，多标签页互不影响）；**页面全部关闭 → 释放 NPU、宽限 10 秒后服务自动退出**，重跑 `start.sh` 即可（幂等）。NPU 被别的 demo 占着时会明确报错，关掉那个 demo 再来。手动停：重跑 `start.sh` 自动停旧实例，或 `curl 127.0.0.1:8090/api/bye`。同一时刻只跑一个转写任务（NPU 独占）。

<details>
<summary><b>出问题了看哪里</b></summary>

| 现象 | 在板子上看 |
|---|---|
| 页面打不开 | `pgrep -af asr_engine.py`，`curl 127.0.0.1:8090/api/status` |
| 一直「加载模型」 | 冷启动约 1 分钟，等一等；服务日志 `tail /tmp/asr_start.log` |
| 点按钮没反应 / 提示「服务已退出」 | 页面曾全部关闭过。页面开着时每 5s 心跳保活不会退；重跑 `sh start.sh` 后刷新页面 |
| 提示 NPU 被占用 | 其他 demo 页面还开着，关掉它再提交 |
| 上传后提示解码失败 | 换常见格式（wav/mp3）；m4a/ogg 取决于浏览器解码器 |
| 转写结果是空的 | 音频太短或没人声；录长一点（≥3 秒清晰人声） |

</details>

<details>
<summary><b>一些实现细节</b></summary>

- 引擎用 `stdbuf -oL` 行缓冲启动官方二进制——它是块缓冲的，不加这个整段转写会一次性涌出来，没有逐轮流式效果
- 浏览器负责音频标准化：MediaRecorder/文件 → `decodeAudioData` → `OfflineAudioContext` 重采样 16kHz 单声道 → PCM16 WAV 上传，板上不需要 sox/ffmpeg
- SSE 协议：`phase`（加载中）/ `round`（第 N 轮）/ `commit`（定稿追加）/ `unfix`（未定稿刷新）/ `perf` / `final`（全文，权威结果，结束后整段替换）
- 事件带 `jid`，页面只渲染自己提交的任务；断线 EventSource 自动重连
- core_mask 两级都 `0xFF`（8 核转换的模型，单核 mask 会报 `not match with npu core number 8`）
- 官方包里的离线版二进制（`rknn_qwen3_asr_demo`）和 `encoder.rknn/weight` 没用到——online 版本身就带流式输出且精度路径相同

</details>
