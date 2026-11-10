#!/usr/bin/env python3
"""Validate the SquareLine image assets compiled into panda_ui.

Checks every generated image descriptor and every lv_image_set_src() reference
without needing the device or a display. Exits non-zero on any broken asset.

Usage:
  python3 tools/ui_sim/check_images.py
  python3 tools/ui_sim/check_images.py --sheet build/ui_face_sheet.png
"""
import argparse
import glob
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI_DIR = os.path.join(ROOT, "panda_ai", "components", "panda_ui", "generated")
IMG_DIR = os.path.join(UI_DIR, "images")
SCREEN_DIR = os.path.join(UI_DIR, "screens")

BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")
DESC_RE = re.compile(r"const\s+lv_image_dsc_t\s+(\w+)\s*=\s*\{(.*?)\};", re.S)
MAP_RE = re.compile(r"uint8_t\s+(\w+)_map\[\]\s*=\s*\{(.*?)\};", re.S)
USED_RE = re.compile(r"lv_image_set_src\s*\(\s*\w+\s*,\s*&(\w+)\s*\)")

BPP = {"LV_COLOR_FORMAT_I1": 1, "LV_COLOR_FORMAT_I2": 2,
       "LV_COLOR_FORMAT_I4": 4, "LV_COLOR_FORMAT_I8": 8}


def parse_int(expr):
    expr = expr.strip()
    if not re.fullmatch(r"[0-9+\s*()]+", expr):
        raise ValueError("unsupported data_size expression: %r" % expr)
    return int(eval(expr, {"__builtins__": {}}, {}))


def expected_size(cf, w, h):
    if cf in ("LV_COLOR_FORMAT_RGB565", "LV_COLOR_FORMAT_ARGB8565"):
        return w * h * 2
    if cf == "LV_COLOR_FORMAT_RGB565A8":
        return w * h * 3
    if cf == "LV_COLOR_FORMAT_RGB888":
        return w * h * 3
    if cf == "LV_COLOR_FORMAT_ARGB8888":
        return w * h * 4
    if cf in BPP:
        return (w * h * BPP[cf] + 7) // 8 + (1 << BPP[cf]) * 4
    return None


def load_images():
    images = {}
    errors = []
    for path in sorted(glob.glob(os.path.join(IMG_DIR, "*.c"))):
        text = open(path, encoding="utf-8", errors="ignore").read()
        dm = DESC_RE.search(text)
        mm = MAP_RE.search(text)
        name = os.path.basename(path)[:-2]
        if not dm or not mm:
            errors.append("%s: missing descriptor or pixel map" % name)
            continue
        body = dm.group(2)
        try:
            cf = re.search(r"\.header\.cf\s*=\s*(\w+)", body).group(1)
            magic = re.search(r"\.header\.magic\s*=\s*(\w+)", body).group(1)
            w = int(re.search(r"\.header\.w\s*=\s*(\d+)", body).group(1))
            h = int(re.search(r"\.header\.h\s*=\s*(\d+)", body).group(1))
            data_size = parse_int(re.search(r"\.data_size\s*=\s*([^,;]+)", body).group(1))
        except (AttributeError, ValueError) as exc:
            errors.append("%s: bad descriptor (%s)" % (name, exc))
            continue
        data = bytes(int(b, 16) for b in BYTE_RE.findall(mm.group(2)))
        images[dm.group(1)] = dict(name=name, cf=cf, magic=magic, w=w, h=h,
                                   data_size=data_size, data=data)
    return images, errors


