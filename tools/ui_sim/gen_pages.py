#!/usr/bin/env python3
"""Generate web/pages.js from the SquareLine user pages.

The state screens are already rendered by app.js from the image assets. This
script covers the pages that are owned by generated UI navigation instead:
quick settings, time/weather, notification, personality, sleep/system settings,
system info, character/state previews, expression manager and animation manager.

Only static layout is extracted. Values that are changed at runtime by app_core
(brightness, volume, Wi-Fi text, etc.) are shown with the values compiled into
the generated screen, exactly like a fresh SquareLine screen.

Usage:
  python3 tools/ui_sim/gen_pages.py
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCREEN_DIR = os.path.join(ROOT, "panda_ai", "components", "panda_ui", "generated", "screens")
OUT = os.path.join(ROOT, "tools", "ui_sim", "web", "pages.js")

PAGES = [
    ("quick_settings", "快捷设置", "快捷设置页；首页下滑进入。"),
    ("time_weather", "时间 / 天气", "时间、日期与迷你熊猫天气页；首页左滑进入。"),
    ("notification", "通知", "消息 / 日程 / 提醒列表；首页上滑进入。"),
    ("personality", "个性化", "性格、反馈强度、动画活跃度与睡眠设置入口。"),
    ("sleep_settings", "睡眠设置", "自动休眠、唤醒方式与睡眠动画开关。"),
    ("system_settings", "系统设置", "亮度、音量、网络、时间同步与系统信息入口。"),
    ("system_info", "存储与版本", "SD 卡、版本、角色预览、表情管理、动画管理入口。"),
    ("character_preview_1", "角色预览 1/3", "表情缩略图第 1 页。"),
    ("character_preview_2", "角色预览 2/3", "表情缩略图第 2 页。"),
    ("state_preview", "状态预览 3/3", "状态屏快捷入口第 3 页。"),
    ("expression_manager", "表情管理", "19 个表情，其中 1 个缺失。"),
    ("animation_manager", "动画管理", "动画帧数与启用状态列表。"),
]

EXTRA_FACES = [
    ("face_laugh", "Laugh", "face_05_laugh", "额外表情页；状态机未枚举。"),
    ("face_excited", "Excited", "face_20_celebrate", "额外表情页；状态机未枚举。"),
    ("face_confused", "Confused", "face_12_confused", "额外表情页；状态机未枚举。"),
    ("face_love", "Love", "face_17_love", "额外表情页；状态机未枚举。"),
    ("face_confident", "Confident", "face_15_confident", "额外表情页；状态机未枚举。"),
    ("face_cried", "Cried", "face_16_cried", "额外表情页；状态机未枚举。"),
]

SCREEN_ENUM = {
    "home": "idle",
    "wakeup": "wake",
    "listening": "listening",
    "thinking": "thinking",
    "speaking": "speaking",
    "network_lost": "offline",
    "reconnecting": "reconnecting",
    "asr_failed": "error_asr",
    "llm_error": "error_llm",
    "tts_error": "error_tts",
    "battery_low": "low_battery",
    "face_sleep": "sleep",
    "face_happy": "face_happy",
    "face_curious": "face_curious",
    "face_surprised": "face_surprised",
    "face_sad": "face_sad",
    "face_shy": "face_shy",
    "face_sleepy": "face_sleepy",
    "face_angry": "face_angry",
}

PAGE_IDS = {p[0] for p in PAGES} | {p[0] for p in EXTRA_FACES}

FONT_SIZE = {
    "ui_font_Big60": 60,
    "ui_font_Title22": 22,
    "ui_font_Body18": 18,
    "ui_font_Small16": 16,
}

# Generated screen target -> preview target understood by app.js.
TARGETS = {
    "home": "live",
    "wakeup": "screen:wake",
    "listening": "screen:listening",
    "thinking": "screen:thinking",
    "speaking": "screen:speaking",
    "asr_failed": "screen:error_asr",
    "llm_error": "screen:error_llm",
    "tts_error": "screen:error_tts",
    "network_lost": "screen:offline",
    "reconnecting": "screen:reconnecting",
    "battery_low": "screen:low_battery",
    "face_sleep": "screen:sleep",
    "face_happy": "screen:face_happy",
    "face_curious": "screen:face_curious",
    "face_surprised": "screen:face_surprised",
    "face_sad": "screen:face_sad",
    "face_shy": "screen:face_shy",
    "face_sleepy": "screen:face_sleepy",
    "face_angry": "screen:face_angry",
    "face_laugh": "face_laugh",
    "face_excited": "face_excited",
    "face_confused": "face_confused",
    "face_love": "face_love",
    "face_confident": "face_confident",
    "face_cried": "face_cried",
    "quick_settings": "quick_settings",
    "time_weather": "time_weather",
    "notification": "notification",
    "personality": "personality",
    "sleep_settings": "sleep_settings",
    "system_settings": "system_settings",
    "system_info": "system_info",
    "character_preview_1": "character_preview_1",
    "character_preview_2": "character_preview_2",
    "state_preview": "state_preview",
    "expression_manager": "expression_manager",
    "animation_manager": "animation_manager",
}


def c_string(value):
    return (value.replace("\\n", "\n").replace("\\t", "\t")
            .replace('\\"', '"').replace("\\\\", "\\"))


def number(block, pattern, default=0):
    match = re.search(pattern, block)
    return int(match.group(1)) if match else default


def color(block, pattern, default=None):
    match = re.search(pattern, block)
    return "#" + match.group(1)[2:] if match else default


def parse_gestures(text, owner):
    """Parse the root screen callback's LV_DIR_* -> screen navigation map."""
    callbacks = {}
    for match in re.finditer(r"void ui_event_(\w+)\(lv_event_t \* e\)\s*\{(.*?)\n\}", text, re.S):
        callbacks[match.group(1)] = match.group(2)
    body = callbacks.get(owner, "")
    if not body:
        return {}

    gestures = {}
    for match in re.finditer(r"LV_DIR_(LEFT|RIGHT|TOP|BOTTOM)\)\s*\{([^}]*)\}", body, re.S):
        target = re.search(r"_ui_screen_change\(&ui_(\w+)", match.group(2))
        if not target:
            continue
        name = target.group(1)
        if name in PAGE_IDS:
            ref = name
        elif name in SCREEN_ENUM:
            ref = "screen:" + SCREEN_ENUM[name]
        elif name in TARGETS and TARGETS[name] != "live":
            ref = TARGETS[name]
        elif name in TARGETS:
            ref = "screen:idle"
        else:
            continue
        direction = {"TOP": "up", "BOTTOM": "down",
                     "LEFT": "left", "RIGHT": "right"}[match.group(1)]
        gestures[direction] = ref
    return gestures


