#!/usr/bin/env python3
"""Regenerates src/font16x28.h: a 16x28 1-bit monospace bitmap font (ASCII 0x20-0x7E) rendered
natively from DejaVuSansMono-Bold.ttf at 16x28 (not upscaled from a smaller bitmap, unlike the
original 8x14 font this replaced 2026-08-11 - see kernel.cuh's c_fontCellWidth/Height comment).
Kept in the repo (unlike that original font's gen_font.py, which was "throwaway" and lost) since
it's genuinely useful if the font ever needs retuning (size, padding, source typeface). Run from
anywhere - paths below are resolved relative to this script's own location, not the cwd.
"""
import pathlib

from PIL import Image, ImageDraw, ImageFont

CELL_W = 16
CELL_H = 28
FIRST = 0x20
LAST = 0x7E
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
SCRIPT_DIR = pathlib.Path(__file__).resolve().parent

PROBE = "MHgjy|ABCDEFG0123456789"


def measure(size):
    font = ImageFont.truetype(FONT_PATH, size)
    top = None
    bottom = None
    maxw = 0
    for ch in PROBE:
        img = Image.new("L", (CELL_W * 2, CELL_H * 2), 0)
        draw = ImageDraw.Draw(img)
        draw.text((CELL_W // 2, CELL_H // 2), ch, font=font, fill=255)
        bbox = img.getbbox()
        w = font.getlength(ch)
        maxw = max(maxw, w)
        if bbox is not None:
            t = bbox[1] - CELL_H // 2
            b = bbox[3] - CELL_H // 2
            top = t if top is None else min(top, t)
            bottom = b if bottom is None else max(bottom, b)
    return maxw, top, bottom


# Leave ~2px padding top/bottom (roughly matching the original 8x14 font's look) and require the
# widest probed glyph's advance to fit within CELL_W.
best_size = None
best_top = None
best_bottom = None
for size in range(8, 40):
    maxw, top, bottom = measure(size)
    height = bottom - top
    if maxw <= CELL_W and height <= (CELL_H - 4):
        best_size = size
        best_top = top
        best_bottom = bottom
    else:
        break

font = ImageFont.truetype(FONT_PATH, best_size)
y_offset = (CELL_H - (best_bottom - best_top)) // 2 - best_top

print(f"chosen size={best_size} top={best_top} bottom={best_bottom} y_offset={y_offset}")

rows = []
preview_cols = 16
preview = Image.new("L", (CELL_W * preview_cols, CELL_H * ((LAST - FIRST + 1 + preview_cols - 1) // preview_cols)), 0)

for i, code in enumerate(range(FIRST, LAST + 1)):
    ch = chr(code)
    img = Image.new("L", (CELL_W, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    w = font.getlength(ch)
    x_offset = int(round((CELL_W - w) / 2))
    draw.text((x_offset, y_offset), ch, font=font, fill=255)

    preview.paste(img, ((i % preview_cols) * CELL_W, (i // preview_cols) * CELL_H))

    glyph_rows = []
    for y in range(CELL_H):
        bits = 0
        for x in range(CELL_W):
            v = img.getpixel((x, y))
            bit = 1 if v >= 128 else 0
            bits = (bits << 1) | bit
        glyph_rows.append(bits)
    rows.append((code, ch, glyph_rows))

preview_big = preview.resize((preview.width * 4, preview.height * 4), Image.NEAREST)
preview_big.save(SCRIPT_DIR / "font16x28_preview.png")

header = []
header.append("#pragma once")
header.append("")
header.append("// Device-only: this header uses the CUDA __constant__ attribute and must only be #included from")
header.append("// a .cu translation unit (currently just kernel.cu), after kernel.cuh (which defines the font")
header.append("// geometry constants - c_fontCellWidth/c_fontCellHeight/c_fontFirstChar/c_fontLastChar - this")
header.append("// header intentionally does not redefine them).")
header.append("")
header.append("#include <stdint.h>")
header.append("")
header.append("// Auto-generated 16x28 monospace bitmap font, ASCII 0x20-0x7E, one uint16_t per row")
header.append("// (MSB = leftmost column). Generated from DejaVuSansMono-Bold.ttf via gen_font16x28.py, rendered")
header.append("// natively at 16x28 (not nearest-neighbor upscaled from a smaller bitmap like the original")
header.append("// font8x14.h) for genuinely finer edge detail at the same on-screen size (c_fontScale=1 default).")
header.append(f"__constant__ uint16_t g_font16x28[{LAST - FIRST + 1}][{CELL_H}] = {{")
for code, ch, glyph_rows in rows:
    esc = {"\\": "\\\\", "'": "\\'"}.get(ch, ch)
    vals = ", ".join(f"0x{v:04X}" for v in glyph_rows)
    header.append(f"    {{{vals}}}, // 0x{code:02X} '{esc}'")
header.append("};")
header.append("")

with open(SCRIPT_DIR / "src" / "font16x28.h", "w") as f:
    f.write("\n".join(header))

print("wrote font16x28.h,", len(rows), "glyphs")
