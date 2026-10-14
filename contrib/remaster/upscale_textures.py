#!/usr/bin/env python3
"""Build a high resolution texture pack from extracted Descent bitmaps.

Usage: upscale_textures.py INDIR OUTDIR [--scale 4] [--max-size 256] [--only NAME...]

INDIR is the output of extract_d1_pig.py.  Each bitmap is upscaled 4x with
Real-ESRGAN's compact animevideov3 model (realesrgan-ncnn-py, runs on CPU
or any Vulkan GPU) followed by a light unsharp mask.  The larger x4plus
model was rejected: it invents detail (rock turns into wood grain).  The
result is written to OUTDIR/hires/<name>.png.  Zip OUTDIR's contents (store, no compression)
as a .dxa file and put it next to the game data; the remaster renderer
picks the replacements up automatically.

  - 64x64 wall textures are padded by wrapping so they still tile seamlessly.
  - Transparent pixels get their colour bled from neighbours before
    upscaling (no dark fringes), and alpha is resized separately and kept
    crisp.
  - Bitmaps larger than --max-size (cockpit art, title screens) are skipped.
"""
import argparse
import sys
import time
from pathlib import Path

from PIL import Image, ImageFilter

try:
    import numpy as np
except ImportError:  # pragma: no cover
    raise SystemExit("numpy is required")


def bleed_rgb(img):
    """Fill fully transparent pixels with the average of opaque neighbours."""
    a = np.asarray(img.split()[3], dtype=np.float32) / 255.0
    rgb = np.asarray(img.convert("RGB"), dtype=np.float32)
    if a.min() > 0.5:
        return img.convert("RGB")
    mask = (a > 0.5).astype(np.float32)
    filled = rgb * mask[..., None]
    known = mask.copy()
    for _ in range(64):
        if known.min() > 0:
            break
        acc = np.zeros_like(filled)
        cnt = np.zeros_like(known)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                acc += np.roll(np.roll(filled * known[..., None], dy, 0), dx, 1)
                cnt += np.roll(np.roll(known, dy, 0), dx, 1)
        grow = (known == 0) & (cnt > 0)
        filled[grow] = acc[grow] / cnt[grow][:, None]
        known[grow] = 1.0
    return Image.fromarray(np.clip(filled, 0, 255).astype(np.uint8), "RGB")


def pad(img, p, wrap):
    w, h = img.size
    if wrap:
        tiled = Image.new(img.mode, (w * 3, h * 3))
        for i in range(3):
            for j in range(3):
                tiled.paste(img, (i * w, j * h))
        return tiled.crop((w - p, h - p, 2 * w + p, 2 * h + p))
    arr = np.asarray(img)
    arr = np.pad(arr, ((p, p), (p, p)) + ((0, 0),) * (arr.ndim - 2), mode="edge")
    return Image.fromarray(arr, img.mode)


def upscale_one(upscaler, src, scale):
    img = src.convert("RGBA")
    w, h = img.size
    wrap = (w, h) == (64, 64)
    p = 16 if wrap else 8
    rgb = pad(bleed_rgb(img), p, wrap)
    big = upscaler.process_pil(rgb)
    k = big.size[0] // rgb.size[0]
    big = big.crop((p * k, p * k, (p + w) * k, (p + h) * k))
    if k != scale:
        big = big.resize((w * scale, h * scale), Image.LANCZOS)
    big = big.filter(ImageFilter.UnsharpMask(radius=1.2, percent=140, threshold=1))
    alpha = img.split()[3]
    if alpha.getextrema() != (255, 255):
        a = pad(alpha, p, wrap).resize(((w + 2 * p) * scale, (h + 2 * p) * scale), Image.BICUBIC)
        a = a.crop((p * scale, p * scale, (p + w) * scale, (p + h) * scale))
        # keep edges crisp but anti-aliased
        a = a.point(lambda v: 0 if v < 80 else 255 if v > 175 else int((v - 80) * 255 / 95))
        big.putalpha(a)
        return big
    return big.convert("RGB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir")
    ap.add_argument("outdir")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--max-size", type=int, default=256)
    ap.add_argument("--model", type=int, default=2, help="realesrgan-ncnn-py model index (2 = realesr-animevideov3-x4)")
    ap.add_argument("--gpu", type=int, default=-1, help="Vulkan GPU id, -1 for CPU")
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()

    from realesrgan_ncnn_py import Realesrgan
    up = Realesrgan(gpuid=args.gpu, model=args.model)

    indir = Path(args.indir)
    out = Path(args.outdir) / "hires"
    out.mkdir(parents=True, exist_ok=True)
    files = sorted(indir.glob("*.png"))
    if args.only:
        files = [f for f in files if f.stem in set(args.only)]
    t0 = time.time()
    done = 0
    for n, f in enumerate(files):
        dst = out / f.name
        if dst.exists():
            continue
        img = Image.open(f)
        if max(img.size) > args.max_size:
            continue
        upscale_one(up, img, args.scale).save(dst, optimize=False, compress_level=6)
        done += 1
        if done % 50 == 0:
            el = time.time() - t0
            print(f"{n + 1}/{len(files)}  {el:.0f}s", flush=True)
    print(f"upscaled {done} bitmaps in {time.time() - t0:.0f}s -> {out}")


if __name__ == "__main__":
    sys.exit(main())
