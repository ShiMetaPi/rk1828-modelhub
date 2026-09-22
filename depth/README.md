[← 返回总览](../README.md)

# 📏 深度相机

网页里看摄像头画面，点一下拍摄键，**出一张原图 + 一张深度图**：离得近的东西偏红、远的偏蓝。拍完还能拖滑块把原图和深度图叠在一起看，旁边一根热成像式的色条对照远近。没摄像头也能玩——上传任意 JPG/PNG，同样出深度图。

<img src="../docs/img/depth_demo.png" alt="深度相机 demo 界面" width="560">

模型是 Depth-Anything-V3-Base，官方拆成三个子模型接力跑，全部在 NPU 上：`local`（每张图独立提特征 token）→ `global`（跨 view 建模）→ `head`（出深度图）。引擎把官方 rknn3-model-zoo 的管线完整移植成常驻 C++ 进程，三个模型**共享一块 NPU 内部内存**，比各开各的省不少显存。

**输出是相对深度**：只保证「谁近谁远、近多少」的关系正确，不保证绝对米数（DA3-BASE 就是不吃相机内参的相对深度模型，页面上的数值不带米制含义）。要真·米制深度得换带内参输入的 metric 变体，另说。

## 准备

- 模型放 `model/` 子目录（运行本目录下的 `deploy.sh` 一键拉取、校验）：6 个文件约 **1.1GB**

| 东西 | 板子上的位置 |
|---|---|
| 模型（6 个：local / global / head 的 rknn+weight） | `<demo 目录>/model/` |
| USB 摄像头（可选） | UVC 协议即可；没有就用上传图片模式 |

**网络**：deploy 阶段要从 GitHub Releases 下载约 1.1GB，板子得能上网；走代理的话先设好再跑（地址和端口换成自己的）：

```bash
export http_proxy=http://<代理地址>:<你的端口> https_proxy=http://<代理地址>:<你的端口>
```

## 跑起来

```bash
# 1) 拿代码（GitHub / Gitee 二选一；板子上已有仓库就 git pull）
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # 国内快

# 2) 拉模型（约 1.1GB；断点续传 + MD5 校验，中断了重跑接着下）
cd rk1828-modelhub/depth
sh deploy.sh

# 3) 启动（首次运行会先自动编译 C++ 引擎，约几十秒）
sh start.sh
```

运行后板子浏览器自动打开页面。从电脑访问：同网段直接开 **http://<板子IP>:8091**；点对点直连先在电脑上转发端口，再开 `http://127.0.0.1:<你的端口>`：

```bash
ssh -L <你的端口>:127.0.0.1:8091 root@<板子IP>
```

### 板子下载慢？在电脑上先下好

用浏览器打开 [models-depth Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-depth) 把 6 个模型文件全部下载，拷进板子 demo 的 `model/`：

```bash
scp da3_base_*.rknn da3_base_*.weight root@<板子IP>:<仓库路径>/depth/model/
```

放好后照样跑 `sh deploy.sh`：已就位且 MD5 正确的文件直接跳过下载。

- **拍摄**：取景框是方形（模型输入就是方形，所见即所得），点红圆点出「原图 vs 深度图」+ 叠加对比
- **上传**：点虚线框或拖图进去，自动取画面中央的方形区域，效果相同

## 速度

实测：首次打开页面后台加载模型约 **7~10 秒**（取景的功夫就加载完了，拍摄时不用等）；之后**每张约 0.2 秒**——芯片级 pre ~15ms · local ~18ms · global ~52ms · head ~52ms · 上色 ~36ms。模型常驻 NPU 占用约 **1.2GB / 5GB**。

## 模型什么时候加载？

跟其他 demo 一个逻辑：页面开着服务才活着（每 5s 心跳保活，多标签页互不影响）；**页面全部关闭 → 释放 NPU、宽限 10 秒后服务自动退出**，重跑 `start.sh` 即可（幂等）。NPU 被别的 demo 占着时会明确报错，关掉那个 demo 再来。同一时刻只处理一张图（NPU 独占）。

<details>
<summary><b>出问题了看哪里</b></summary>

| 现象 | 在板子上看 |
|---|---|
| 页面打不开 | `pgrep -af depth_engine.py`，`curl 127.0.0.1:8091/api/status` |
| 一直「模型加载中」 | 首次 7~10 秒，等一等；日志 `tail /tmp/depth_start.log`、引擎日志 `tail /tmp/depth_engine.stdout` |
| 摄像头打开失败 | USB 摄像头没插好（`lsusb`）；或浏览器比摄像头先启动了——插好后刷新页面 |
| 第一张特别慢 | 那是在加载模型（约十几秒），之后每张约 0.2 秒 |
| 提示 NPU 被占用 | 其他 demo 页面还开着，关掉它再拍 |
| 提示「服务已退出」 | 页面曾全部关闭过。重跑 `sh start.sh` 后刷新页面 |

</details>
