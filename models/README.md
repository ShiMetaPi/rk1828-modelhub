[← 返回总览](../README.md)

# 📦 模型下载与部署

![规模](https://img.shields.io/badge/16%20%E4%B8%AA%E6%96%87%E4%BB%B6%20%C2%B7%20%E7%BA%A6%207.3G-0e7490.svg)
![校验](https://img.shields.io/badge/MD5%20%E6%A0%A1%E9%AA%8C%20%C2%B7%20%E6%96%AD%E7%82%B9%E7%BB%AD%E4%BC%A0-1e293b.svg)

模型文件比较大（最大单个 2.5GB），不适合放进 git 仓库，统一放在 GitHub Releases 上，用本目录的脚本一条命令拉下来、校验、放到 demo 期望的位置。

## 有哪些模型

| 目标 | 内容 | 大小 |
|---|---|---|
| `chat-w4` | MiniCPM5-2B W4A16 四件（rknn / weight / embed / tokenizer） | 约 1.9G |
| `chat-w8` | MiniCPM5-2B W8A16 两件（词表/embed 共用 W4 的包，所以会连 W4 一起装） | 约 4.4G |
| `vl` | Qwen2.5-VL-3B 六件 + 运行库四个 .so | 约 2.9G |

## 用法

在板子上（或先把仓库放到板子上再）执行：

```bash
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh models/download_models.sh vl        # 装视频问答的模型
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh models/download_models.sh chat-w4   # 装聊天（W4）
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh models/download_models.sh chat-w8   # 装聊天（W8，含共用件）
GITHUB_REPO=ShiMetaPi/rk1828-modelhub sh models/download_models.sh all       # 全装
```

脚本行为：

- 下载支持断点续传，中途断了直接重跑同一条命令，会接着下
- 超过 GitHub 单文件 2G 上限的模型（W8 的 weight）在 Releases 里是分卷存的，脚本会自动探测、下载全部分卷、合并、校验
- 每个文件装好后都做 MD5 校验，装好的文件重复跑会直接跳过
- 模型最终位置：聊天在 `/root/rknn_MiniCPM5_2B_demo/model/`（W8 在 `/root/w8a16/`），视频问答在 `/userdata/models/qwen2.5-vl-3b/`——和两个 demo 的默认路径一致，装完即用
- 下载缓存在 `/userdata/tmp/models`，装完自动清掉。合并大文件时需要额外空间，装 `chat-w8` 前确认 `/userdata` 有 5G 以上空闲

只想校验已装好的文件的话：

```bash
cd models && md5sum -c md5sum.txt
```

<details>
<summary><b>网络不好 / 想走加速代理？</b></summary>

下载源可以用 `BASE_URL` 覆盖，比如走 ghproxy 加速：

```bash
BASE_URL=https://ghproxy.cn/https://github.com/ShiMetaPi/rk1828-modelhub/releases/download/models \
  sh models/download_models.sh vl
```

</details>
