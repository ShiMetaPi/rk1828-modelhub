# PaddleOCR-VL Demo

通用 OCR demo，基于 PaddleOCR-VL 多模态视觉-语言模型（百度开源）跑在 RK1828 NPU 上。
本目录是项目 6 合一 demo 仓库（chat/vl/tts/asr/depth/yolo26 + 本 demo）的第 7 个。

| 端口 | 引擎形态 | 模型 |
|---|---|---|
| 8093 | C++ ocr_engine（Unix socket + stb_image + 官方子模块） | PaddleOCR-VL（504×504 vision + 4-bit LLM） |

## 板端快速开始

```sh
cd paddleocr_vl
sh deploy.sh     # 拉 SDK + 模型 + 编译引擎
sh start.sh      # 起服务（NPU 占用检查 + ocr_server.py + 浏览器）
```

浏览器打开 `http://localhost:8093/`（或 `http://<board-ip>:8093/`），
拖入 / 上传图片，点「开始识别」即可。

`stop.sh` 强制停（杀 python + C++ 引擎子进程）。

## 架构（与 vl/tts/asr/depth/yolo26 完全一致的三层）

```
浏览器 (ocr.html)
    ↑ SSE 流式 token
    ↓ POST /api/ocr (multipart 上传图)
Python web (ocr_server.py :8093)
    ↓ 启 / 杀 / 探活
C++ 引擎 (ocr_engine /tmp/ocr_engine.sock)
    ↓ init_paddleocr_vl_model / inference_paddleocr_vl_model
RKNN3 NPU (vision 504² + mlpar + 4-bit LLM)
```

- **页面驱动生命周期**：开页面 GET / → 触发后台 preload（首请求时 init 一次模型）。
  关页面 / 切到后台 → POST /api/bye → 引擎卸载，NPU 释放。
- **NPU 独占**：启动前 `rknn-smi` 查占用，被占时 start.sh 拒绝。
- **模型留热**：首次推理后模型常驻 ~75 秒无活动才卸载。

## 模型

9 个文件，约 795 MB，部署在 `<demo>/{llm,vision}/`（注意：不像其他 demo 包在
`model/` 里，PaddleOCR-VL 沿用 rknn3-model-zoo 上游布局，直接 `llm/` + `vision/`）：

| 子目录 | 文件 |
|---|---|
| llm/  | PaddleOCR-llm.{rknn, weight, tokenizer.gguf, embed.bin} |
| vision/ | PaddleOCR-vision.{rknn, weight} |
| vision/ | PaddleOCR-vision-mlp_AR.{rknn, weight}（MLP-AR 子图）|
| vision/ | position_embedding_model.bin |

Vision 输入 504×504（已固化在 `engine/src/vision/rknn_paddleocr_vl_vision.h`）。
模型目录预留了 OCR 之外的接口（Table / Chart / Formula），但 UI 只暴露 OCR 模式。

## deploy.sh：两路拉模型

按顺序尝试：

1. **GitHub Release**（tag `models-paddleocr-vl`）—— 公开仓库 `ShiMetaPi/rk1828-modelhub`，
   `curl + md5sum` 校验。可用 `MIRROR_URL=...` 切到本地镜像加速。
2. **本地 llm/ + vision/ 已就绪** —— 若 GitHub 拉不到（Release 尚未发布或断网），脚本会
   扫描 `<here>/{llm,vision}/`，MD5 全过的就跳过、未过的报错并提示两种补救方式：
   - 把 Release 补发，或
   - 从 PC `rsync` 过来：`rsync -avP <pc_paddleocr_vl>/{llm,vision}/ <board>:<here>/{llm,vision}/`

`librknn3_api.so` 从 tag `rknn3-api` 拉，跟其他 demo 共用同一份。

## 引擎 C++ 代码组织

```
engine/
├── CMakeLists.txt           # 链 librknn3_api + tokenizers-cpp + stb_image
├── build.sh                 # 板上编译入口（deploy.sh 自动调）
├── sdk/rknn3_api.h          # RKNN3 头文件（从 vl 复制）
├── src/
│   ├── main.cpp             # 我们的 Unix-socket server：init_/infer_/release_ 的 socket 包装
│   ├── paddleocr_vl.cc/h    # 官方子编排（init/inference/release + 共享内部内存）
│   ├── llm/                 # 官方 LLM 子模块
│   ├── vision/              # 官方 vision 子模块（MODEL_WIDTH/HEIGHT=504 已固化）
│   └── mlpar/               # 官方 MLP-AR 子模块（动态 shape 强制 CPU）
├── 3rdparty/
│   ├── stb/                 # stb_image.h + stb_image_write.h（单头文件）
│   └── tokenizer/           # libtokenizer.a + Tokenizer.h（llama.cpp-backed）
```

**socket 协议**（单行 JSON，`\n` 分隔）：

```
{"cmd":"ping"}
  → {"ev":"pong","loaded":1}
{"cmd":"load","qid":N}
  → {"ev":"done","qid":N,"loaded":1,"load_ms":12345}（或 {"ev":"error","msg":...}）
{"cmd":"infer","qid":N,"img":"/path/to/img.png","prompt":"ocr|table|chart|formula"}
  → 多行：
    {"ev":"token","qid":N,"text":"<fcel>考评项目"}
    {"ev":"token","qid":N,"text":"<lcel><fcel>权重"}
    ...
    {"ev":"done","qid":N,"tokens":418,"vision_ms":507,"llm_ms":1743,"total_ms":2400}
{"cmd":"unload","qid":N}
  → {"ev":"done","qid":N,"loaded":0}
```

## 当前局限 / 后续可做

- UI 只暴露 **OCR 模式**（默认 prompt）。Table / Chart / Formula 引擎已支持，
  改 ocr.html 加按钮即可，前端会传 `prompt:"table"` 等。
- 模型未上 GitHub Release（v1 阶段）。首次部署需手动 rsync。
- 性能：参考 README，vision ~507ms + LLM decode ~240 tok/s，
  1-2 秒内出结果是常见情况。

## 已知坑 / 备忘

- 引擎 C++ 二进制加载模型后常驻 ~800 MB NPU 内存，与其他 demo **互斥**。
  start.sh 起服务前 `rknn-smi` 自查占用。
- `position_embedding_model.bin` 是模型专属文件（3.2 MB），
  不能复用其他 demo 的同名文件。
- mlpar 子图必须在 CPU 跑（动态 shape），NPU 只吃 vision 编码段 + LLM decode。