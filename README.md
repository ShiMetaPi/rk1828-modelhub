<div align="center">

# RK1828 ModelHub

**一块 RK1828 NPU 板卡，跑两个大模型 demo**

模型全部在板端 NPU 上推理，浏览器就是操作界面，电脑上什么都不用装。

[![License](https://img.shields.io/badge/License-MIT-0e7490.svg)](LICENSE)
![Platform](https://img.shields.io/badge/RK1828-NPU%20%C2%B7%20RKNN-1e293b.svg)
![Models](https://img.shields.io/badge/MiniCPM5--2B%20%7C%20Qwen2.5--VL--3B-1e293b.svg)

</div>

## 两个 demo

<table>
<tr>
<td width="50%" align="center" valign="top">

### 💬 聊天

<img src="docs/img/chat_demo.png" alt="聊天 demo" width="420">

MiniCPM5-2B，W4A16 / W8A16 在线切换，流式输出，上下文快满时自动压缩成摘要接着聊。

实测 **105 token/s**（W4）

**[→ 去 chat/ 看怎么跑](chat/README.md)**

</td>
<td width="50%" align="center" valign="top">

### 📷 视频问答

<img src="docs/img/vl_demo.png" alt="视频问答 demo" width="420">

Qwen2.5-VL-3B 多模态，接一个 USB 摄像头，对着实时画面提问。

问一句 **1 秒内**出答案

**[→ 去 vl/ 看怎么跑](vl/README.md)**

</td>
</tr>
</table>

## 还提供了什么

- **模型一键部署**（[models/](models/)）—— 模型文件放在 GitHub Releases 上，一条命令完成下载、分卷合并、MD5 校验、部署到位，支持断点续传
- **实测性能数据**（[bench/](bench/)）—— 各模型的 NPU 内存占用、推理速度、主机内存开销，都是在板子上实测的

## 需要什么硬件

| 东西 | 说明 |
|---|---|
| RK1828 NPU 板卡 | NPU 设备内存 5120MB 独立池；测试环境是 RK3588 EVB10 载板 + Debian 12 |
| USB 摄像头 | 只在视频问答 demo 用到，UVC 协议、支持 MJPG 1080p 即可 |
| 一台电脑 | 能 SSH 到板子、能开浏览器就行（Windows / Linux / Mac 都可以） |

软件方面：聊天 demo 要求板子系统自带 `rkllm3-server`；视频问答 demo 要在板上编译一次（`g++` + libjpeg 头文件，板卡镜像一般都有）。具体见各 demo 的 README。

## 怎么跑起来

整体三步：**装模型 → 起 demo → 浏览器访问**。

**第 0 步**：确认电脑能 SSH 到板子（`ssh root@<板子IP>` 能登录）。

**第 1 步：部署模型**（两个 demo 都要先做这步）

```bash
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
cd rk1828-modelhub
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh models/download_models.sh chat-w4   # 装/换: chat-w4 / chat-w8 / vl / all
```

**第 2 步：起 demo**

- 聊天（免编译，两条命令）：[chat/README.md](chat/README.md)
- 视频问答（板上编译一次，约一分钟）：[vl/README.md](vl/README.md)

**第 3 步**：浏览器打开 `http://<板子IP>:8089`（聊天）或 `http://<板子IP>:8080`（视频问答），开聊。

## 仓库地址

两边同步更新，随便选一个：

- GitHub：<https://github.com/ShiMetaPi/rk1828-modelhub>
- Gitee：<https://gitee.com/ShiMetaPi_0/rk1828-modelhub>（国内 clone 快；模型文件只在 GitHub Releases，下载时可以给脚本配代理或 `BASE_URL` 加速，见 [models/README.md](models/README.md)）

## License

代码 MIT；模型文件遵循各自原始许可，详见 [LICENSE](LICENSE)。
