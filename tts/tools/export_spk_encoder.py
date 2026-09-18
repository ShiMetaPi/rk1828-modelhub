#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
export_spk_encoder.py — 导出 Qwen3-TTS 说话人编码器为 ONNX（给 RKNN 转换用）

在转换服务器上跑（一次性）：

    pip install torch transformers librosa numpy onnx onnxruntime
    python3 export_spk_encoder.py                     # 全流程：导出+校验+金标准
    python3 export_spk_encoder.py 参考音频.wav        # 额外用真音频做校验
    python3 export_spk_encoder.py --pad zeros         # 转换器不支持 reflect pad 时的退路

产物（都输出到 ./export/）：
    spk_encoder.onnx          模型图（输入 input_values: [1,937,128] fp32 = 10s 的
                              128 维 log-mel；输出 [1,2048] 说话人向量）
    spk_mel_128x513.f32       librosa(Slaney) mel 滤波器组矩阵，128x513 fp32，
                              板端 C++ 直接加载（省得复刻公式）
    golden_input_mel.npy      金标准输入：固定伪随机波形的 mel（和板端对数用）
    golden_output.npy         金标准输出：torch 模型对上面的输出（2048）
    calib_mel.npy             8 个随机 mel，给需要量化校准的转换流程用

之后的 RKNN 转换（用你服务器上现有的流程，同 speech_decoder 那类普通模型）：
    输入:  input_values, [1,937,128], float32
    输出:  [1,2048] float32
    建议:  fp16 权重、不做 INT8 量化（向量精度重要）；校准数据用 calib_mel.npy
    产物命名: spk_embed.rknn + spk_embed.weight（跟 text_projection 一个惯例）
