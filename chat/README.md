# MiniCPM5-2B 聊天 Demo

跑在板子上的浏览器聊天界面：流式输出、多轮对话，上下文快满时自动把旧对话压成一段摘要接着聊（页面上会有提示条），右上角下拉可以在 W4A16 和 W8A16 之间切换。纯 Python 标准库写的，不用编译。

## 准备

- 板子系统里要有 `rkllm3-server`（终端里 `which rkllm3-server` 能找到就行）
- 模型已经放到板子上（怎么放见仓库 models/ 的说明），程序默认从这些位置读：

| 东西 | 板子上的位置 |
|---|---|
| W4 模型（四个文件：rknn / weight / embed.bin / tokenizer.gguf） | `/root/rknn_MiniCPM5_2B_demo/model/` |
| W8 模型（rknn / weight，词表和 embed 跟 W4 共用） | `/root/w8a16/` |
| 聊天模板 minicpm5.jinja（本目录里就有） | `/root/rknn_MiniCPM5_2B_demo/` |

## 跑起来

```bash
# 传到板子
ssh root@<板子IP> "mkdir -p /root/chat_web"
scp -r chat/chat_web/* chat/minicpm5.jinja root@<板子IP>:/root/chat_web/

# 放好聊天模板（跑过 models 部署脚本的话可跳过这步）
ssh root@<板子IP> "mkdir -p /root/rknn_MiniCPM5_2B_demo && cp /root/chat_web/minicpm5.jinja /root/rknn_MiniCPM5_2B_demo/"

# 启动（这时只起了网页壳，还没加载模型）
ssh root@<板子IP> "sh /root/chat_web/start.sh"
```

浏览器打开 **http://<板子IP>:8089**。模型在页面打开后才开始加载，等十几秒就能聊了。

## 模型什么时候加载？

模型不常驻，跟着网页走：页面开着模型就一直在，页面一关 NPU 马上释放（就算浏览器直接崩了，板子也会在 75 秒后自己把模型释放掉）。

几个实际效果：

- 想换到视频问答 demo？关掉这个标签页、开那个就行
- 如果页面提示"NPU 已被其他 demo 占用"，说明另一个 demo 的页面还开着——关掉它，这边会自动重试，不用手动干预
- 右上角切换 W4/W8：先释放旧模型再加载新的，大概 30~60 秒，对话会清空

## 停掉

```bash
ssh root@<板子IP> "sh /root/chat_web/start.sh stop"
```

## 出问题了看哪里

| 现象 | 在板子上看 |
|---|---|
| 页面提示 rkllm3-server 拉不起来 | `tail /tmp/rkllm3-server.log` |
| 页面没反应 | `pgrep -af rkllm3-server`，`curl 127.0.0.1:8081/v1/models` |
| 网页壳本身的日志 | `tail /tmp/chat_web.log` |

## 一些已知行为

- 模型上下文 2048 token，聊满了自动清理：旧对话变成一段文字摘要塞回去，最近两轮保留原文，页面出一条提示条。跨清理模型仍记得之前聊过的内容，可以放心长聊
- `<think>` 思考模式默认关着，开了会啰嗦很多还慢
- 实际速度：W4 大约 105 token/s，W8 大约 70 token/s
