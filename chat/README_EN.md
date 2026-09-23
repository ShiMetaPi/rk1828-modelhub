[← Back](../README_EN.md) · [中文](README.md)

# 💬 MiniCPM5-2B Chat Demo

A browser chat UI running on the board: streaming output, multi-turn conversation, and automatic summarization of old turns when the context is nearly full (with an on-page indicator). A dropdown in the top-right switches between W4A16 and W8A16.

<img src="../docs/img/chat_demo.png" alt="Chat demo UI" width="560">

## Prerequisites

- The board must have `rkllm3-server`; `start.sh` probes for it and tells you if it's missing.
- Models and the chat template are pulled and verified by `deploy.sh` in this directory, into the default locations:

| Item | On-board location |
|---|---|
| W4 model (four files: rknn / weight / embed.bin / tokenizer.gguf) | `<demo dir>/model/w4/` |
| W8 model (rknn / weight; shares vocab and embed with W4) | `<demo dir>/model/w8/` |
| Chat template minicpm5.jinja (already in the repo, placed by deploy) | `<demo dir>/model/` |

**Network**: deploy downloads ~4.4 GB from GitHub Releases, so the board needs internet. For a proxy, set it up before running (use your own address/port):

```bash
export http_proxy=http://<proxy-addr>:<port> https_proxy=http://<proxy-addr>:<port>
```

## Run

```bash
# 1) Get the code (GitHub or Gitee; git pull if the repo already exists on the board)
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # faster in China

# 2) Pull models (~4.4 GB; resumable + MD5; rerun to continue an interrupted download)
cd rk1828-modelhub/chat
sh deploy.sh

# 3) Start (this only launches the web shell; the model isn't loaded yet)
sh start.sh
```

The board browser auto-opens the page (~2 s); the model loads only after the page opens, so wait ~10 s before chatting. From a PC: same subnet → open **http://<board-ip>:8089**; point-to-point → forward the port first, then open `http://127.0.0.1:<your-port>`:

```bash
ssh -L <your-port>:127.0.0.1:8089 root@<board-ip>
```

### Slow on the board? Download on a PC first

Open the [models-chat Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-chat) in a browser, download all 6 model files, and copy them into the demo's `model/`. Note the two W8 files must be **renamed to drop `-w8`** when placed in `model/w8/`:

| Downloaded file | On-board location |
|---|---|
| MiniCPM5-2B.rknn / .weight / .embed.bin / .tokenizer.gguf | `chat/model/w4/` (names unchanged) |
| MiniCPM5-2B-w8.rknn / MiniCPM5-2B-w8.weight | `chat/model/w8/`, renamed to MiniCPM5-2B.rknn / MiniCPM5-2B.weight |

```bash
scp MiniCPM5-2B.* root@<board-ip>:<repo-path>/chat/model/w4/
```

Files over 2 GB are split on the Release (`xxx.part-aa`, `xxx.part-ab`, …); download all parts and reassemble on the PC before transfer: Windows `copy /b xxx.part-aa+xxx.part-ab xxx`, Linux/mac `cat xxx.part-* > xxx`. Then run `sh deploy.sh` as usual — files already present with correct MD5 are skipped.

## When does the model load?

The model is not resident; it follows the page. Page open → model stays; **closing the page releases the NPU immediately, and after a 10 s grace (against refresh-kills) the whole service exits**. Re-run `start.sh` to bring it back (idempotent, no port conflict). Even if the browser crashes without notifying, the board releases the model after 75 s and exits the service after 300 s.

In practice:

- Switch to another demo? Just close this tab and open the other.
- "NPU is occupied by another demo" → another demo's page is still open; close it, and this one retries automatically.
- Switching W4/W8 top-right: releases the old model then loads the new (~30–60 s); the conversation is cleared.

## Stop

```bash
sh <demo dir>/start.sh stop
```

<details>
<summary><b>Where to look when something breaks</b></summary>

| Symptom | On the board |
|---|---|
| Page says rkllm3-server won't start | `tail /tmp/rkllm3-server.log` |
| Page unresponsive | `pgrep -af rkllm3-server`, `curl 127.0.0.1:8081/v1/models` |
| Web shell logs | `tail /tmp/chat_web.log` |

</details>
