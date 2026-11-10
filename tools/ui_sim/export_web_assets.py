#!/usr/bin/env python3
"""Export every generated LVGL image to a web PNG.

Reads the same generated C arrays that are compiled into the firmware, so the
HTML simulator shows the actual asset pixels rather than approximations. Handles
RGB565, RGB565A8 and the indexed I2/I4/I8 palette formats used by the icons.

Usage:
  python3 tools/ui_sim/export_web_assets.py
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_images import load_images  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tools", "ui_sim", "web", "assets")

INDEX_BPP = {
    "LV_COLOR_FORMAT_I1": 1,
    "LV_COLOR_FORMAT_I2": 2,
    "LV_COLOR_FORMAT_I4": 4,
    "LV_COLOR_FORMAT_I8": 8,
}


def png_rgba(path, width, height, rows):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    blob = (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(blob)


def rgb565(v):
    return (((v >> 11) & 0x1F) * 255 // 31,
            ((v >> 5) & 0x3F) * 255 // 63,
            (v & 0x1F) * 255 // 31)


def export(name, tables):
    key = "ui_img_" + name + "_png"
    img = tables.get(key)
    if img is None:
        print("missing image:", key)
        return False
    w, h, data, cf = img["w"], img["h"], img["data"], img["cf"]
    if cf in ("LV_COLOR_FORMAT_RGB565", "LV_COLOR_FORMAT_RGB565A8"):
        rows = []
        for y in range(h):
            row = bytearray()
            for x in range(w):
                i = y * w + x
                v = data[i * 2] | (data[i * 2 + 1] << 8)
                r, g, b = rgb565(v)
                a = data[w * h * 2 + i] if cf == "LV_COLOR_FORMAT_RGB565A8" else 255
                row += bytes((r, g, b, a))
            rows.append(row)
    elif cf in INDEX_BPP:
        # LVGL indexed maps store the BGRA palette first, then row-aligned
        # index bytes. I4/I2 rows are padded to a whole byte.
        bpp = INDEX_BPP[cf]
        colors = 1 << bpp
        palette = data[:colors * 4]
        stride = (w * bpp + 7) // 8
        base = colors * 4
        rows = []
        for y in range(h):
            row = bytearray()
            for x in range(w):
                byte = data[base + y * stride + (x * bpp) // 8]
                shift = 8 - bpp - ((x * bpp) % 8)
                index = (byte >> shift) & (colors - 1)
                off = index * 4
                b, g, r, a = palette[off:off + 4]
                row += bytes((r, g, b, a))
            rows.append(row)
    else:
        print("unsupported format for web export:", key, cf)
        return False
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".png")
    png_rgba(path, w, h, rows)
    print("  %-24s %3dx%-3d %6.1f KiB" % (name + ".png", w, h,
                                           os.path.getsize(path) / 1024))
    return True


def main():
    tables, errors = load_images()
    for err in errors:
        print("FAIL:", err)
    if errors:
        return 1
    print("exporting to", OUT)
    ok = True
    names = sorted(key[len("ui_img_"):-len("_png")] for key in tables
                   if key.startswith("ui_img_") and key.endswith("_png"))
    for name in names:
        ok = export(name, tables) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
