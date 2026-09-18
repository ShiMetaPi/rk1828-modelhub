[← 返回总览](../README.md)

# 💬 MiniCPM5-2B 聊天 Demo

![端口](https://img.shields.io/badge/%E7%AB%AF%E5%8F%A3-8089-0e7490.svg)
![免编译](https://img.shields.io/badge/%E7%BA%AF%20Python%20%C2%B7%20%E5%85%8D%E7%BC%96%E8%AF%91-1e293b.svg)
![量化](https://img.shields.io/badge/W4A16%20%7C%20W8A16%20%E5%9C%A8%E7%BA%BF%E5%88%87%E6%8D%A2-1e293b.svg)

跑在板子上的浏览器聊天界面：流式输出、多轮对话，上下文快满时自动把旧对话压成一段摘要接着聊（页面上会有提示条），右上角下拉可以在 W4A16 和 W8A16 之间切换。

<img src="../docs/img/chat_demo.png" alt="聊天 demo 界面" width="560">

## 准备

- 板子系统里要有 `rkllm3-server`；`start.sh` 会自动探测，缺了会告诉你
- **记忆功能（可选）**：`sh setup_memory.sh` 装好 mem0 环境后自动开启。长期事实（偏好、工作地点、之前聊过的话题等）跨轮不丢；名字/自我介绍类问题因模型 rlhf 固化仍是已知限制。默认不开（自动降级为仅会话内档案）

- 模型放到板子上（运行本目录下的 `deploy.sh` 一键拉取、校验、放到默认位置），程序默认从这些位置读：

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

<details>
<summary><b>出问题了看哪里</b></summary>

| 现象 | 在板子上看 |
|---|---|
| 页面提示 rkllm3-server 拉不起来 | `tail /tmp/rkllm3-server.log` |
| 页面没反应 | `pgrep -af rkllm3-server`，`curl 127.0.0.1:8081/v1/models` |
| 网页壳本身的日志 | `tail /tmp/chat_web.log` |

</details>

<details>
<summary><b>一些已知行为</b></summary>

- 模型上下文 2048 token。长期记忆走「两阶段问答」：每轮回答后台异步提炼一句用户信息（名字/偏好/事实），滚动保存在摘要区并注入每次提问；聊得再久，名字和偏好也能召回。原文只保留最近两轮，摘要超过十条自动合并，不会撑爆上下文
- `<think>` 思考模式默认关着，开了会啰嗦很多还慢
- 实际速度：W4 大约 105 token/s，W8 大约 70 token/s；记忆提炼在后台异步跑，不拖慢回复的首字

</details>
