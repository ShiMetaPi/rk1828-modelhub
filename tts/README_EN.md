[← Back](../README_EN.md) · [中文](README.md)

# 🔊 Qwen3-TTS Text-to-Speech Demo

Type text, click once in the browser, and hear synthesized speech. You can also steer the delivery with a natural-language instruction ("say it in a happy, excited tone" / "in a sad, tearful tone"), switch among 9 preset voices (including Sichuan and Beijing dialects), and **synthesize with your own voice** — give a 5–10 s reference clip and it clones a voice of the same name.

<img src="../docs/img/tts_demo.png" alt="TTS demo UI" width="560">

The core is a 4-model pipeline (all on the NPU): `text_projector → talker → code_predictor → speech_decoder`, outputting 24 kHz mono WAV. The talker is a 1.7B CustomVoice with instruction control.

## Prerequisites

- The board needs the RKNN3 runtime `librknn3_api.so` (under `/usr/lib`) and the header `rknn3_api.h` (installed with the chat/vl demo SDK); the first `start.sh` compiles the C++ engine and tells you what's missing.
- **Deploy the vl demo first**: the engine's build SDK (header + libtokenizer.a) is taken from `../vl/engine` by default; if vl isn't on this board, point `TTS_SDK_ROOT` at it.
- Models are pulled and verified by `deploy.sh`: 15 files placed **flat** in `model/` (the engine reads by filename, no subdirectories).

| Item | On-board location |
|---|---|
| Models (15: talker / code_predictor / speech_decoder / text_projection / spk_embed rknn+weight, tokenizer.json, 3 embeds, mel filters) | `<demo dir>/model/` |

**Network**: deploy downloads ~4 GB from GitHub Releases, so the board needs internet. For a proxy, set it up before running (use your own address/port):

```bash
export http_proxy=http://<proxy-addr>:<port> https_proxy=http://<proxy-addr>:<port>
```

## Run

```bash
# 1) Get the code (GitHub or Gitee; git pull if the repo already exists on the board)
git clone https://github.com/ShiMetaPi/rk1828-modelhub.git
# git clone https://gitee.com/ShiMetaPi_0/rk1828-modelhub.git   # faster in China

# 2) Pull models (~4 GB; resumable + MD5; rerun to continue)
cd rk1828-modelhub/tts
sh deploy.sh

# 3) Start (only the web shell; the first run auto-builds the C++ engine; the model loads when the page opens)
sh start.sh
```

The board browser auto-opens the page (~2 s). From a PC: same subnet → open **http://<board-ip>:8088**; point-to-point → forward the port first, then open `http://127.0.0.1:<your-port>`:

```bash
ssh -L <your-port>:127.0.0.1:8088 root@<board-ip>
```

Type text (optionally fill the "emotion instruction" and pick a voice), then click "Generate".

### Slow on the board? Download on a PC first

Open the [models-tts Release](https://github.com/ShiMetaPi/rk1828-modelhub/releases/tag/models-tts), download all 15 files, and copy them **flat** into `model/` (no subdirectories):

```bash
scp talker.rknn talker.weight ... root@<board-ip>:<repo-path>/tts/model/
```

Files over 2 GB are split on the Release (`xxx.part-aa`, `xxx.part-ab`, …, e.g. talker.weight); download all parts and reassemble on the PC: Windows `copy /b xxx.part-aa+xxx.part-ab xxx`, Linux/mac `cat xxx.part-* > xxx`. Then run `sh deploy.sh` — files already present with correct MD5 are skipped.

## When does the model load?

Same logic as the chat/vl demos: the engine runs only while the page is open; **closing the page releases the NPU immediately, and after a 10 s grace the whole service exits**. Re-run `start.sh` (idempotent, no "address already in use"). When the NPU is held by another demo the engine won't force its way in and the API errors — close the other demo and refresh. Manual stop: re-running `start.sh` stops the old instance, or `curl 127.0.0.1:8088/api/bye`.

## Synthesis speed

"你好" ~1.4 s, "欢迎使用语音合成" ~1.9 s, a 15-character sentence ~3.8 s. Emotion instructions change prosody (sad is noticeably slower than happy).

## Voice cloning (your own voice)

How it works: reference audio → 2048-dim speaker vector (8 KB `.npy`) → injected into the talker on the board. **Extraction happens entirely on the board**: pick an audio file or record in the page, the browser decodes and resamples to 24 kHz PCM, `spk_encoder.cc` runs the speaker encoder (12M-param ECAPA) on the NPU, writes the vector straight into `voices/` — no restart, no PC involved.

```bash
# 0) (one-time) Convert the speaker encoder to RKNN — see tools/HANDOFF_export.md:
#     run tools/export_spk_encoder.py on a server to get ONNX + mel filterbank + golden refs,
#     then follow the existing RKNN conversion flow for spk_embed.rknn + spk_embed.weight,
#     copy to the model dir (skipping this doesn't break synthesis; the page's "extract voice" just reports unavailable)

# 1) Open http://<board-ip>:8088 → "Voice cloning": pick audio or "record", name the voice, click "Extract voice"

# 2) Select the new voice in the dropdown and generate as usual
```

- Reference audio: **5–10 s of clean speech** (single speaker, no BGM, no reverb), minimum 3 s; the engine takes the first 10 s — **record the full 10 s for best similarity** (short clips are zero-padded, diluting the vector).
- On-board extraction measured: vector extraction 0.11 s; golden-set cosine 0.9993 (board C++ preprocessing + NPU inference vs server PyTorch ground truth).
- `spk_embed.rknn` / `spk_embed.weight` / `spk_mel_128x513.f32` are already in deploy.sh's Release download list; to re-convert the encoder yourself, see `tools/HANDOFF_export.md`.
- The encoder is the speaker encoder from the official Qwen3-TTS 12Hz 1.7B-Base (community export `marksverdhei/Qwen3-Voice-Embedding-12Hz-1.7B`, ECAPA 12M params, Apache-2.0); on-board preprocessing (STFT + mel filterbank) is bit-aligned with transformers, with the mel matrix loaded from `spk_mel_128x513.f32` rather than reimplemented in C++.
- Recording needs a browser secure context: the on-board chromium (127.0.0.1) is fine; from a PC over a bare IP getUserMedia is blocked — use "pick an audio file" instead.
- A manual path is kept: run `tools/extract_spk_embed.py` on a PC to get a `.npy`, then upload or `scp` it into `voices/` — exactly equivalent to on-board extraction.
- Names are limited to lowercase letters / digits / `_` / `-`, 1–24 chars; files are validated (exactly 2048 float32), invalid names/files are rejected.

<details>
<summary><b>Where to look when something breaks</b></summary>

| Symptom | On the board |
|---|---|
| Page won't open | `pgrep -af tts_engine.py`, `curl 127.0.0.1:8088/api/status` |
| Stuck "loading" | model load ~40 s, wait; engine log `tail /tmp/tts_engine.stdout` |
| Button dead / "service exited" | the service exited (all pages closed). A heartbeat every 5 s keeps it alive while a page is open; re-run `sh start.sh` and refresh |
| Generation errors | `/tmp/tts_engine.stdout` (C++ engine) and the web shell log |
| Chinese garbled | don't send Chinese via Windows curl directly (console encoding); use the browser or on-board curl |

</details>