def parse_links(text):
    """Map an object's event callback to the screen it navigates to."""
    callbacks = {}
    for match in re.finditer(r"void ui_event_(\w+)\(lv_event_t \* e\)\s*\{(.*?)\n\}", text, re.S):
        body = match.group(2)
        target = re.search(r"_ui_screen_change\(&ui_(\w+)", body)
        if target:
            callbacks[match.group(1)] = target.group(1)

    links = {}
    for match in re.finditer(r"lv_obj_add_event_cb\((ui_\w+),\s*ui_event_(\w+),", text):
        obj, callback = match.groups()
        target = callbacks.get(callback)
        if target and target in TARGETS:
            links[obj] = TARGETS[target]
    return links


def parse_screen(screen_id):
    path = os.path.join(SCREEN_DIR, "ui_%s.c" % screen_id)
    text = open(path, encoding="utf-8").read()
    init = re.search(r"void ui_%s_screen_init\(void\)\s*\{(.*?)\n\}\n" % screen_id,
                     text, re.S)
    if not init:
        raise RuntimeError("missing screen_init for %s" % screen_id)
    body = init.group(1)
    links = parse_links(text)

    creations = list(re.finditer(
        r"(?m)^\s{4}(ui_[A-Za-z0-9_]+) = lv_(obj|label|image)_create\((\w+)\);",
        body))
    nodes = {}
    order = []

    for i, match in enumerate(creations):
        name, kind, parent = match.groups()
        start = match.end()
        end = creations[i + 1].start() if i + 1 < len(creations) else len(body)
        block = body[start:end]

        if parent == "ui_" + screen_id:
            cx, cy = 180, 180
        else:
            parent_node = nodes.get(parent)
            if parent_node is None:
                continue
            cx, cy = parent_node["cx"], parent_node["cy"]

        cx += number(block, r"lv_obj_set_x\([^,]+,\s*(-?\d+)\)")
        cy += number(block, r"lv_obj_set_y\([^,]+,\s*(-?\d+)\)")
        w = number(block, r"lv_obj_set_width\([^,]+,\s*(\d+)\)")
        h = number(block, r"lv_obj_set_height\([^,]+,\s*(\d+)\)")

        label = re.search(r'lv_label_set_text\([^,]+,\s*"((?:\\.|[^"\\])*)"', block)
        image = re.search(r"lv_image_set_src\([^,]+,\s*&ui_img_(\w+)_png\)", block)
        text_color = color(block, r"text_color\([^,]+,\s*lv_color_hex\((0x[0-9A-Fa-f]+)\)")
        bg_color = color(block, r"bg_color\([^,]+,\s*lv_color_hex\((0x[0-9A-Fa-f]+)\)")
        font = re.search(r"text_font\([^,]+,\s*&(ui_font_\w+)", block)
        align = re.search(r"text_align\([^,]+,\s*(LV_TEXT_ALIGN_\w+)", block)

        node = {
            "name": name,
            "type": kind,
            "cx": cx,
            "cy": cy,
            "w": w,
            "h": h,
            "x": cx - w // 2,
            "y": cy - h // 2,
            "hidden": bool(re.search(
                r"lv_obj_add_flag\(%s,\s*LV_OBJ_FLAG_HIDDEN\)" % re.escape(name), block)),
        }
        if kind == "label":
            node["text"] = c_string(label.group(1)) if label else ""
            node["color"] = text_color or "#FFFFFF"
            node["font"] = FONT_SIZE.get(font.group(1), 16) if font else 16
            node["align"] = {
                "LV_TEXT_ALIGN_CENTER": "center",
                "LV_TEXT_ALIGN_RIGHT": "right",
                "LV_TEXT_ALIGN_LEFT": "left",
            }.get(align.group(1) if align else "", "left")
            node["opacity"] = number(block, r"text_opa\([^,]+,\s*(\d+)", 255) / 255.0
        elif kind == "image":
            node["img"] = image.group(1) if image else ""
        else:
            node["bg"] = bg_color or "transparent"
            node["bgOpacity"] = number(block, r"bg_opa\([^,]+,\s*(\d+)", 0) / 255.0
            node["radius"] = number(block, r"lv_obj_set_style_radius\([^,]+,\s*(\d+)")
            node["removeStyle"] = "lv_obj_remove_style_all(%s)" % name in block
        if name in links:
            node["link"] = links[name]

        nodes[name] = node
        order.append(node)

    return {
        "id": screen_id,
        "kind": "layout",
        "file": "ui_%s.c" % screen_id,
        "bg": "#000000",
        "gestures": parse_gestures(text, screen_id),
        "objects": order,
    }


