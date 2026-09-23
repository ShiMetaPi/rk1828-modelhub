[← Back](../README_EN.md) · [中文](README.md)

# 📏 Depth Camera

Watch the camera in the page, click the shutter, and get **an original image + a depth map**: near objects lean red, far ones lean blue. After shooting you can drag a slider to overlay the original and the depth map, with a thermal-style color bar for distance reference. No camera? Upload any JPG/PNG and get a depth map just the same.

<img src="../docs/img/depth_demo.png" alt="Depth camera demo UI" width="560">

The model is Depth-Anything-V3-Base, split by the official repo into three sub-models that run in sequence, all on the NPU: `local` (per-image feature tokens) → `global` (cross-view modeling) → `head` (depth map). The engine ports the official rknn3-model-zoo pipeline into a resident C++ process; the three models **share one block of NPU internal memory**, saving a good deal of VRAM versus separate allocations.

**The output is relative depth**: only the "who's nearer, by how much" relation is guaranteed, not absolute meters (DA3-BASE is a relative-depth model that takes no camera intrinsics; the on-page values carry no metric meaning). True metric depth needs the intrinsics-aware variant — a separate topic.

## Prerequisites

- Models go in `model/` (pulled and verified by `deploy.sh`): 6 files, ~**1.1 GB**.

| Item | On-board location |
|---|---|
| Models (6: local / global / head rknn+weight) | `<demo dir>/model/` |
| USB camera (optional) | UVC; without one, use the upload mode |

**Network**: deploy downloads ~1.1 GB from GitHub Releases, so the board needs internet. For a proxy, set it up before running (use your own address/port):

```bash
export http_proxy=http://<proxy-addr>:<port> https_proxy=http://<proxy-addr>:<port>
```

## Run

```bash
# 1) Get the code (GitHub or Gitee; git pull if the repo already exists on the board)
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # faster in China

# 2) Pull models (~1.1 GB; resumable + MD5; rerun to continue)
cd rk1828-modelhub/depth
sh deploy.sh

# 3) Start (the first run auto-builds the C++ engine, ~tens of seconds)
sh start.sh
```

The board browser auto-opens the page. From a PC: same subnet → open **http://<board-ip>:8091**; point-to-point → forward the port first, then open `http://127.0.0.1:<your-port>`:

```bash
ssh -L <your-port>:127.0.0.1:8091 root@<board-ip>
```

### Slow on the board? Download on a PC first

Open the [models-depth Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-depth), download all 6 model files, and copy them into `model/`:

```bash
scp da3_base_*.rknn da3_base_*.weight root@<board-ip>:<repo-path>/depth/model/
```

Then run `sh deploy.sh` — files already present with correct MD5 are skipped.

- **Shoot**: the viewfinder is square (the model input is square, what you see is what you get); click the red dot for "original vs depth map" + overlay comparison.
- **Upload**: click the dashed box or drag an image in; it auto-crops the center square, same result.

## Speed

Measured: opening the page loads the model in **7–10 s** in the background (done while framing, no wait when shooting); after that **~0.2 s per shot** — chip-level pre ~15 ms · local ~18 ms · global ~52 ms · head ~52 ms · colorize ~36 ms. The resident model uses ~**1.2 GB / 5 GB** of the NPU.

## When does the model load?

Same logic as the other demos: the service lives only while a page is open (5 s heartbeat keep-alive; multiple tabs don't interfere); **closing all pages → NPU released, service auto-exits after a 10 s grace**. Re-run `start.sh` (idempotent). When the NPU is held by another demo it errors explicitly — close that demo and retry. Only one image is processed at a time (NPU is exclusive).

<details>
<summary><b>Where to look when something breaks</b></summary>

| Symptom | On the board |
|---|---|
| Page won't open | `pgrep -af depth_engine.py`, `curl 127.0.0.1:8091/api/status` |
| Stuck "loading model" | first time 7–10 s, wait; log `tail /tmp/depth_start.log`, engine log `tail /tmp/depth_engine.stdout` |
| Camera fails | USB camera not seated (`lsusb`); or the browser started before the camera — reseat it and refresh |
| First shot very slow | that's model loading (~10+ s); after that ~0.2 s per shot |
| "NPU occupied" | another demo's page is still open; close it and shoot again |
| "Service exited" | all pages were closed. Re-run `sh start.sh` and refresh |

</details>
