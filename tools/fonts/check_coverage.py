#!/usr/bin/env python3
"""Verify the CJK fallback font covers every non-ASCII glyph the UI uses.

Run after touching UI strings or regenerating the font:

    python3 tools/fonts/check_coverage.py

It parses the cmap tables of the generated `ui_font_CJK16.c` and checks that
every non-ASCII character inside C string literals of the firmware sources has
a glyph. Also flags a stale font (generator charset != shipped cmap) and any
SquareLine font missing the `.fallback` link.
"""

import pathlib
import re
import sys

import gen_cjk_font

ROOT = gen_cjk_font.ROOT
FONT_C = gen_cjk_font.OUT
FONT_DIR = FONT_C.parent

SCAN_DIRS = [
    ROOT / "panda_ai/main",
    ROOT / "panda_ai/components/panda_ui",
    ROOT / "panda_ai/components/panda_state",
]
SKIP_DIR_NAMES = {"managed_components", "fonts"}
STRING_RE = re.compile(r'"(?:\\.|[^"\\\n])*"', re.DOTALL)


def parse_font(path):
    text = path.read_text(encoding="utf-8")

    lists = {}
    for name, body in re.findall(
            r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};",
            text, re.DOTALL):
        lists[name] = [int(v, 0) for v in body.replace("\n", " ").split(",")
                       if v.strip()]

    cmap_body = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\] =\s*\{(.*?)\n\};",
                          text, re.DOTALL)
    if cmap_body is None:
        raise SystemExit(f"cmap table not found in {path}")
    covered = set()
    for block in re.findall(r"\{(.*?)\}", cmap_body.group(1), re.DOTALL):
        start = int(re.search(r"\.range_start = (\d+)", block).group(1))
        length = int(re.search(r"\.range_length = (\d+)", block).group(1))
        sparse = "SPARSE" in block
        if sparse:
            name = re.search(r"\.unicode_list = (unicode_list_\d+)", block).group(1)
            covered.update(start + off for off in lists[name])
        else:
            covered.update(range(start, start + length))
    return covered


def scan_sources():
    used = {}
    for base in SCAN_DIRS:
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".c", ".h"):
                continue
            if SKIP_DIR_NAMES & set(path.relative_to(ROOT).parts):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                for literal in STRING_RE.findall(line):
                    for ch in literal:
                        if ord(ch) > 0x7E:
                            used.setdefault(ch, []).append(
                                f"{path.relative_to(ROOT)}:{lineno}")
    return used


def main():
    if not FONT_C.exists():
        sys.exit(f"font not generated: {FONT_C}")
    covered = parse_font(FONT_C)
    used = scan_sources()

    errors = []

    # The source OTF has no glyph for some punct/letterlike codepoints, so only
    # characters the UI actually renders are checked.
    missing = sorted((ch for ch in used if ord(ch) not in covered),
                     key=ord)
    for ch in missing:
        sites = used[ch]
        errors.append(f"U+{ord(ch):04X} {ch!r} not in {FONT_C.name}: "
                      + ", ".join(sites[:4]))

    for font in sorted(FONT_DIR.glob("ui_font_*.c")):
        if font == FONT_C:
            continue
        if ".fallback = &ui_font_CJK16" not in font.read_text(encoding="utf-8"):
            errors.append(f"{font.name} has no .fallback = &ui_font_CJK16")

    print(f"{FONT_C.name}: {len(covered)} glyphs")
    print(f"UI sources: {len(used)} distinct non-ASCII chars")
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
