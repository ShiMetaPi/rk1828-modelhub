[← 返回总览](../README.md) · [English](README_EN.md)

# 📷 Qwen2.5-VL-3B 视频问答 Demo

接一个 USB 摄像头，浏览器里看实时画面，对着画面打字提问，回答流式蹦出来。

<img src="../docs/img/vl_demo.png" alt="视频问答 demo 界面" width="560">

核心是个 C++ 写的引擎（`engine/` 目录，编译出来叫 `vl_engine`），它负责摄像头采集、图像预处理、在 NPU 上跑视觉模型和多模态对话模型，再把结果流式吐给网页。

## 准备

- 板子上有 `g++` 和 libjpeg 开发头文件（没有就 `apt install -y g++ libjpeg-dev`）；deploy 末尾编译引擎时用，缺了会告诉你
- 一个 UVC USB 摄像头（支持 MJPG 1080p 就行，自动识别 `/dev/video*`，倒着装也没关系，页面会翻正）
- 模型和运行库由本目录下的 `deploy.sh` 一键拉取、校验：六个模型文件进 `model/`，四个 `lib*.so` 进 `lib/`（编译和运行都从这里找）

**网络**：deploy 阶段要从 GitHub Releases 下载约 4.9GB，板子得能上网；走代理的话先设好再跑（地址和端口换成自己的）：

```bash
export http_proxy=http://<代理地址>:<你的端口> https_proxy=http://<代理地址>:<你的端口>
```

## 跑起来

```bash
# 1) 拿代码（GitHub / Gitee 二选一；板子上已有仓库就 git pull）
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # 国内快

# 2) 拉模型 + 编译引擎（约 4.9GB；断点续传 + MD5 校验，中断了重跑接着下；下完自动编译 vl_engine，一分钟左右）
cd rk1828-modelhub/vl
sh deploy.sh

# 3) 启动（只起网页和守护，引擎等页面打开才加载）
sh start.sh
```

运行后板子浏览器自动打开页面（约 2 秒），等右上角状态点变绿（约 20 秒），画面出来就能提问了。从电脑访问：同网段直接开 **http://<板子IP>:8080**；点对点直连先在电脑上转发端口，再开 `http://127.0.0.1:<你的端口>`：

```bash
ssh -L <你的端口>:127.0.0.1:8080 root@<板子IP>
```

### 板子下载慢？在电脑上先下好

用浏览器打开 [models-vl Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-vl) 把 10 个文件全部下载：六个 `Qwen2.5-VL-3B-*` 模型拷进 `model/`，四个 `lib*.so` 拷进 `lib/`：

```bash
scp Qwen2.5-VL-3B-* root@<板子IP>:<仓库路径>/vl/model/
scp lib*.so root@<板子IP>:<仓库路径>/vl/lib/
```

超过 2GB 的文件在 Release 上是分片（`xxx.part-aa`、`xxx.part-ab`…），要把分片下齐、在电脑上拼回原文件再传：Windows `copy /b xxx.part-aa+xxx.part-ab xxx`，Linux/mac `cat xxx.part-* > xxx`。放好后照样跑 `sh deploy.sh`：已就位且 MD5 正确的文件直接跳过下载，接着编译引擎。

## 引擎什么时候加载？

跟聊天 demo 一个逻辑：页面开着引擎才跑；**页面关掉立即释放 NPU，宽限 10 秒（防刷新误杀）后整个服务自动退出**，重新运行 `start.sh` 即可（幂等，不会端口冲突）。NPU 被别的 demo 占着时引擎不会硬上，页面顶部会提示先关掉那个，关了自动重试。手动停：`sh start.sh stop`。

## 性能

问一句 1 秒内出答案；视觉编码一帧约 250ms，回答速度 44~46 token/s。NPU 占 2.3G 左右且多轮推理期间不增长；另外引擎要在系统内存里放 embed 表，会吃掉 0.7G 左右。

<details>
<summary><b>出问题了看哪里</b></summary>

| 现象 | 在板子上看 |
|---|---|
| 一直没画面 | `tail /tmp/vl_engine.log`（采集用的哪个 /dev/video）；摄像头换个 USB 口试试 |
| 引擎一直不加载 | 页面顶部横幅（多半是 NPU 被占了），或 `tail /tmp/vl_watch.log` |
| 回答报错 | `/tmp/vl_engine.log` 和 `/tmp/vl_web.log` |

</details>
