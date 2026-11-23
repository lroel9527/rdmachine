#!/usr/bin/env python3
"""Trains the client's 1x -> 2x text upscaler and writes its weights
(docs/upscaler.md).

  python3 tools/upscaler/train.py [--data DIR] [--out client/upscaler_weights.inc]

Steps: a corpus of sentences from this repository's docs and code comments;
make_data.swift draws text pages, app icons and wallpapers at 1x and 2x with
AppKit (the same picture twice); a small network is fitted to predict each 2x
pixel from the 7 x 7 neighbourhood of 1x pixels around it; the weights go into
a C array the client compiles in, and the test pictures are scored against
pixel repetition and Lanczos.

The network, applied to each colour channel on its own: a linear 7 x 7 filter
per output phase (the 2 x 2 pixels a 1x pixel becomes), plus a correction from
49 -> 16 -> 8 -> 4 with ReLU. Needs numpy and Pillow; about half a minute.
"""
import argparse
import glob
import os
import random
import re
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
K, R = 7, 3          # window and reach in 1x pixels
H1, H2 = 16, 8       # hidden units (the client's kernel takes any; 16, 8 is six times faster than 64, 32 for 0.3 dB)
EPOCHS, BATCH = 20, 4096


def corpus(path):
    random.seed(7)
    out = []
    for f in sorted([os.path.join(ROOT, "README.md")] + glob.glob(os.path.join(ROOT, "docs", "*.md"))):
        t = re.sub(r"[`*|#>\[\]()]", " ", open(f, encoding="utf-8").read())
        for para in t.split("\n"):
            para = " ".join(para.split())
            i = 0
            while i < len(para):
                n = random.randint(12, 34)
                chunk = para[i:i + n].strip()
                if len(chunk) >= 8:
                    out.append(chunk)
                i += n
    for pat in ("client/*.mm", "core/src/*.cpp", "session/src/*.cpp"):
        for f in sorted(glob.glob(os.path.join(ROOT, pat))):
            for line in open(f, encoding="utf-8", errors="ignore"):
                s = line.strip()
                s = s[2:].strip() if s.startswith("//") else s
                if 8 <= len(s) <= 70:
                    out.append(s[:60])
    random.shuffle(out)
    open(path, "w", encoding="utf-8").write("\n".join(out))


def make_data(data):
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "make_data")
        subprocess.run(["swiftc", "-O", "-o", exe, os.path.join(ROOT, "tools", "upscaler", "make_data.swift")], check=True)
        text = os.path.join(tmp, "corpus.txt")
        corpus(text)
        subprocess.run([exe, data, text], check=True)


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def patches(ch):
    """Every 7 x 7 neighbourhood of a channel, edges repeated: (H*W, 49), row by row."""
    p = np.pad(ch, R, mode="edge")
    return np.lib.stride_tricks.sliding_window_view(p, (K, K)).reshape(-1, K * K)


def phases(hr):
    """The 2 x 2 pixels each 1x pixel becomes, in the order (0,0) (0,1) (1,0) (1,1): (H*W, 4)."""
    return np.stack([hr[0::2, 0::2], hr[0::2, 1::2], hr[1::2, 0::2], hr[1::2, 1::2]], axis=-1).reshape(-1, 4)


def unphase(ph, h, w):
    ph = ph.reshape(h, w, 4)
    out = np.empty((h * 2, w * 2), np.float32)
    out[0::2, 0::2], out[0::2, 1::2], out[1::2, 0::2], out[1::2, 1::2] = ph[..., 0], ph[..., 1], ph[..., 2], ph[..., 3]
    return out


def samples(files, cap, rng):
    """Neighbourhoods and targets of every channel of `files`; flat areas mostly left out."""
    X, Y = [], []
    for f in files:
        lr, hr = load(f), load(f.replace("_1x", "_2x"))
        for c in range(3):
            P, T = patches(lr[..., c]), phases(hr[..., c])
            keep = (P.var(axis=1) > 1e-5) | (rng.random(len(P)) < 0.02)
            X.append(P[keep]); Y.append(T[keep])
    X, Y = np.concatenate(X), np.concatenate(Y)
    if len(X) > cap:
        i = rng.choice(len(X), cap, replace=False)
        X, Y = X[i], Y[i]
    return X, Y