def validate(images, refs):
    errors = []
    warnings = []
    if len(images) != len(set(images)):
        errors.append("duplicate image descriptors")
    for key, img in images.items():
        name = img["name"]
        if img["magic"] != "LV_IMAGE_HEADER_MAGIC":
            errors.append("%s: bad magic %s" % (name, img["magic"]))
        if img["w"] <= 0 or img["h"] <= 0:
            errors.append("%s: invalid size %dx%d" % (name, img["w"], img["h"]))
        want = expected_size(img["cf"], img["w"], img["h"])
        if want is None:
            errors.append("%s: unsupported color format %s" % (name, img["cf"]))
        else:
            if img["data_size"] != want:
                errors.append("%s: data_size=%d, expected %d for %s %dx%d"
                              % (name, img["data_size"], want, img["cf"],
                                 img["w"], img["h"]))
            if len(img["data"]) != img["data_size"]:
                errors.append("%s: map has %d bytes, descriptor says %d"
                              % (name, len(img["data"]), img["data_size"]))
        if len(img["data"]) > 1 and len(set(img["data"][:img["data_size"]])) <= 1:
            errors.append("%s: pixel data is uniform (blank asset)" % name)
        if img["cf"] in ("LV_COLOR_FORMAT_RGB565", "LV_COLOR_FORMAT_RGB565A8"):
            px = len(img["data"]) // 2
            if px and len(set(img["data"][:px * 2:2])) <= 1:
                errors.append("%s: all pixels share one RGB565 colour" % name)

    for name, ref in refs:
        if ref not in images:
            errors.append("%s: lv_image_set_src(&%s) has no descriptor" % (name, ref))
    declared = set(images)
    unused = sorted(declared - {r for _, r in refs})
    if unused:
        warnings.append("%d image(s) not referenced by any screen: %s"
                        % (len(unused), ", ".join(unused)))
    return errors, warnings


def collect_refs():
    refs = []
    for path in sorted(glob.glob(os.path.join(SCREEN_DIR, "*.c"))):
        text = open(path, encoding="utf-8", errors="ignore").read()
        for ref in USED_RE.findall(text):
            refs.append((os.path.basename(path), ref))
    return refs


def write_png(path, width, height, rows):
    """rows: list of bytearray RGB triples, top to bottom."""
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(png)


def rgb565_sheet(images, path, cols=5, cell=140):
    faces = sorted((k for k in images if re.fullmatch(r"ui_img_face_\d+_\w+_png", k)),
                   key=lambda k: k)
    if not faces:
        return []
    rows_n = (len(faces) + cols - 1) // cols
    width, height = cols * cell, rows_n * cell
    canvas = [bytearray([24, 24, 24] * width) for _ in range(height)]
    for i, key in enumerate(faces):
        img = images[key]
        w, h = img["w"], img["h"]
        ox, oy = (i % cols) * cell, (i // cols) * cell
        pad = 4
        scale = min((cell - 2 * pad) / w, (cell - 2 * pad) / h)
        dw, dh = max(1, int(w * scale)), max(1, int(h * scale))
        src = img["data"]
        for y in range(dh):
            row = canvas[oy + pad + y]
            sy = min(h - 1, int(y / scale))
            for x in range(dw):
                sx = min(w - 1, int(x / scale))
                v = src[(sy * w + sx) * 2] | (src[(sy * w + sx) * 2 + 1] << 8)
                r = ((v >> 11) & 0x1F) * 255 // 31
                g = ((v >> 5) & 0x3F) * 255 // 63
                b = (v & 0x1F) * 255 // 31
                off = (ox + pad + x) * 3
                row[off:off + 3] = bytes((r, g, b))
    write_png(path, width, height, canvas)
    return faces


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet", help="write a PNG contact sheet of the 20 face images")
    args = ap.parse_args()

    images, errors = load_images()
    refs = collect_refs()
    errs, warns = validate(images, refs)
    errors += errs

    if args.sheet:
        faces = rgb565_sheet(images, args.sheet)
        print("contact sheet: %s (%d faces)" % (args.sheet, len(faces)))

    total = sum(img["data_size"] for img in images.values())
    print("images: %d descriptors, %d referenced, %.1f MiB pixel data"
          % (len(images), len({r for _, r in refs}), total / (1024 * 1024)))
    for w in warns:
        print("WARN: " + w)
    for e in errors:
        print("FAIL: " + e)
    print("image check: %s (%d failure(s))" % ("PASS" if not errors else "FAIL",
                                               len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
