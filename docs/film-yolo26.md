# YOLO26 从 0 到 1 部署流程（拍摄脚本）

> 镜头记录：板子零状态 → clone 项目 → 推代码 → 拉模型 → 编译启动 → 实时识别演示。
>
> 当前状态（2026-09-21）：板子 demo 已全部清空、/userdata 无模型；PC 镜像 `release_mirror/models-yolo26` 6 个文件已 MD5 核验与 deploy.sh 一致；代码已推送（`49ffb08`），clone 即最新蓝白版。
>
> **所有脚本幂等**——命令打错、中断，直接原样重跑即可，已就位的文件自动跳过。

---

## 0. 开拍前：两个常驻窗口（挂好就行，不入镜）

PC 开两个 Git Bash 窗口，拍摄全程保持运行：

```bash
# 窗口 A：模型下载站（把 PC 的 release_mirror 变成网站；支持断点续传）
python D:/hsx_workspace/rk1828/tools/range_server.py 8000 --directory D:/hsx_workspace/rk1828/release_mirror

# 窗口 B：传送隧道（板子访问它自己的 127.0.0.1:8000 = 访问这台电脑）
ssh -N -R 8000:localhost:8000 root@169.254.62.200
```

> 板子没有外网（网线直连电脑），模型包由电脑经隧道喂给它。窗口 B 断了拉模型会失败，重开再跑一次 deploy.sh 即断点续传。

---

## 0. 镜头⓪（拍 PC 屏幕）：备货——从 GitHub Releases 拉模型到电脑 · 拍 30~60 秒即可

```bash
cd D:\hsx_workspace\rk1828\release_mirror
gh release download models-yolo26 -R ShiMetaPi/rk1828-modelhub -D models-yolo26
```

> 板子没外网，模型包先由电脑从 Releases 备好，板上的 deploy.sh 再从电脑拉。本集镜像已备好并验过 MD5，此步为补拍插入镜头用（重跑加 `--clobber`）。

## 1. 镜头①（拍 PC 屏幕）：clone 项目 · ~1 分钟

Git Bash 里：

```bash
cd /d/
git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub film_yolo    # 国内快；GitHub 也行
cd film_yolo
ls    # 给镜头扫一眼：yolo26/ depth/ tts/ asr/ vl/ docs/ ...
```

**✅ 成功标志**：clone 无报错，`ls` 能看到各 demo 目录。

## 2. 镜头②（拍 PC 屏幕）：推代码到板子 · ~10 秒

```bash
tar cf - --exclude='__pycache__' -C yolo26 . | ssh root@169.254.62.200 "mkdir -p /root/yolo26_demo && tar xf - -C /root/yolo26_demo"
```

**✅ 成功标志**：几秒内静默返回（tar 流打包→SSH 传输→板端解包，电脑上无中间文件）。

## 3. 镜头③（拍板子屏幕）：拉模型 · ~10 秒

板子桌面（XFCE）打开终端，逐行输入：

```sh
cd /root/yolo26_demo
GITHUB_REPO=x BASE_URL=http://127.0.0.1:8000/models-yolo26 sh deploy.sh
```

> `GITHUB_REPO=x` 是占位符：`BASE_URL` 已把下载地址整个指到电脑镜像，仓库名不起作用，但脚本要求它必须有值。

**预期画面**（共 6 个文件、约 11MB）：

```
开始部署 YOLO26 模型 → /root/yolo26_demo/model
↓ yolo26n-seg.rknn
✔ /root/yolo26_demo/model/yolo26n-seg.rknn 就位
↓ yolo26n-seg.weight
✔ ...
（det / pose 同样，共 6 行 ✔）
全部完成。启动:  cd /root/yolo26_demo && sh start.sh
```

## 4. 镜头④（拍板子屏幕，主镜头）：启动 · 编译 ~1 分钟

```sh
sh start.sh
```

**预期画面**：

```
[yolo26] 首次运行，正在编译…      ← cmake / make 滚屏约 30~60 秒（可口播：首次现场编译引擎）
[yolo26] 编译完成
[yolo26] 启动 Web 服务 :8092     ← 2 秒后浏览器自动弹出 demo 页
```

页面打开后加载模型约十几秒出第一帧（进度有提示），之后进入实时识别。

## 5. 镜头⑤：演示

- **det / seg / pose** 三个页签各点一下（切换约 0.3 秒，实时 ~14 FPS：框 / 实例彩罩 / 关键点骨架）
- 演完**直接关浏览器窗口** → 约 10 秒后终端可见服务自动退出
- （可选加分镜头）终端跑 `rknn-smi info`，口播「NPU 已释放」

---

## 故障排查

| 现象 | 处理 |
|---|---|
| deploy 下载卡住 / 失败 | 窗口 B 隧道断了：重开隧道 → 再跑一次 deploy.sh（断点续传） |
| 页面摄像头黑屏 | USB 摄像头重插一次，页面 F5 |
| start.sh 提示「NPU 被占用」 | 有页面没关干净：关浏览器窗口，等 10 秒重跑 |
| 命令打错 / 中断 | 原样重跑（脚本幂等，已就位文件自动跳过） |
| 桌面突然弹回登录界面 | 已装看门狗，等 ~30 秒自动回桌面，重开终端继续 |

## 环境速查

| 项 | 值 |
|---|---|
| 板子 | 169.254.62.200，root 免密 SSH，离线（网线直连 PC） |
| 模型镜像 | `D:\hsx_workspace\rk1828\release_mirror\models-yolo26`（已核验） |
| demo 端口 | 8092（浏览器自动打开） |
| 代码版本 | `49ffb08`（GitHub + Gitee 同步） |
| PC 命令 shell | Git Bash（tar / ssh 管道兼容性最好） |