def train(data):
    rng = np.random.default_rng(1)
    tr = os.path.join(data, "train")
    X1, Y1 = samples(sorted(glob.glob(os.path.join(tr, "text*_1x.png"))), 2_100_000, rng)
    X2, Y2 = samples(sorted(glob.glob(os.path.join(tr, "icons*_1x.png"))), 450_000, rng)
    X3, Y3 = samples(sorted(glob.glob(os.path.join(tr, "photo-*_1x.png"))), 450_000, rng)
    X = np.concatenate([X1, X2, X3]).astype(np.float32)
    Y = np.concatenate([Y1, Y2, Y3]).astype(np.float32)
    print(f"{len(X)} samples (text {len(X1)}, icons {len(X2)}, photos {len(X3)})", file=sys.stderr)

    # The linear part: least squares, normal equations in chunks.
    G, Rr = np.zeros((K * K + 1, K * K + 1)), np.zeros((K * K + 1, 4))
    for s in range(0, len(X), 200_000):
        xa = np.concatenate([X[s:s + 200_000], np.ones((len(X[s:s + 200_000]), 1), np.float32)], axis=1).astype(np.float64)
        G += xa.T @ xa
        Rr += xa.T @ Y[s:s + 200_000]
    F = np.linalg.solve(G + 1e-3 * np.eye(K * K + 1), Rr).astype(np.float32)
    Yr = Y - (X @ F[:-1] + F[-1])

    def init(i, o):
        return (rng.standard_normal((i, o)) * np.sqrt(2.0 / i)).astype(np.float32), np.zeros(o, np.float32)

    p = [*init(K * K, H1), *init(H1, H2), *init(H2, 4)]
    p[4] *= 0.1
    m = [np.zeros_like(q) for q in p]
    v = [np.zeros_like(q) for q in p]
    t, steps, start = 0, EPOCHS * (len(X) // BATCH), time.time()
    for ep in range(EPOCHS):
        perm = rng.permutation(len(X))
        tot = 0.0
        for i in range(len(X) // BATCH):
            b = perm[i * BATCH:(i + 1) * BATCH]
            x, y = X[b] - 0.5, Yr[b]
            W1, b1, W2, b2, W3, b3 = p
            z1 = x @ W1 + b1; a1 = np.maximum(z1, 0)
            z2 = a1 @ W2 + b2; a2 = np.maximum(z2, 0)
            out = a2 @ W3 + b3
            d = (out - y) * (2.0 / BATCH)
            tot += float(((out - y) ** 2).mean())
            d2 = (d @ W3.T) * (z2 > 0)
            d1 = (d2 @ W2.T) * (z1 > 0)
            grads = [x.T @ d1, d1.sum(0), a1.T @ d2, d2.sum(0), a2.T @ d, d.sum(0)]
            t += 1
            lr = 1e-3 * (0.05 + 0.95 * 0.5 * (1 + np.cos(np.pi * t / steps)))
            for k, g in enumerate(grads):
                m[k] = 0.9 * m[k] + 0.1 * g
                v[k] = 0.999 * v[k] + 0.001 * g * g
                p[k] -= lr * (m[k] / (1 - 0.9 ** t)) / (np.sqrt(v[k] / (1 - 0.999 ** t)) + 1e-8)
        print(f"epoch {ep + 1}/{EPOCHS}: MSE {tot / (len(X) // BATCH):.6f} ({time.time() - start:.0f} s)", file=sys.stderr)
    return F, p


def upscale(img, F, p):
    """The model on a whole picture, as the client runs it: each channel on its own."""
    h, w, _ = img.shape
    W1, b1, W2, b2, W3, b3 = p
    out = np.empty((h * 2, w * 2, 3), np.float32)
    for c in range(3):
        P = patches(img[..., c])
        ph = P @ F[:-1] + F[-1]
        for s in range(0, len(P), 131072):
            a1 = np.maximum((P[s:s + 131072] - 0.5) @ W1 + b1, 0)
            ph[s:s + 131072] += np.maximum(a1 @ W2 + b2, 0) @ W3 + b3
        out[..., c] = unphase(ph, h, w)
    return np.clip(np.floor(out * 255 + 0.5), 0, 255) / 255


def luma(img):
    return img @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def psnr(a, b):
    return 10 * np.log10(1.0 / max(float(np.mean((a - b) ** 2)), 1e-12))


def evaluate(data, F, p):
    """PSNR (luma) of each test picture against its native 2x drawing."""
    te = os.path.join(data, "test")
    rows = []
    for f in sorted(glob.glob(os.path.join(te, "*_1x.png"))):
        lr, hr = load(f), load(f.replace("_1x", "_2x"))
        h, w, _ = lr.shape
        pix = lr.repeat(2, axis=0).repeat(2, axis=1)
        lan = np.asarray(Image.open(f).convert("RGB").resize((w * 2, h * 2), Image.LANCZOS), np.float32) / 255
        mod = upscale(lr, F, p)
        name = os.path.basename(f)[:-7]
        rows.append((name, psnr(luma(pix), luma(hr)), psnr(luma(lan), luma(hr)), psnr(luma(mod), luma(hr))))
    return rows


def write(out, F, p, rows):
    W1, b1, W2, b2, W3, b3 = p
    parts = [F[:-1], F[-1], W1, b1, W2, b2, W3, b3]
    flat = np.concatenate([q.astype(np.float32).ravel() for q in parts])
    lines = [
        "// Generated by tools/upscaler/train.py; do not edit (docs/upscaler.md).",
        "// The 1x -> 2x text upscaler, applied to each colour channel on its own:",
        "//   phase[k] = sum_i x[i] * lin[i][k] + lin_bias[k]",
        "//            + (relu(relu((x - 0.5) * W1 + b1) * W2 + b2) * W3 + b3)[k]",
        "// x: the 7 x 7 neighbourhood row by row (edges repeated), phases (0,0) (0,1) (1,0) (1,1).",
        f"// Layout: lin[49][4], lin_bias[4], W1[49][{H1}], b1[{H1}], W2[{H1}][{H2}], b2[{H2}], W3[{H2}][4], b3[4]"
        f" (upscaler_model.h: kHidden1 = {H1}, kHidden2 = {H2}).",
        "// Test pictures, PSNR (dB, luma) against the native 2x drawing: pixels repeated / Lanczos / this model:",
    ]
    lines += [f"//   {n:<18s} {a:6.2f} / {b:6.2f} / {c:6.2f}" for n, a, b, c in rows]
    lines.append(f"// {len(flat)} floats")
    body = ",\n".join(", ".join(f"{x:.9g}f" for x in flat[i:i + 8]) for i in range(0, len(flat), 8))
    open(out, "w").write("\n".join(lines) + "\n" + body + "\n")


def main():
    global H1, H2
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", help="reuse (or keep) the drawn pictures in this folder")
    ap.add_argument("--out", default=os.path.join(ROOT, "client", "upscaler_weights.inc"))
    ap.add_argument("--hidden", default=f"{H1},{H2}", help="hidden units of the two layers; the client's upscaler_model.h must say the same")
    args = ap.parse_args()
    H1, H2 = (int(v) for v in args.hidden.split(","))
    if args.data and os.path.isdir(os.path.join(args.data, "test")):
        data = args.data
    else:
        data = args.data or tempfile.mkdtemp(prefix="rdm-upscaler-")
        make_data(data)
    F, p = train(data)
    rows = evaluate(data, F, p)
    for n, a, b, c in rows:
        print(f"{n:<18s} pixels {a:6.2f}  lanczos {b:6.2f}  model {c:6.2f} dB")
    write(args.out, F, p, rows)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
