# 板端指南：不接电脑，从零装到日常用

> 这份文档写给**站在板子面前的人**。装好之后日常使用完全不需要电脑；
> 只有「从零安装」需要电脑帮忙一次（板子默认离线，模型包由电脑中转）。

## 一、板子信息

| 项 | 值 |
|---|---|
| 管理 IP | `169.254.62.200`（网线/USB 直连，root 免密或密码 `rk1828`） |
| 系统 | Debian 12（XFCE 桌面，开机自动登录 root） |
| NPU | 5120MB 独立内存池，**一次只够跑一个 demo** |
| 远程桌面 | 浏览器开 `http://169.254.62.200:6080`（noVNC，密码 rk1828） |

## 二、日常使用（装好后，零电脑）

**跑一个 demo = 一条命令：**

```sh
cd /root/vl_demo      && sh start.sh    # 视频问答  → 浏览器自动打开 :8080
cd /root/tts_demo     && sh start.sh    # 语音合成  :8088
cd /root/asr_demo     && sh start.sh    # 语音字幕  :8090
cd /root/depth_demo   && sh start.sh    # 深度相机  :8091
cd /root/yolo26_demo  && sh start.sh    # 实时识别  :8092
```

- 首次启动会现场编译 C++ 引擎（几分钟），之后都是秒起；首次加载模型约十几秒。
- **玩完直接关浏览器窗口**即可：页面关闭约 10 秒后服务自动退出、NPU 自动释放，不用敲任何命令。
- **换下一个 demo**：先关掉上一个的浏览器窗口，再跑下一个的 `start.sh`。
  NPU 一次只装得下一个 demo 的模型，这是硬限制；脚本会检测并提示「NPU 被占用」。
- 浏览器窗口就是全部界面；`start.sh` 需要保持那个终端开着（最小化没事，关掉终端=关掉服务）。

**录一段演示视频：**

```sh
sh /root/record.sh start yolo    # 开始录 → /root/yolo_月日_时分秒.mkv（1080p，硬编码不占 CPU）
sh /root/record.sh stop          # 停止，文件立即可播放
```

录完用 `scp root@169.254.62.200:/root/yolo_*.mkv .` 拷到电脑，或在 noVNC 里下载。

**常见问题（都是已知情况）：**

| 现象 | 处理 |
|---|---|
| 提示「NPU 被占用」 | 上一个 demo 的页面还没退干净。关掉它的浏览器窗口，等 10 秒再跑 |
| 摄像头黑屏/打不开 | USB 摄像头接触不良，重插一下，然后页面 F5（深度相机/实时识别没摄像头可切「上传图片」） |
| 桌面突然弹回登录界面 | 显示驱动偶发崩溃，已装看门狗，**等 30 秒左右自动回桌面**，无需操作 |
| 页面显示「服务已退出」 | 正常生命周期提示；重新跑 `sh start.sh` 后刷新页面 |

## 三、从零安装（全新板子 / 清空重来）

板子默认**没有外网**（只有和电脑的点对点连接），所以安装 = 电脑推代码 + 电脑中转模型包，全程约 30~60 分钟（大头是模型传输）。装完后回到第二节，不再需要电脑。

### 电脑侧（一次性）

```bash
# 1. 拿代码
git clone https://github.com/ShiMetaPi/rk1828-modelhub      # 或 Gitee 镜像
cd rk1828-modelhub

# 2. 下载 5 个模型包（每个 demo 一个 Release tag，共约 10GB）
for t in models-vl models-tts models-asr models-depth models-yolo26; do
  gh release download $t -D release_mirror/$t
done

# 3. 推代码到板子（Windows 注意：推完要在板子上补 chmod +x，见下）
for pair in "vl vl" "tts tts" "asr asr" "depth depth" "yolo26 yolo26"; do
  set -- $pair
  tar cf - --exclude='__pycache__' -C $1 . | \
    ssh root@169.254.62.200 "mkdir -p /root/$2_demo && tar xf - -C /root/$2_demo"
done

# 4. 起本地模型服务 + 反向隧道（板子离线，deploy.sh 的下载走这条隧道）
python -m http.server 8000 -d release_mirror &
ssh -N -R 8000:localhost:8000 root@169.254.62.200 &    # 保持开着
```

### 板子侧（在板子的终端里，或 ssh 进去）

```sh
# Windows 推过来的文件没有执行位，先补上
chmod +x /root/asr_demo/rknn_qwen3_asr_demo_online

# 每个 demo：一条 deploy.sh 拉模型（MD5 自动校验，断了重跑即续传）
cd /root/vl_demo     && GITHUB_REPO=x BASE_URL=http://127.0.0.1:8000/models-vl     sh deploy.sh
cd /root/tts_demo    && GITHUB_REPO=x BASE_URL=http://127.0.0.1:8000/models-tts    sh deploy.sh
cd /root/asr_demo    && GITHUB_REPO=x BASE_URL=http://127.0.0.1:8000/models-asr    sh deploy.sh
cd /root/depth_demo  && GITHUB_REPO=x BASE_URL=http://127.0.0.1:8000/models-depth  sh deploy.sh
cd /root/yolo26_demo && GITHUB_REPO=x BASE_URL=http://127.0.0.1:8000/models-yolo26 sh deploy.sh

# 恢复自定义音色（声音克隆的声纹文件，tts）
mkdir -p /userdata/models/qwen3-tts/voices && cp /root/tts_demo/voices/*.npy $_/

# 逐个验证（一次一个，关掉浏览器再验下一个）
sh /root/yolo26_demo/start.sh
```

**安装顺序**：vl 最先（tts 编译要用它的 `engine/sdk` 头文件和静态库），tts 次之，其余随意。

**磁盘布局**（为什么这样分）：

| 路径 | 内容 | 说明 |
|---|---|---|
| `/root/*_demo/` | 代码 + 小模型（asr 2.9G / depth 1.1G / yolo26 12M） | 根分区 |
| `/userdata/models/` | 大模型（vl 2.9G / tts 3.4G 平铺） | 独立分区，空间大 |
| `/userdata/tmp/` | deploy.sh 的下载缓存 | 下完自动挪走，可随时清 |

> 有外网的板子可以跳过隧道：直接 `GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh deploy.sh`，
> 或用 `BASE_URL` 指向任何镜像加速。

## 四、这套部署验证过什么

2026-09-21 板子清零重装走的就是本文档流程（清空板子 → 推代码 → deploy.sh 拉模型 → 逐个启动），
命令均来自实测路径。