"""
import argparse
import os
import sys

import numpy as np
import torch

MODEL_ID = "marksverdhei/Qwen3-Voice-Embedding-12Hz-1.7B"

# 与 feature_extraction_ecapa_tdnn.py 一致
SAMPLE_RATE = 24000
N_FFT = 1024
HOP = 256
N_MELS = 128
FMIN = 0.0
FMAX = 12000.0

# 板端固定 10 秒输入：240000 样本 → reflect pad 384*2 → 937 帧
AUDIO_LEN = SAMPLE_RATE * 10                      # 240000
N_FRAMES = 1 + (AUDIO_LEN + 2 * ((N_FFT - HOP) // 2) - N_FFT) // HOP  # 937


class ExportWrapper(torch.nn.Module):
    """复刻 EcapaTdnnSpeakerEncoder.forward，去掉恒真的 mask 分支（全长度输入时是 no-op）。"""

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, input_values):
        m = self.model
        eps = m.asp.eps
        h = input_values.transpose(1, 2)          # (B, mel, T)
        hs_list = []
        for layer in m.blocks:
            h = layer(h)
            hs_list.append(h)
        h = torch.cat(hs_list[1:], dim=1)          # MFA 拼接
        h = m.mfa(h)
        # AttentiveStatisticsPooling（全长度，无 mask）
        T = h.shape[-1]
        gmean = h.mean(dim=2, keepdim=True)
        gstd = torch.sqrt(((h - gmean) ** 2).mean(dim=2, keepdim=True).clamp(eps))
        att = torch.cat([h, gmean.expand(-1, -1, T), gstd.expand(-1, -1, T)], dim=1)
        att = m.asp.conv(torch.tanh(m.asp.tdnn(att)))
        att = torch.softmax(att, dim=2)
        mean = (att * h).sum(dim=2, keepdim=True)
        std = torch.sqrt((att * (h - mean) ** 2).sum(dim=2, keepdim=True).clamp(eps))
        pooled = torch.cat([mean, std], dim=1)     # (B, 2C, 1)
        return m.fc(pooled).squeeze(-1)            # (B, 2048)


def switch_pad_to_zeros(model):
    """把所有 Conv1d 的 reflect padding 换成 zeros（'same' 的填充量都是对称的，
    数值差异只在序列边缘，作为转换器不支持 Pad(reflect) 时的退路）。"""
    n = 0
    for mod in model.modules():
        if isinstance(mod, torch.nn.Conv1d) and mod.padding_mode != "zeros":
            mod.padding_mode = "zeros"
            n += 1
    print(f"[pad] {n} 个 conv 切到 zeros padding")


def compute_mel(audio):
    """和 EcapaTdnnFeatureExtractor._compute_mel 完全一致（用于金标准/校准数据）"""
    from librosa.filters import mel as librosa_mel_fn

    y = torch.from_numpy(audio.astype(np.float32)).unsqueeze(0)
    mel_basis = torch.from_numpy(
        librosa_mel_fn(sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS, fmin=FMIN, fmax=FMAX)
    ).float()
    padding = (N_FFT - HOP) // 2
    y = torch.nn.functional.pad(y.unsqueeze(1), (padding, padding), mode="reflect").squeeze(1)
    hann = torch.hann_window(N_FFT)  # 默认 periodic=True，板端 C++ 要用同一版
    spec = torch.stft(y, N_FFT, hop_length=HOP, win_length=N_FFT,
                      window=hann, center=False, return_complex=True)
    mel = torch.matmul(mel_basis, torch.abs(spec))
    mel = torch.log(torch.clamp(mel, min=1e-5))
    return mel.transpose(1, 2)  # (1, T, 128)


def cos(a, b):
    a, b = np.asarray(a).flatten(), np.asarray(b).flatten()
    return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?", help="可选：用一个真实 wav 做端到端校验")
    ap.add_argument("--pad", choices=["reflect", "zeros"], default="reflect")
    ap.add_argument("--out", default="export")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    from transformers import AutoModel
    print(f"[load] {MODEL_ID}")
    model = AutoModel.from_pretrained(MODEL_ID, trust_remote_code=True)
    model.eval()
    if args.pad == "zeros":
        switch_pad_to_zeros(model)

    # ── 1) ONNX 导出（固定 [1, 937, 128]）──────────────────────────
    wrapper = ExportWrapper(model).eval()
    dummy = torch.randn(1, N_FRAMES, N_MELS)
    onnx_path = os.path.join(args.out, "spk_encoder.onnx")
    torch.onnx.export(
        wrapper, dummy, onnx_path,
        input_names=["input_values"], output_names=["embedding"],
        opset_version=12, do_constant_folding=True,
        dynamic_axes=None,  # 板端固定 10s，静态图对转换器最友好
    )
    print(f"[onnx] {onnx_path}  input_values=[1,{N_FRAMES},{N_MELS}] -> embedding=[1,2048]")

    # ── 2) onnxruntime 对拍 torch ──────────────────────────────────
    ref = wrapper(dummy).detach().numpy()
    try:
        import onnxruntime as ort
        sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
        got = sess.run(None, {"input_values": dummy.numpy()})[0]
        c = cos(ref, got)
        print(f"[check] 随机输入 torch vs onnx cosine = {c:.6f}")
        if c < 0.9999:
            sys.exit("[error] ONNX 导出数值不一致，先别转 RKNN")
    except ImportError:
        print("[check] 没装 onnxruntime，跳过对拍（pip install onnxruntime 可补）")

    if args.wav:
        import librosa
        audio, sr = librosa.load(args.wav, sr=None, mono=True)
        if sr != SAMPLE_RATE:
            audio = librosa.resample(audio, orig_sr=sr, target_sr=SAMPLE_RATE)
        audio = audio[:AUDIO_LEN]
        mel = compute_mel(audio)
        if mel.shape[1] < N_FRAMES:  # 右侧补齐到固定帧数
            mel = torch.nn.functional.pad(mel, (0, 0, 0, N_FRAMES - mel.shape[1]))
        mel = mel[:, :N_FRAMES]
        vec_t = wrapper(mel).detach().numpy()
        try:
            import onnxruntime as ort
            sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
            vec_o = sess.run(None, {"input_values": mel.numpy()})[0]
            print(f"[check] 真音频 torch vs onnx cosine = {cos(vec_t, vec_o):.6f}")
        except ImportError:
            pass
        print(f"[check] 真音频向量范数 = {np.linalg.norm(vec_t):.4f}")

    # ── 3) mel 滤波器组（板端 C++ 直接加载，杜绝公式复刻误差）────────
    from librosa.filters import mel as librosa_mel_fn
    mel_fb = librosa_mel_fn(sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS,
                            fmin=FMIN, fmax=FMAX).astype(np.float32)  # (128, 513)
    fb_path = os.path.join(args.out, "spk_mel_128x513.f32")
    mel_fb.tofile(fb_path)
    print(f"[mel] {fb_path}  shape={mel_fb.shape}（128x513 fp32 行优先）")

    # ── 4) 金标准：固定种子的波形 → mel / 向量（板端联调用）─────────
    rng = np.random.RandomState(20260918)
    wave = (rng.randn(AUDIO_LEN) * 0.05).astype(np.float32)
    mel_g = compute_mel(wave)                                    # (1, 937, 128)
    assert mel_g.shape == (1, N_FRAMES, N_MELS), mel_g.shape
    np.save(os.path.join(args.out, "golden_input_mel.npy"), mel_g.numpy())
    np.save(os.path.join(args.out, "golden_output.npy"),
            wrapper(mel_g).detach().numpy().astype(np.float32))
    print(f"[golden] golden_input_mel.npy / golden_output.npy（种子 20260918，板端对数用）")

    # ── 5) 校准数据（若转换流程要量化校准；建议直接 fp16 不量化）──────
    calib = np.stack([compute_mel((rng.randn(AUDIO_LEN) * 0.05).astype(np.float32)).numpy()[0]
                      for _ in range(8)])
    np.save(os.path.join(args.out, "calib_mel.npy"), calib.astype(np.float32))
    print(f"[calib] calib_mel.npy shape={calib.shape}")

    print("""
================ 下一步：RKNN 转换（用你服务器现有流程）================
  输入:      input_values  [1, 937, 128] float32（10 秒音频的 128 维 log-mel）
  输出:      embedding     [1, 2048] float32
  建议:      fp16 权重、不做 INT8 量化；校准数据用 calib_mel.npy
  产物命名:  spk_embed.rknn + spk_embed.weight
  转完和 spk_mel_128x513.f32 / golden_*.npy 一起拷回，板端联调对数。
=======================================================================""")


if __name__ == "__main__":
    main()
