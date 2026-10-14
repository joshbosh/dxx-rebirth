#!/usr/bin/env python3
"""Extract every bitmap from a Descent 1 PIG as RGBA PNG.

Usage: extract_d1_pig.py DESCENT.PIG DESCENT.HOG OUTDIR

Writes OUTDIR/<name>.png (animation frames as name_<frame>.png, matching the
names the remaster renderer looks up) plus OUTDIR/index.tsv with each
bitmap's size and flags.  Transparency follows the OpenGL renderer: colour
255 is transparent on BM_FLAG_TRANSPARENT bitmaps, colour 254 on
BM_FLAG_SUPER_TRANSPARENT ones.

Requires Pillow.  Supports the PC registered/OEM PIG (v1.4/1.5 layout) and
the PC shareware PIG.
"""
import struct
import sys
from pathlib import Path

from PIL import Image

BM_FLAG_TRANSPARENT = 1
BM_FLAG_SUPER_TRANSPARENT = 2
BM_FLAG_NO_LIGHTING = 4
BM_FLAG_RLE = 8
DBM_FLAG_ABM = 64
DBM_FLAG_LARGE = 128

# Sizes of PIG files that have no leading Pigdata_start word.
NO_START_WORD_SIZES = {
    5092871, 4520145, 4520146,   # shareware variants
    4881019, 4881020,            # v1.0
}


def read_hog(path):
    data = Path(path).read_bytes()
    if data[:3] != b"DHF":
        raise SystemExit(f"{path}: not a HOG file")
    pos, files = 3, {}
    while pos + 17 <= len(data):
        name = data[pos:pos + 13].split(b"\0", 1)[0].decode("latin-1").lower()
        (size,) = struct.unpack_from("<i", data, pos + 13)
        pos += 17
        files[name] = data[pos:pos + size]
        pos += size
    return files


def rle_decode(src, w, h):
    """D1 RLE: int32 total size, h bytes of line lengths, then lines."""
    out = bytearray()
    line_sizes = src[4:4 + h]
    pos = 4 + h
    for y in range(h):
        line = src[pos:pos + line_sizes[y]]
        pos += line_sizes[y]
        row = bytearray()
        i = 0
        while i < len(line) and len(row) < w:
            b = line[i]
            i += 1
            if (b & 0xE0) == 0xE0:
                count = b & 0x1F
                if count == 0:
                    break
                row += bytes([line[i]]) * count
                i += 1
            else:
                row.append(b)
        row = row[:w] + bytes(max(0, w - len(row)))
        out += row
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    pig_path, hog_path, outdir = sys.argv[1:]
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)

    hog = read_hog(hog_path)
    pal_raw = hog.get("palette.256")
    if not pal_raw:
        raise SystemExit("palette.256 not found in HOG")
    pal = [(pal_raw[i * 3] * 4, pal_raw[i * 3 + 1] * 4, pal_raw[i * 3 + 2] * 4) for i in range(256)]

    pig = Path(pig_path).read_bytes()
    start = 0 if len(pig) in NO_START_WORD_SIZES else struct.unpack_from("<i", pig, 0)[0]
    n_bitmaps, n_sounds = struct.unpack_from("<ii", pig, start)
    header_size = n_bitmaps * 17 + n_sounds * 20
    data_base = start + 8 + header_size

    index = []
    for i in range(n_bitmaps):
        h = pig[start + 8 + i * 17: start + 8 + (i + 1) * 17]
        raw_name, dflags, width, height, flags, _avg, offset = struct.unpack("<8sBBBBBi", h)
        name = raw_name.split(b"\0", 1)[0].decode("latin-1").lower()
        if dflags & DBM_FLAG_ABM:
            name = f"{name}_{dflags & 63}"
        if dflags & DBM_FLAG_LARGE:
            width += 256
        if not width or not height:
            continue
        src = pig[data_base + offset:]
        pixels = rle_decode(src, width, height) if flags & BM_FLAG_RLE else src[:width * height]

        img = Image.new("RGBA", (width, height))
        px = []
        for c in pixels:
            if c == 254 and flags & BM_FLAG_SUPER_TRANSPARENT:
                px.append((255, 255, 255, 0))
            elif c == 255 and flags & BM_FLAG_TRANSPARENT:
                px.append((0, 0, 0, 0))
            else:
                r, g, b = pal[c]
                px.append((r, g, b, 255))
        img.putdata(px)
        img.save(out / f"{name}.png")
        index.append(f"{name}\t{width}\t{height}\t{flags}")

    (out / "index.tsv").write_text("name\twidth\theight\tflags\n" + "\n".join(index) + "\n")
    print(f"extracted {len(index)} bitmaps to {out}")


if __name__ == "__main__":
    main()
