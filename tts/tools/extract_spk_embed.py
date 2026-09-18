#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
extract_spk_embed.py — 从参考音频提取 Qwen3-TTS 声音克隆用的说话人向量

在 PC/服务器上跑（板子不做音频→向量提取，只消费 .npy）：

    python3 extract_spk_embed.py 参考音频.wav --name myvoice
    → ./voices/myvoice.npy   （2048 个 float32，约 8KB）

然后把 .npy 上传到 tts demo 页面（http://<板子IP>:8088 →「我的音色」），
或直接放到板子 {TTS_MODEL_DIR}/voices/myvoice.npy。

依赖：
    pip install torch transformers librosa numpy

模型（首次运行自动从 HuggingFace 下载，约 50MB）：
    marksverdhei/Qwen3-Voice-Embedding-12Hz-1.7B
    官方 Qwen3-TTS-12Hz-1.7B-Base 里抽出来的说话人编码器（ECAPA，12M 参数，Apache-2.0）
国内网络下载慢可先： export HF_ENDPOINT=https://hf-mirror.com

参考音频要求：5~10 秒干净人声（单人、无 BGM、无混响），最短 3 秒。
传多段音频时会额外输出一个均值版本（通常更稳）。

对拍两个 .npy 的相似度（验证提取一致性用）：
    python3 extract_spk_embed.py --compare a.npy b.npy
"""
import argparse
import os
import sys

import numpy as np

MODEL_ID = "marksverdhei/Qwen3-Voice-Embedding-12Hz-1.7B"
EMBED_DIM = 2048


def load_audio(path):
    import librosa
    audio, sr = librosa.load(path, sr=None, mono=True)
    dur = len(audio) / sr
    if dur < 3:
        print(f"[warn] {path} 只有 {dur:.1f} 秒，建议 5~10 秒，太短克隆质量会差")
    elif dur > 30:
        print(f"[warn] {path} 有 {dur:.1f} 秒，只取前 10 秒")
        audio = audio[: int(sr * 10)]
    return audio, sr


def extract(paths, model=None, processor=None):
    import torch
    if model is None:
        from transformers import AutoModel, AutoProcessor
        print(f"[load] {MODEL_ID}（首次运行会下载权重）")
        processor = AutoProcessor.from_pretrained(MODEL_ID, trust_remote_code=True)
        model = AutoModel.from_pretrained(MODEL_ID, trust_remote_code=True)
        model.eval()
    results = []
    for p in paths:
        audio, sr = load_audio(p)
        inputs = processor(audio, sampling_rate=sr)
        with torch.no_grad():
            emb = model(**inputs).last_hidden_state  # (1, 2048)
        vec = emb.squeeze(0).to(torch.float32).cpu().numpy()
        if vec.shape != (EMBED_DIM,):
            sys.exit(f"[error] {p} 输出维度异常: {vec.shape}（应为 ({EMBED_DIM},)）")
        print(f"[ok] {p}: dim={vec.shape[0]} norm={np.linalg.norm(vec):.4f}")
        results.append(vec)
    return results


def sanitize_name(name):
    return "".join(c if c.isalnum() or c in "_-" else "_" for c in name.lower())[:24]


def main():
    ap = argparse.ArgumentParser(description="Qwen3-TTS 说话人向量提取（声音克隆）")
    ap.add_argument("audio", nargs="*", help="参考音频文件（wav/mp3/flac…，可多个）")
    ap.add_argument("--name", help="输出音色名（默认取第一个音频的文件名）")
    ap.add_argument("--out", default="voices", help="输出目录（默认 ./voices）")
    ap.add_argument("--compare", nargs=2, metavar=("A.npy", "B.npy"),
                    help="比较两个已有 .npy 的 cosine 相似度")
    args = ap.parse_args()

    if args.compare:
        a = np.load(args.compare[0]).astype(np.float32)
        b = np.load(args.compare[1]).astype(np.float32)
        if a.shape != (EMBED_DIM,) or b.shape != (EMBED_DIM,):
            sys.exit(f"[error] 维度不对: {a.shape} vs {b.shape}")
        cos = float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))
        print(f"cosine = {cos:.4f}（越接近 1 越像同一人/同一提取器）")
        return

    if not args.audio:
        sys.exit(ap.format_help())

    vecs = extract(args.audio)
    os.makedirs(args.out, exist_ok=True)
    base = sanitize_name(args.name or os.path.splitext(os.path.basename(args.audio[0]))[0])
    out = os.path.join(args.out, base + ".npy")
    np.save(out, vecs[0].astype(np.float32))
    print(f"[done] {out}")
    print(f"下一步：浏览器打开 http://<板子IP>:8088 上传（我的音色），"
          f"或 scp 到板子 {{TTS_MODEL_DIR}}/voices/{base}.npy")
    if len(vecs) > 1:
        out2 = os.path.join(args.out, base + "_mean.npy")
        np.save(out2, np.mean(vecs, axis=0).astype(np.float32))
        print(f"[done] {out2}（多段均值，通常更稳）")


if __name__ == "__main__":
    main()
