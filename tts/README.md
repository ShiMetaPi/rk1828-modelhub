[← 返回总览](../README.md)

# 🔊 Qwen3-TTS 语音合成 Demo

![端口](https://img.shields.io/badge/%E7%AB%AF%E5%8F%A3-8088-0e7490.svg)
![板上编译](https://img.shields.io/badge/C%2B%2B%20%C2%B7%20%E6%9D%BF%E4%B8%8A%E7%BC%96%E8%AF%91-1e293b.svg)
![语气指令](https://img.shields.io/badge/%E8%AF%AD%E6%B0%94%E6%8C%87%E4%BB%A4%20%C2%B7%209%20%E9%9F%B3%E8%89%B2-1e293b.svg)

输入一段文字，浏览器里点一下就能听合成语音。还能用一句自然语言控制语气（「用开心激动的语气说」「用悲伤带哭腔的语气说」），以及切换 9 个预置音色（含四川话、北京话）。

<img src="../docs/img/tts_demo.png" alt="语音合成 demo 界面" width="560">

核心是一条 4 模型管线（全部在 NPU 上跑）：`text_projector → talker → code_predictor → speech_decoder`，最后输出 24kHz 单声道 WAV。talker 是 1.7B CustomVoice，支持 instruction 控制。

## 准备

- 板子上要有 RKNN3 运行库 `librknn3_api.so`（`/usr/lib` 下）和头文件 `rknn3_api.h`（随 chat/vl demo 的 SDK 一起装）；首次跑 `start.sh` 会现编译 C++ 引擎，缺什么它会告诉你
- 模型放到 `/userdata/models/qwen3-tts/`（运行本目录下的 `deploy.sh` 一键拉取、校验、放到默认位置）：12 个文件**平铺在同一目录**（引擎按文件名读取，没有子目录）

| 东西 | 板子上的位置 |
|---|---|
| 模型（12 个：talker / code_predictor / speech_decoder / text_projection 的 rknn+weight，tokenizer.json，3 个 embed） | `/userdata/models/qwen3-tts/` |

## 跑起来

```bash
# 传到板子
ssh root@<板子IP> "mkdir -p /root/tts_demo"
scp -r tts/* root@<板子IP>:/root/tts_demo/

# 拉模型（在板上，约 4GB；断点续传 + MD5 校验，可重复跑）
ssh root@<板子IP> "GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh /root/tts_demo/deploy.sh"

# 启动（只起网页壳，模型等页面打开才加载）
ssh root@<板子IP> "sh /root/tts_demo/start.sh"
```

浏览器打开 **http://<板子IP>:8088**，输入文字（可选填「语气指令」、选「音色」），点「生成语音」即可。

## 模型什么时候加载？

跟聊天/视频 demo 一个逻辑：页面开着引擎才跑，页面关掉 75 秒后引擎自动退出、NPU 释放。NPU 被别的 demo 占着时引擎不会硬上，接口会报错，关掉那个 demo 再刷新即可。手动停：关掉页面等 75 秒自动释放，或 `curl 127.0.0.1:8088/api/bye`。

## 合成速度

「你好」约 1.4s，「欢迎使用语音合成」约 1.9s，15 字句约 3.8s。语气指令会改变韵律（悲伤比开心明显更慢）。

<details>
<summary><b>出问题了看哪里</b></summary>

| 现象 | 在板子上看 |
|---|---|
| 页面打不开 | `pgrep -af tts_engine.py`，`curl 127.0.0.1:8088/api/status` |
| 一直「加载中」 | 模型加载约 40s，等一等；引擎日志 `tail /tmp/tts_engine.stdout` |
| 生成报错 | `/tmp/tts_engine.stdout`（C++ 引擎）和网页壳日志 |
| 中文乱码 | 别用 Windows 本地 curl 直接发中文（控制台编码问题），走浏览器或板上 curl |

</details>

<details>
<summary><b>一些已知行为</b></summary>

- 语气指令走的是 talker 的 instruct 通道（`<|im_start|>user\n{指令}<|im_end|>` 拼在 prefill 最前），只在 1.7B CustomVoice 上有效
- 9 个音色：serena / vivian / uncle_fu / ryan / aiden / ono_anna / sohee / eric（四川话）/ dylan（北京话）；不选则默认 girl_base
- 输出 `/tmp/tts_out_<qid>.wav`，RIFF WAVE IEEE Float mono 24000Hz

</details>
