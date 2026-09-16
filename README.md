<div align="center">

# RK1828 ModelHub

**一块 RK1828 NPU 板卡，多个大模型 demo**

模型全部在板端 NPU 上推理，浏览器就是操作界面，电脑上什么都不用装。

[![License](https://img.shields.io/badge/MIT-0e7490.svg)](LICENSE)
![Platform](https://img.shields.io/badge/RK1828-NPU%20%C2%B7%20RKNN-1e293b.svg)

</div>

## 三个 demo

每个 demo 完全自包含——自己的模型、自己的脚本、自己的文档。挑一个心仪的，按 README 跑 `deploy.sh` 即可。

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
<td colspan="2" align="center" valign="top">

### 🔊 语音合成

![开发中](https://img.shields.io/badge/status-开发中-orange.svg)

Qwen3-TTS-12Hz-1.7B 模型已转 rknn（4 个 rknn + embeds + tokenizer）。demo 形态（CLI / web / 集成）待定。

**[→ tts/README.md](tts/README.md)**

</td>
</tr>
</table>

## 部署模式（每个 demo 独立）

```bash
# 在板上：
cd /path/to/<demo>/     # chat/ 或 vl/
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh deploy.sh
sh start.sh
# 浏览器开 http://<板子IP>:<demo端口>
```

每个 demo 的 `deploy.sh` 只拉自己需要的模型，MD5 内联校验，断点续传——不用再回到顶层 `models/` 找脚本。

## 硬件

| 东西 | 说明 |
|---|---|
| RK1828 NPU 板卡 | 设备内存 5120MB 独立池；测试环境是 RK3588 EVB10 载板 + Debian 12 |
| USB 摄像头 | 仅视频问答 demo 需要，UVC 协议、支持 MJPG 1080p |
| 一台电脑 | 能 SSH 到板子、能开浏览器即可（Windows / Linux / Mac 都行） |

软件：聊天 demo 要求板子系统自带 `rkllm3-server`；视频问答 demo 要在板上编译一次（`g++` + libjpeg 头文件）。具体见各 demo 的 README。

## 仓库

两边同步更新：

- GitHub：<https://github.com/ShiMetaPi/rk1828-modelhub>
- Gitee：<https://gitee.com/ShiMetaPi_0/rk1828-modelhub>（国内 clone 快；模型只在 GitHub Releases，下载脚本支持 `BASE_URL` 或 `HTTPS_PROXY` 加速）

## License

代码 MIT；模型文件遵循各自原始许可，详见 [LICENSE](LICENSE)。