def extra_face_page(page_id, title, face, note):
    path = os.path.join(SCREEN_DIR, "ui_%s.c" % page_id)
    text = open(path, encoding="utf-8").read()
    return {
        "id": page_id,
        "kind": "face",
        "title": title,
        "file": "ui_%s.c" % page_id,
        "face": face,
        "note": note,
        "gestures": parse_gestures(text, page_id),
        "objects": [],
    }


def main():
    pages = {}
    groups = [
        {"id": "device", "title": "设备页面（手势进入）", "pages": []},
        {"id": "extra", "title": "扩展表情（状态机未枚举）", "pages": []},
    ]

    for screen_id, title, note in PAGES:
        page = parse_screen(screen_id)
        page["title"] = title
        page["note"] = note
        pages[screen_id] = page
        groups[0]["pages"].append(screen_id)

    for page_id, title, face, note in EXTRA_FACES:
        page = extra_face_page(page_id, title, face, note)
        pages[page_id] = page
        groups[1]["pages"].append(page_id)

    screen_gestures = {}
    for gen_name, enum_name in SCREEN_ENUM.items():
        path = os.path.join(SCREEN_DIR, "ui_%s.c" % gen_name)
        text = open(path, encoding="utf-8").read()
        gestures = parse_gestures(text, gen_name)
        if gestures:
            screen_gestures[enum_name] = gestures

    payload = {"groups": groups, "pages": pages,
               "screen_gestures": screen_gestures}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("/* Generated by tools/ui_sim/gen_pages.py from generated/screens. */\n")
        fh.write("window.PANDA_PAGES = ")
        json.dump(payload, fh, ensure_ascii=False, indent=1)
        fh.write(";\n")

    total = sum(len(p.get("objects", [])) for p in pages.values())
    print("wrote %s" % OUT)
    print("%d pages, %d objects" % (len(pages), total))
    for group in groups:
        print("  %-28s %d pages" % (group["title"], len(group["pages"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
