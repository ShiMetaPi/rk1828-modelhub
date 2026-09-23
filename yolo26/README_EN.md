[← Back](../README_EN.md) · [中文](README.md)

# 🎯 YOLO26 Real-time Vision

Real-time recognition from the camera in the page, with **detect / segment / pose tabs switched in one click**: detect draws boxes + classes, segment overlays a per-pixel instance mask, pose draws a 17-keypoint skeleton. Switching tabs swaps the engine's model in ~0.3 s — camera mode resumes streaming, upload mode re-runs the current image with the new model. You can also upload any image for single-frame inference.

<table>
<tr>
<td width="33%" align="center" valign="top">

**Detect** — boxes + classes

<img src="../docs/img/yolo26_det.png" alt="Detect tab: bus.jpg 5 objects" width="290">

</td>
<td width="33%" align="center" valign="top">

**Segment** — boxes + instance mask

<img src="../docs/img/yolo26_seg.png" alt="Segment tab: bus.jpg 4 objects with mask" width="290">

</td>
<td width="33%" align="center" valign="top">

**Pose** — 17-keypoint skeleton

<img src="../docs/img/yolo26_pose.png" alt="Pose tab: person skeleton" width="290">

</td>
</tr>
</table>

The models are the YOLO26n det / seg / pose triplets (COCO 80 classes), all W8A8 INT8-quantized, input 640×640. The engine ports the W8A8 post-processing of the three official rknn3-model-zoo examples (`yolo26` / `yolo26_segment` / `yolo26_pose`) into one resident C++ process: direct box regression (no DFL), sigmoid score names already in probability space, mask via coeff×proto **integer dot product** — no dequantization and no floating point anywhere.

The live feed uses a **frozen-frame** scheme: the canvas always shows the frame being analyzed, so masks align exactly with the image and there's no smear however fast the subject moves (the trade-off: the feed pace ≈ inference frame rate, not live-stream smoothness).

## Prerequisites

- Models go in `model/` (pulled and verified by `deploy.sh`): 6 files, ~**11.5 MB**.

| Item | On-board location |
|---|---|
| Models (yolo26n det / seg / pose, one rknn + weight each) | `<demo dir>/model/` |
| USB camera (optional) | UVC; without one, use the upload mode |

**Network**: deploy downloads the models from GitHub Releases (only 11.5 MB); for a proxy, set it up before running (use your own address/port):

```bash
export http_proxy=http://<proxy-addr>:<port> https_proxy=http://<proxy-addr>:<port>
```

## Run

```bash
# 1) Get the code (GitHub or Gitee; git pull if the repo already exists on the board)
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # faster in China

# 2) Pull models (~11.5 MB; resumable + MD5)
cd rk1828-modelhub/yolo26
sh deploy.sh

# 3) Start (the first run auto-builds the C++ engine, ~tens of seconds)
sh start.sh
```

The board browser auto-opens the page. From a PC: same subnet → open **http://<board-ip>:8092**; point-to-point → forward the port first, then open `http://127.0.0.1:<your-port>`:

```bash
ssh -L <your-port>:127.0.0.1:8092 root@<board-ip>
```

### Slow on the board? Download on a PC first

Open the [models-yolo26 Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-yolo26), download all 6 model files, and copy them into `model/`:

```bash
scp yolo26n-*.rknn yolo26n-*.weight root@<board-ip>:<repo-path>/yolo26/model/
```

Then run `sh deploy.sh` — files already present with correct MD5 are skipped.

- **Live**: click "start camera"; boxes and masks appear on the feed; top-right shows FPS / engine latency / page latency.
- **Switch task**: click the "detect / segment / pose" tab; the engine swaps models in ~0.3 s — camera mode pauses a frame then resumes, upload mode re-runs the current image.
- **Upload**: click the upload button to pick an image for single-frame inference, same result.

## When does the model load?

Same logic as the other demos: the service lives only while a page is open (5 s heartbeat keep-alive; multiple tabs don't interfere); **closing all pages → NPU released, service auto-exits after a 10 s grace**. Re-run `start.sh` (idempotent). When the NPU is held by another demo it errors explicitly — close that demo and retry. Only one frame is processed at a time (NPU is exclusive; busy frames are dropped, not queued).

<details>
<summary><b>Where to look when something breaks</b></summary>

| Symptom | On the board |
|---|---|
| Page won't open | `pgrep -af yolo26_server.py`, `curl 127.0.0.1:8092/api/status` |
| Stuck "loading model" | normally <1 s; log `tail /tmp/yolo26_start.log`, engine log `tail /tmp/yolo26_engine.stdout` |
| Camera fails | USB camera not seated (`lsusb`); or the browser started before the camera — reseat it and refresh |
| FPS well below 14 | check the "page" latency: high means something is stealing CPU in the background; high "engine" latency means the NPU is being disturbed |
| "NPU occupied" | another demo's page is still open; close it and start again |
| "Service exited" | all pages were closed. Re-run `sh start.sh` and refresh |

</details>
