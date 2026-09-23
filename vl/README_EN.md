[← Back](../README_EN.md) · [中文](README.md)

# 📷 Qwen2.5-VL-3B Video QA Demo

Plug in a USB camera, watch the live feed in the browser, type questions about the frame, and get streamed answers.

<img src="../docs/img/vl_demo.png" alt="Video QA demo UI" width="560">

The core is a C++ engine (in `engine/`, built as `vl_engine`) that handles camera capture, image preprocessing, runs the vision model and the multimodal dialogue model on the NPU, and streams results back to the page.

## Prerequisites

- `g++` and libjpeg dev headers on the board (otherwise `apt install -y g++ libjpeg-dev`); used when deploy compiles the engine at the end — it tells you if they're missing.
- A UVC USB camera (MJPG 1080p; auto-detected at `/dev/video*`; mounting it upside down is fine, the page flips it back).
- Models and runtime libs are pulled and verified by `deploy.sh`: six model files into `model/`, four `lib*.so` into `lib/` (used by both build and run).

**Network**: deploy downloads ~4.9 GB from GitHub Releases, so the board needs internet. For a proxy, set it up before running (use your own address/port):

```bash
export http_proxy=http://<proxy-addr>:<port> https_proxy=http://<proxy-addr>:<port>
```

## Run

```bash
# 1) Get the code (GitHub or Gitee; git pull if the repo already exists on the board)
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # faster in China

# 2) Pull models + build engine (~4.9 GB; resumable + MD5; rerun to continue; then auto-builds vl_engine, ~1 min)
cd rk1828-modelhub/vl
sh deploy.sh

# 3) Start (only the web shell and watcher; the engine loads when the page opens)
sh start.sh
```

The board browser auto-opens the page (~2 s); wait for the status dot to turn green (~20 s), then the feed appears and you can ask. From a PC: same subnet → open **http://<board-ip>:8080**; point-to-point → forward the port first, then open `http://127.0.0.1:<your-port>`:

```bash
ssh -L <your-port>:127.0.0.1:8080 root@<board-ip>
```

### Slow on the board? Download on a PC first

Open the [models-vl Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-vl), download all 10 files: six `Qwen2.5-VL-3B-*` models into `model/`, four `lib*.so` into `lib/`:

```bash
scp Qwen2.5-VL-3B-* root@<board-ip>:<repo-path>/vl/model/
scp lib*.so root@<board-ip>:<repo-path>/vl/lib/
```

Files over 2 GB are split on the Release (`xxx.part-aa`, `xxx.part-ab`, …); download all parts and reassemble on the PC: Windows `copy /b xxx.part-aa+xxx.part-ab xxx`, Linux/mac `cat xxx.part-* > xxx`. Then run `sh deploy.sh` — files already present with correct MD5 are skipped, and it continues to build the engine.

## When does the engine load?

Same logic as the Chat demo: the engine runs only while the page is open; **closing the page releases the NPU immediately, and after a 10 s grace (against refresh-kills) the whole service exits**. Re-run `start.sh` to bring it back (idempotent, no port conflict). When the NPU is held by another demo, the engine won't force its way in — the page shows a banner telling you to close the other demo first; it retries automatically. Manual stop: `sh start.sh stop`.

## Performance

Answers in under 1 second per question; vision encoding is ~250 ms per frame, decoding 44–46 token/s. The NPU holds ~2.3 GB, stable across turns; the engine also places the embed table in system memory (~0.7 GB).

<details>
<summary><b>Where to look when something breaks</b></summary>

| Symptom | On the board |
|---|---|
| No feed | `tail /tmp/vl_engine.log` (which /dev/video it used); try another USB port |
| Engine never loads | the top banner (usually the NPU is occupied), or `tail /tmp/vl_watch.log` |
| Answers error out | `/tmp/vl_engine.log` and `/tmp/vl_web.log` |

</details>
