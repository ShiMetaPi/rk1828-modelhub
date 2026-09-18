# 服务器任务：导出 Qwen3-TTS 说话人编码器（给 RK1828 板端 NPU 用）

> 需要拷给服务器的只有一个文件：`export_spk_encoder.py`

## 背景（一句话）

RK1828 板子做声音模仿：参考音频 → 2048 维说话人向量 → TTS 克隆合成。
向量提取这步要放到**板子 NPU** 上跑，需要把这个 12M 参数的说话人编码器转成 RKNN。
服务器负责两件事：① 跑导出脚本产出 ONNX 和配套文件；② 用现有 RKNN 转换流程转模型。

## 第一步：跑导出脚本

```bash
# 1) 依赖（一次性）
pip install torch transformers librosa numpy onnx onnxruntime

# 国内网络下载 HuggingFace 慢的话先：
export HF_ENDPOINT=https://hf-mirror.com

# 2) 运行（首次自动下载 ~50MB 模型 marksverdhei/Qwen3-Voice-Embedding-12Hz-1.7B）
python3 export_spk_encoder.py

# 3)（可选）服务器上有真人声 wav 的话附带做一次真音频校验：
python3 export_spk_encoder.py 某人声录音.wav
```

预期产物（全部在 `./export/` 下）：

| 文件 | 用途 |
|---|---|
| `spk_encoder.onnx` | 模型图。输入 `input_values [1,937,128] fp32`（10 秒 24kHz 音频的 128 维 log-mel），输出 `[1,2048] fp32` |
| `spk_mel_128x513.f32` | mel 滤波器组矩阵（128×513 fp32），板端 C++ 直接加载 |
| `golden_input_mel.npy` | 金标准输入（板端联调对数用） |
| `golden_output.npy` | 金标准输出（torch 真值） |
| `calib_mel.npy` | 8 组校准数据（8×937×128），转换流程要量化校准时用 |

脚本自带自检：torch vs onnx cosine ≥ 0.9999 才放行，不达标会报错退出——**报错就停下来排查，别继续转**。

## 第二步：ONNX → RKNN（用转 speech_decoder 的那套现有流程）

- 输入：`input_values`，形状 `[1, 937, 128]`，float32
- 输出：`embedding`，形状 `[1, 2048]`，float32
- 配置建议：**fp16 权重；不要做 INT8 量化**（说话向量精度重要）
- 若流程强制要校准数据 → 用 `calib_mel.npy`
- **产物命名：`spk_embed.rknn` + `spk_embed.weight`**（和板上 text_projection 一个惯例，weight 单独拆文件）
- 转换后若有对拍步骤：输入喂 `golden_input_mel.npy`，输出和 `golden_output.npy` 比 cosine，应 ≥ 0.999

## 第三步：把这 5 个文件拷回来（板端联调用）

```
spk_embed.rknn
spk_embed.weight
spk_mel_128x513.f32
golden_input_mel.npy
golden_output.npy
```

## 可能遇到的问题

| 现象 | 处理 |
|---|---|
| 转换器报 `Pad(reflect)` 不支持 | 重跑 `python3 export_spk_encoder.py --pad zeros`（边缘数值差异极小，不影响质量） |
| 没装 onnxruntime | `pip install onnxruntime`；实在装不上脚本会跳过对拍（不建议跳） |
| HuggingFace 连不上 | `export HF_ENDPOINT=https://hf-mirror.com` 后重跑 |
| Conv1D 算子不熟 | 模型是纯 1D 卷积族（和 speech_decoder/Mimi codec 同族），现有流程转过同类 |
