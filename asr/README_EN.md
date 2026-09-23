[← Back](../README_EN.md) · [中文](README.md)

# 📝 Qwen3-ASR Speech Subtitles

Speak into the mic; when you stop, **subtitles stream out turn by turn like typing**: confirmed sentences are black, fixed text that grows; the half-sentence being recognized shows as a gray draft that keeps correcting until it "commits" to black. You can also upload an audio file (wav/mp3/flac/m4a, …) with the same result.

<img src="../docs/img/asr_demo.png" alt="ASR demo UI" width="560">

The whole pipeline runs on the NPU: audio encoder + Qwen3-0.6B language model (8-core core_mask). The web shell parses the official `rknn_qwen3_asr_demo_online` binary's per-turn output (`commit_add_text` / `unfix_text`) into SSE events for the browser; **the binary and runtime come straight from the official package — zero compilation on the board**.

## Prerequisites

- This directory ships the official demo package's runtime pieces: `rknn_qwen3_asr_demo_online` (binary), `lib/` (librknn3_api etc.), `mel_128_filters.txt` (feature filters, must be in the working dir).
- Models go in `model/` (pulled and verified by `deploy.sh`): 6 files, ~2.4 GB.

| Item | On-board location |
|---|---|
| Models (6: encoder_online and llm rknn+weight, tokenizer.gguf, embed.bin) | `<demo dir>/model/` |

**Network**: deploy downloads ~2.4 GB from GitHub Releases, so the board needs internet. For a proxy, set it up before running (use your own address/port):

```bash
export http_proxy=http://<proxy-addr>:<port> https_proxy=http://<proxy-addr>:<port>
```

## Run

```bash
# 1) Get the code (GitHub or Gitee; git pull if the repo already exists on the board)
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # faster in China

# 2) Pull models (~2.4 GB; resumable + MD5; rerun to continue; zero compilation on the board)
cd rk1828-modelhub/asr
sh deploy.sh

# 3) Start
sh start.sh
```

The board browser auto-opens the page. From a PC: same subnet → open **http://<board-ip>:8090**; point-to-point → forward the port first, then open `http://127.0.0.1:<your-port>`:

```bash
ssh -L <your-port>:127.0.0.1:8090 root@<board-ip>
```

### Slow on the board? Download on a PC first

Open the [models-asr Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-asr), download all 6 model files, and copy them into `model/`:

```bash
scp encoder_online.* llm.* root@<board-ip>:<repo-path>/asr/model/
```

Then run `sh deploy.sh` — files already present with correct MD5 are skipped.

- **Mic recording**: click the red dot to start (live waveform + timer), click again to stop → auto-decode and commit → subtitles stream out. Needs a browser secure context: on-board chromium (127.0.0.1) is fine; from a PC over a bare IP the mic is blocked — use upload mode.
- **Upload audio**: click the dashed box or drag a file in (wav / mp3 / flac / ogg / m4a; decoded to 16 kHz mono in the browser before submission).

## Transcription speed

Measured (15 s English test clip, 16 turns): per turn, chip-level **audio ~33 ms · ttft ~70 ms**, the whole clip finishes in a few seconds. The model loads before the first turn — **cold start ~1 min, then ~15 s** (weights stay in page cache). Max single-clip length is 17 min.

## When does the model load?

Same logic as the other demos: the service lives only while a page is open (5 s heartbeat keep-alive; multiple tabs don't interfere); **closing all pages → NPU released, service auto-exits after a 10 s grace**. Re-run `start.sh` (idempotent). When the NPU is held by another demo it errors explicitly — close that demo and retry. Manual stop: re-running `start.sh` stops the old instance, or `curl 127.0.0.1:8090/api/bye`. Only one transcription runs at a time (NPU is exclusive).

<details>
<summary><b>Where to look when something breaks</b></summary>

| Symptom | On the board |
|---|---|
| Page won't open | `pgrep -af asr_engine.py`, `curl 127.0.0.1:8090/api/status` |
| Stuck "loading model" | cold start ~1 min, wait; service log `tail /tmp/asr_start.log` |
| Button dead / "service exited" | all pages were closed. A 5 s heartbeat keeps it alive while a page is open; re-run `sh start.sh` and refresh |
| "NPU occupied" | another demo's page is still open; close it and resubmit |
| Decode failed after upload | use a common format (wav/mp3); m4a/ogg depend on the browser decoder |
| Empty transcription | audio too short or no speech; record longer (≥3 s clear speech) |

</details>
