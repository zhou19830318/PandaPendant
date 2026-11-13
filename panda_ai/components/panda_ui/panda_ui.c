#include "panda_ui.h"
#include "panda_display.h"
#include "panda_state.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "qrcode.h"
#include "ui.h"

static const char *TAG = "ui";

static panda_state_t s_state;
static panda_state_view_t s_view;
static lv_timer_t *s_state_timer;
static bool s_status_override;
static uint32_t s_status_override_until;
static int s_battery_pct = -1;
static bool s_charging;
static bool s_battery_present;
static bool s_wifi_connected;
static bool s_wifi_ap;
static bool s_wifi_connecting;
static int s_volume = 70;
static int s_brightness = 80;
static bool s_muted;
static bool s_time_valid;
static char s_status[96];
static char s_subtitle[PANDA_STATE_SUBTITLE_MAX];

static panda_ui_menu_hooks_t s_hooks;
static int s_personality = 1;
static int s_feedback = 1;
static int s_animation = 1;
static int s_auto_sleep_s = 300;
static int s_wake_mode = 2;
static bool s_sleep_anim = true;
static bool s_dnd;
static uint32_t s_anim_ms = 150;
static bool s_weather_valid;
static int s_weather_temp;
static panda_ui_weather_t s_weather_icon = PANDA_UI_WEATHER_CLOUDY;
static bool s_sd_mounted;
static uint64_t s_sd_free_kb;
static uint64_t s_sd_total_kb;
static bool s_menu_syncing;
static lv_obj_t *s_bri_slider;
static lv_obj_t *s_vol_slider;

static lv_obj_t *s_status_box;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_subtitle_box;
static lv_obj_t *s_subtitle_lbl;

/* Subtitle window: two text lines. Longer replies scroll bottom-to-top so the
 * overlay never grows over the rest of the screen. */
#define SUBTITLE_LINE_PX 20
#define SUBTITLE_LINES 2
#define SUBTITLE_PAD_PX 4
#define SUBTITLE_CONTENT_PX (SUBTITLE_LINE_PX * SUBTITLE_LINES)
#define SUBTITLE_BOX_PX (SUBTITLE_CONTENT_PX + SUBTITLE_PAD_PX * 2)
#define SUBTITLE_SCROLL_PX_PER_S 12
static lv_obj_t *s_toast_box;
static lv_obj_t *s_toast_lbl;
static lv_timer_t *s_toast_timer;
static uint32_t s_toast_until;
static bool s_ui_ready;
static panda_state_screen_t s_last_screen = PANDA_STATE_SCREEN_BOOT;

/* Provisioning QR page (SoftAP join code plus the 192.168.4.1 URL). */
static lv_obj_t *s_qr_screen;
static lv_obj_t *s_qr_canvas;
static lv_obj_t *s_qr_hint;
static uint8_t *s_qr_buf;
static bool s_qr_visible;

/* MP3 page. The decode task lives in panda_mp3; this page only shows state
 * and forwards touch controls through registered hooks. */
typedef struct {
    void (*prev)(void);
    void (*toggle)(void);
    void (*next)(void);
    void (*exit_page)(void);
} music_hooks_t;

static music_hooks_t s_music_hooks;
static lv_obj_t *s_music_screen;
static lv_obj_t *s_music_title_lbl;
static lv_obj_t *s_music_state_lbl;
static lv_obj_t *s_music_index_lbl;
static lv_obj_t *s_music_toggle_lbl;

static void ui_lock(void)
{
    /* Wait for the LVGL task instead of timing out: the old 1 s timeout
     * returned without the lock and ui_unlock() then gave a semaphore we
     * did not own, corrupting the display lock after a slow render. */
    int64_t t0 = esp_timer_get_time();
    if (!panda_display_lock(0)) {
        ESP_LOGE(TAG, "display lock unavailable");
        return;
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > 150000) {
        ESP_LOGW(TAG, "display lock wait %lld ms", (long long)(dt / 1000));
    }
}

static void ui_unlock(void)
{
    panda_display_unlock();
}

static void show_screen(lv_obj_t *scr)
{
    if (scr == NULL || lv_screen_active() == scr) {
        return;
    }
    uint32_t ms = scr == ui_face_sleep && !s_sleep_anim ? 0 : s_anim_ms;
    if (ms == 0) {
        lv_screen_load(scr);
        return;
    }
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_FADE_ON, ms, 0, false);
}

static lv_obj_t *screen_for(panda_state_screen_t screen)
{
    switch (screen) {
    case PANDA_STATE_SCREEN_BOOT:
    case PANDA_STATE_SCREEN_WAKE:      return ui_wakeup;
    case PANDA_STATE_SCREEN_IDLE:      return ui_home;
    case PANDA_STATE_SCREEN_LISTENING: return ui_listening;
    case PANDA_STATE_SCREEN_THINKING:  return ui_thinking;
    case PANDA_STATE_SCREEN_SPEAKING:  return ui_speaking;
    case PANDA_STATE_SCREEN_OFFLINE:   return ui_network_lost;
    case PANDA_STATE_SCREEN_RECONNECTING: return ui_reconnecting;
    case PANDA_STATE_SCREEN_ERROR_ASR: return ui_asr_failed;
    case PANDA_STATE_SCREEN_ERROR_LLM: return ui_llm_error;
    case PANDA_STATE_SCREEN_ERROR_TTS: return ui_tts_error;
    case PANDA_STATE_SCREEN_LOW_BATTERY: return ui_battery_low;
    case PANDA_STATE_SCREEN_SLEEP:     return ui_face_sleep;
    case PANDA_STATE_SCREEN_FACE_HAPPY: return ui_face_happy;
    case PANDA_STATE_SCREEN_FACE_CURIOUS: return ui_face_curious;
    case PANDA_STATE_SCREEN_FACE_SURPRISED: return ui_face_surprised;
    case PANDA_STATE_SCREEN_FACE_SAD: return ui_face_sad;
    case PANDA_STATE_SCREEN_FACE_SHY: return ui_face_shy;
    case PANDA_STATE_SCREEN_FACE_SLEEPY: return ui_face_sleepy;
    case PANDA_STATE_SCREEN_FACE_ANGRY: return ui_face_angry;
    default: return NULL;
    }
}

/* Settings/preview pages are owned by generated UI gestures. The state
 * machine must not yank the user back to the face screen unless something
 * urgent (voice turn, error, low battery, sleep) happens. */
static bool screen_is_user_page(lv_obj_t *scr)
{
    return scr == ui_quick_settings || scr == ui_time_weather ||
           scr == ui_notification || scr == ui_personality ||
           scr == ui_sleep_settings || scr == ui_system_settings ||
           scr == ui_system_info || scr == ui_character_preview_1 ||
           scr == ui_character_preview_2 || scr == ui_state_preview ||
           scr == ui_expression_manager || scr == ui_animation_manager ||
           scr == s_qr_screen || scr == s_music_screen;
}

/* These screens already draw their own bottom tip pill; the status overlay
 * would sit on top of it and both would crowd the round bezel. */
static bool screen_has_tip(panda_state_screen_t screen)
{
    switch (screen) {
    case PANDA_STATE_SCREEN_OFFLINE:
    case PANDA_STATE_SCREEN_RECONNECTING:
    case PANDA_STATE_SCREEN_ERROR_ASR:
    case PANDA_STATE_SCREEN_ERROR_LLM:
    case PANDA_STATE_SCREEN_ERROR_TTS:
    case PANDA_STATE_SCREEN_LOW_BATTERY:
        return true;
    default:
        return false;
    }
}

static bool screen_is_urgent(panda_state_screen_t screen)
{
    switch (screen) {
    case PANDA_STATE_SCREEN_LISTENING:
    case PANDA_STATE_SCREEN_THINKING:
    case PANDA_STATE_SCREEN_SPEAKING:
    case PANDA_STATE_SCREEN_OFFLINE:
    case PANDA_STATE_SCREEN_RECONNECTING:
    case PANDA_STATE_SCREEN_ERROR_ASR:
    case PANDA_STATE_SCREEN_ERROR_LLM:
    case PANDA_STATE_SCREEN_ERROR_TTS:
    case PANDA_STATE_SCREEN_LOW_BATTERY:
    case PANDA_STATE_SCREEN_SLEEP:
        return true;
    default:
        return false;
    }
}

static void add_gesture_bubble(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        add_gesture_bubble(lv_obj_get_child(obj, i));
    }
}

static lv_obj_t *make_pill(lv_obj_t *parent, int width, int y)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, width);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_align(box, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(box, y);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(box, 176, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    return box;
}

static lv_obj_t *make_pill_label(lv_obj_t *box)
{
    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &ui_font_Small16, 0);
    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
    return lbl;
}

static void toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast_box && lv_tick_get() >= s_toast_until) {
        lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_overlays(void)
{
    lv_obj_t *top = lv_layer_top();

    /* 360x360 round panel: keep every pill corner within ~172 px of the
     * centre, otherwise the bezel clips its left/right ends. The old
     * 300-340 px bars looked like black chords cut by the circle. */
    s_status_box = make_pill(top, 200, -44);
    s_status_lbl = make_pill_label(s_status_box);

    /* Subtitle stays in the bottom quarter of the panel: top edge at y=270,
     * 200 px wide keeps both bottom corners inside the round bezel. */
    s_subtitle_box = make_pill(top, 200, -42);
    lv_obj_set_style_pad_all(s_subtitle_box, SUBTITLE_PAD_PX, 0);
    lv_obj_set_height(s_subtitle_box, SUBTITLE_BOX_PX);
    s_subtitle_lbl = make_pill_label(s_subtitle_box);

    s_toast_box = make_pill(top, 240, 0);
    lv_obj_set_align(s_toast_box, LV_ALIGN_TOP_MID);
    lv_obj_set_y(s_toast_box, 64);
    s_toast_lbl = make_pill_label(s_toast_box);

    s_toast_timer = lv_timer_create(toast_timer_cb, 100, NULL);
}

/* ── Provisioning QR ─────────────────────────────────────────────────── */

/* The generator calls this while we hold the display lock. It rasterises the
 * modules into an L8 canvas in PSRAM: 200 px with a 2-module quiet zone
 * stays inside the round bezel when centred slightly above the middle. */
static void qr_display_cb(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);
    if (size <= 0 || s_qr_screen == NULL) {
        return;
    }
    int modules = size + 4;
    int scale = 200 / modules;
    if (scale < 3) scale = 3;
    if (scale > 8) scale = 8;
    int px = modules * scale;
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)px, LV_COLOR_FORMAT_L8);
    uint8_t *buf = heap_caps_malloc(stride * (size_t)px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "QR buffer alloc failed (%d px)", px);
        return;
    }
    memset(buf, 0xFF, stride * (size_t)px);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (!esp_qrcode_get_module(qr, x, y)) {
                continue;
            }
            int px0 = (x + 2) * scale;
            int py0 = (y + 2) * scale;
            for (int dy = 0; dy < scale; dy++) {
                memset(buf + (size_t)(py0 + dy) * stride + px0, 0x00, (size_t)scale);
            }
        }
    }
    if (s_qr_canvas == NULL) {
        s_qr_canvas = lv_canvas_create(s_qr_screen);
        lv_obj_remove_style_all(s_qr_canvas);
        lv_obj_align(s_qr_canvas, LV_ALIGN_CENTER, 0, -6);
    }
    lv_canvas_set_buffer(s_qr_canvas, buf, px, px, LV_COLOR_FORMAT_L8);
    if (s_qr_buf != NULL) {
        heap_caps_free(s_qr_buf);
    }
    s_qr_buf = buf;
}

void panda_ui_show_provisioning_qr(const char *ap_ssid, const char *ap_pass, const char *url)
{
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    if (s_qr_screen == NULL) {
        s_qr_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_qr_screen, lv_color_hex(0x0B0F14), 0);
        lv_obj_remove_flag(s_qr_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(s_qr_screen);
        lv_label_set_text(title, "配网模式");
        lv_obj_set_style_text_font(title, &ui_font_Title22, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

        s_qr_hint = lv_label_create(s_qr_screen);
        lv_obj_set_width(s_qr_hint, 260);
        lv_label_set_long_mode(s_qr_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_qr_hint, &ui_font_CJK16, 0);
        lv_obj_set_style_text_color(s_qr_hint, lv_color_hex(0x9AA4AE), 0);
        lv_obj_set_style_text_align(s_qr_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_qr_hint, LV_ALIGN_BOTTOM_MID, 0, -22);
    }

    if (!s_qr_visible) {
        if (s_qr_canvas != NULL) {
            lv_obj_del(s_qr_canvas);
            s_qr_canvas = NULL;
        }
        heap_caps_free(s_qr_buf);
        s_qr_buf = NULL;

        char payload[160];
        if (ap_pass != NULL && strlen(ap_pass) >= 8) {
            snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%.32s;P:%.64s;;",
                     ap_ssid ? ap_ssid : "", ap_pass);
        } else {
            snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%.32s;;",
                     ap_ssid ? ap_ssid : "");
        }
        esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
        cfg.display_func = qr_display_cb;
        cfg.max_qrcode_version = 10;
        if (esp_qrcode_generate(&cfg, payload) != ESP_OK) {
            ESP_LOGW(TAG, "QR encode failed");
        }

        char hint[160];
        snprintf(hint, sizeof(hint), "扫码加入热点\n%s\n或访问 %s",
                 ap_ssid ? ap_ssid : "", url ? url : "http://192.168.4.1");
        lv_label_set_text(s_qr_hint, hint);

        s_qr_visible = true;
        show_screen(s_qr_screen);
    }
    ui_unlock();
}

void panda_ui_hide_provisioning_qr(void)
{
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    if (s_qr_visible) {
        if (lv_screen_active() == s_qr_screen) {
            show_screen(ui_home);
        }
        if (s_qr_canvas != NULL) {
            lv_obj_del(s_qr_canvas);
            s_qr_canvas = NULL;
        }
        heap_caps_free(s_qr_buf);
        s_qr_buf = NULL;
        s_qr_visible = false;
    }
    ui_unlock();
}

bool panda_ui_provisioning_qr_visible(void)
{
    return s_qr_visible;
}

/* ── MP3 page ────────────────────────────────────────────────────────── */

typedef enum {
    MUSIC_BTN_PREV = 0,
    MUSIC_BTN_TOGGLE,
    MUSIC_BTN_NEXT,
    MUSIC_BTN_EXIT,
} music_btn_t;

static void music_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    switch ((music_btn_t)(intptr_t)lv_event_get_user_data(e)) {
    case MUSIC_BTN_PREV:
        if (s_music_hooks.prev) s_music_hooks.prev();
        break;
    case MUSIC_BTN_TOGGLE:
        if (s_music_hooks.toggle) s_music_hooks.toggle();
        break;
    case MUSIC_BTN_NEXT:
        if (s_music_hooks.next) s_music_hooks.next();
        break;
    case MUSIC_BTN_EXIT:
        if (s_music_hooks.exit_page) s_music_hooks.exit_page();
        break;
    default:
        break;
    }
}

static lv_obj_t *make_music_button(lv_obj_t *parent, const char *text, int x,
                                   int width, music_btn_t id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, width, 44);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, 70);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2F36), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, music_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &ui_font_CJK16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    return lbl;
}

static lv_obj_t *create_music_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(scr);
    lv_obj_set_size(badge, 88, 88);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x1B2430), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *icon = lv_label_create(badge);
    lv_label_set_text(icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x7FE0C1), 0);
    lv_obj_center(icon);

    s_music_title_lbl = lv_label_create(scr);
    lv_obj_set_width(s_music_title_lbl, 240);
    lv_label_set_long_mode(s_music_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_music_title_lbl, "无歌曲");
    lv_obj_set_style_text_font(s_music_title_lbl, &ui_font_Body18, 0);
    lv_obj_set_style_text_color(s_music_title_lbl, lv_color_hex(0x7FE0C1), 0);
    lv_obj_align(s_music_title_lbl, LV_ALIGN_CENTER, 0, -34);

    s_music_state_lbl = lv_label_create(scr);
    lv_label_set_text(s_music_state_lbl, "未播放");
    lv_obj_set_style_text_font(s_music_state_lbl, &ui_font_CJK16, 0);
    lv_obj_set_style_text_color(s_music_state_lbl, lv_color_hex(0x9AA4AE), 0);
    lv_obj_align(s_music_state_lbl, LV_ALIGN_CENTER, 0, 2);

    make_music_button(scr, "上一首", -92, 76, MUSIC_BTN_PREV);
    s_music_toggle_lbl = make_music_button(scr, "播放", 0, 76, MUSIC_BTN_TOGGLE);
    make_music_button(scr, "下一首", 92, 76, MUSIC_BTN_NEXT);

    s_music_index_lbl = lv_label_create(scr);
    lv_label_set_text(s_music_index_lbl, "-/-");
    lv_obj_set_style_text_font(s_music_index_lbl, &ui_font_Small16, 0);
    lv_obj_set_style_text_color(s_music_index_lbl, lv_color_hex(0x9AA4AE), 0);
    lv_obj_align(s_music_index_lbl, LV_ALIGN_CENTER, 0, 124);
    return scr;
}

bool panda_ui_is_music_page(void)
{
    if (!s_ui_ready || s_music_screen == NULL) {
        return false;
    }
    ui_lock();
    bool on = lv_screen_active() == s_music_screen;
    ui_unlock();
    return on;
}

void panda_ui_set_music(const char *title, bool playing, int index, int count)
{
    if (!s_ui_ready || s_music_title_lbl == NULL) {
        return;
    }
    char idx[24];
    if (count > 0 && index >= 0) {
        snprintf(idx, sizeof(idx), "%d/%d", index + 1, count);
    } else {
        snprintf(idx, sizeof(idx), "-/-");
    }
    const char *state = "未播放";
    if (count <= 0) {
        state = "SD 卡 Music 里没有 MP3";
    } else if (playing) {
        state = "播放中";
    } else {
        state = "已暂停";
    }

    ui_lock();
    lv_label_set_text(s_music_title_lbl, title != NULL && title[0] ? title : "无歌曲");
    lv_label_set_text(s_music_state_lbl, state);
    lv_label_set_text(s_music_index_lbl, idx);
    if (s_music_toggle_lbl != NULL) {
        lv_label_set_text(s_music_toggle_lbl, playing ? "暂停" : "播放");
    }
    ui_unlock();
}

void panda_ui_set_music_hooks(void (*prev)(void), void (*toggle)(void),
                              void (*next)(void), void (*exit_page)(void))
{
    s_music_hooks.prev = prev;
    s_music_hooks.toggle = toggle;
    s_music_hooks.next = next;
    s_music_hooks.exit_page = exit_page;
}

static void update_status_box(void)
{
    if (s_status_box == NULL) {
        return;
    }
    if (s_status[0] == '\0') {
        lv_obj_add_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_status_lbl, s_status);
        lv_obj_remove_flag(s_status_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void subtitle_scroll_exec(void *var, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)var, value);
}

static void update_subtitle_box(void)
{
    if (s_subtitle_box == NULL) {
        return;
    }
    lv_anim_delete(s_subtitle_lbl, subtitle_scroll_exec);
    lv_obj_set_y(s_subtitle_lbl, 0);
    if (s_subtitle[0] == '\0') {
        lv_obj_add_flag(s_subtitle_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_subtitle_lbl, s_subtitle);
    lv_obj_remove_flag(s_subtitle_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_subtitle_lbl);

    /* The label is centred in the pill; scroll offsets are therefore
     * symmetric around 0. Start with the top edge at the content bottom and
     * end with the bottom edge at the content top. */
    int32_t label_h = lv_obj_get_height(s_subtitle_lbl);
    bool scrolling = label_h > SUBTITLE_CONTENT_PX;
    if (scrolling) {
        int32_t span = label_h + SUBTITLE_CONTENT_PX;
        uint32_t duration = (uint32_t)(span * 1000 / SUBTITLE_SCROLL_PX_PER_S);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, s_subtitle_lbl);
        lv_anim_set_exec_cb(&anim, subtitle_scroll_exec);
        lv_anim_set_values(&anim, span / 2, -(span / 2));
        lv_anim_set_duration(&anim, duration);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_obj_set_y(s_subtitle_lbl, span / 2);
        lv_anim_start(&anim);
    }
    ESP_LOGI(TAG, "subtitle %uB label=%dx%d box=%dx%d scroll=%d",
             (unsigned)strlen(s_subtitle),
             (int)lv_obj_get_width(s_subtitle_lbl), (int)lv_obj_get_height(s_subtitle_lbl),
             (int)lv_obj_get_width(s_subtitle_box), (int)lv_obj_get_height(s_subtitle_box),
             (int)scrolling);
}

static void update_volume(void)
{
    if (ui_vol_lbl != NULL) {
        char buf[32];
        if (s_muted) {
            snprintf(buf, sizeof(buf), "静音");
        } else {
            snprintf(buf, sizeof(buf), "音量 %d%%", s_volume);
        }
        lv_label_set_text(ui_vol_lbl, buf);
    }
    if (ui_volm_v != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(ui_volm_v, buf);
    }
    if (s_vol_slider != NULL && lv_slider_get_value(s_vol_slider) != s_volume) {
        lv_slider_set_value(s_vol_slider, s_volume, LV_ANIM_OFF);
    }
}

static void update_brightness(void)
{
    if (ui_bri_v != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(ui_bri_v, buf);
    }
    if (s_bri_slider != NULL && lv_slider_get_value(s_bri_slider) != s_brightness) {
        lv_slider_set_value(s_bri_slider, s_brightness, LV_ANIM_OFF);
    }
}

static void update_wifi(void)
{
    const char *text = "未连接";
    const char *net = "未连接";
    if (s_wifi_ap) {
        text = "配网";
        net = "配网中";
    } else if (s_wifi_connecting) {
        text = "连接中";
        net = "连接中";
    } else if (s_wifi_connected) {
        text = "Wi-Fi";
        net = "Wi-Fi 已连接";
    }
    if (ui_wifi_lbl != NULL) {
        lv_label_set_text(ui_wifi_lbl, text);
    }
    if (ui_net_v != NULL) {
        lv_label_set_text(ui_net_v, net);
    }
    if (ui_wifi_on != NULL) {
        /* The blue overlay means "radio in use", not just "IP acquired";
         * AP mode and the reconnect window should both look enabled. */
        if (s_wifi_connected || s_wifi_connecting || s_wifi_ap) {
            lv_obj_remove_flag(ui_wifi_on, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_wifi_on, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_dnd(void)
{
    if (ui_dnd_on == NULL) {
        return;
    }
    if (s_dnd) {
        lv_obj_remove_flag(ui_dnd_on, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_dnd_on, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_time_sync(void)
{
    if (ui_tsync_v != NULL) {
        lv_label_set_text(ui_tsync_v, s_time_valid ? "已同步" : "未同步");
    }
}

static void update_weather(void)
{
    static const lv_image_dsc_t *const icons[] = {
        &ui_img_icon_sun_png, &ui_img_icon_cloud_sun_png, &ui_img_icon_cloud_off_png,
    };
    if (ui_wx_temp != NULL) {
        char buf[16];
        if (s_weather_valid) {
            snprintf(buf, sizeof(buf), "%d°C", s_weather_temp);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(ui_wx_temp, buf);
    }
    if (ui_wx_icon != NULL) {
        int icon = (int)s_weather_icon;
        if (icon < 0 || icon > 2) {
            icon = 2;
        }
        lv_image_set_src(ui_wx_icon, icons[icon]);
    }
}

static void update_sd(void)
{
    if (ui_sd_v == NULL) {
        return;
    }
    if (!s_sd_mounted || s_sd_total_kb == 0) {
        lv_label_set_text(ui_sd_v, "未挂载");
        if (ui_sd_s_fill != NULL) {
            lv_obj_add_flag(ui_sd_s_fill, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    uint64_t used_kb = s_sd_total_kb > s_sd_free_kb ? s_sd_total_kb - s_sd_free_kb : 0;
    char buf[40];
    if (s_sd_total_kb < 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%llu / %llu MB",
                 (unsigned long long)(used_kb / 1024), (unsigned long long)(s_sd_total_kb / 1024));
    } else {
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB",
                 (double)used_kb / 1048576.0, (double)s_sd_total_kb / 1048576.0);
    }
    lv_label_set_text(ui_sd_v, buf);
    if (ui_sd_s_fill != NULL) {
        int w = (int)((used_kb * 236ULL) / s_sd_total_kb);
        if (w < 0) w = 0;
        if (w > 236) w = 236;
        if (w == 0) {
            lv_obj_add_flag(ui_sd_s_fill, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(ui_sd_s_fill, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(ui_sd_s_fill, w);
            lv_obj_set_x(ui_sd_s_fill, -118 + w / 2);
        }
    }
}

static void update_version(void)
{
    if (ui_ver_v == NULL) {
        return;
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    char buf[40];
    snprintf(buf, sizeof(buf), "v%.31s", desc->version);
    lv_label_set_text(ui_ver_v, buf);
}

static void apply_option_row(lv_obj_t *const *on, int count, int selected)
{
    for (int i = 0; i < count; i++) {
        if (on[i] == NULL) {
            continue;
        }
        if (i == selected) {
            lv_obj_remove_flag(on[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(on[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void apply_menu_selection(void)
{
    lv_obj_t *pers_on[3] = {ui_pers_0_on, ui_pers_1_on, ui_pers_2_on};
    lv_obj_t *fb_on[3] = {ui_fb_0_on, ui_fb_1_on, ui_fb_2_on};
    lv_obj_t *act_on[3] = {ui_act_0_on, ui_act_1_on, ui_act_2_on};
    lv_obj_t *auto_on[4] = {ui_auto_0_on, ui_auto_1_on, ui_auto_2_on, ui_auto_3_on};
    lv_obj_t *wake_on[3] = {ui_wake_0_on, ui_wake_1_on, ui_wake_2_on};

    apply_option_row(pers_on, 3, s_personality);
    apply_option_row(fb_on, 3, s_feedback);
    apply_option_row(act_on, 3, s_animation);
    apply_option_row(auto_on, 4, s_auto_sleep_s == 60 ? 0 : s_auto_sleep_s == 300 ? 1
                                : s_auto_sleep_s == 600 ? 2 : 3);
    apply_option_row(wake_on, 3, s_wake_mode);

    if (ui_sw_anim_on != NULL) {
        if (s_sleep_anim) {
            lv_obj_remove_flag(ui_sw_anim_on, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_sw_anim_on, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui_sw_anim_knob_on != NULL) {
        if (s_sleep_anim) {
            lv_obj_remove_flag(ui_sw_anim_knob_on, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_sw_anim_knob_on, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_anim_level(int level)
{
    s_animation = level;
    s_anim_ms = level <= 0 ? 0 : level == 2 ? 250 : 150;
}

typedef enum {
    MENU_GROUP_PERSONALITY = 0,
    MENU_GROUP_FEEDBACK,
    MENU_GROUP_ANIMATION,
    MENU_GROUP_AUTO_SLEEP,
    MENU_GROUP_WAKE,
    MENU_GROUP_SLEEP_ANIM,
    MENU_GROUP_WIFI,
    MENU_GROUP_DND,
    MENU_GROUP_SLEEP_NOW,
} menu_group_t;

static int auto_sleep_from_option(int index)
{
    switch (index) {
    case 0: return 60;
    case 1: return 300;
    case 2: return 600;
    default: return 0;
    }
}

static void menu_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t code = (uintptr_t)lv_event_get_user_data(e);
    int group = (int)(code >> 4);
    int index = (int)(code & 0x0F);

    switch (group) {
    case MENU_GROUP_PERSONALITY:
        s_personality = index;
        apply_menu_selection();
        if (s_hooks.personality) {
            s_hooks.personality(s_personality, s_feedback, s_animation);
        }
        panda_ui_toast("性格已保存，下一轮对话生效", 1200);
        break;
    case MENU_GROUP_FEEDBACK:
        s_feedback = index;
        apply_menu_selection();
        if (s_hooks.personality) {
            s_hooks.personality(s_personality, s_feedback, s_animation);
        }
        panda_ui_toast("反馈强度已保存", 900);
        break;
    case MENU_GROUP_ANIMATION:
        set_anim_level(index);
        apply_menu_selection();
        if (s_hooks.personality) {
            s_hooks.personality(s_personality, s_feedback, s_animation);
        }
        panda_ui_toast("动画活跃度已保存", 900);
        break;
    case MENU_GROUP_AUTO_SLEEP:
        s_auto_sleep_s = auto_sleep_from_option(index);
        apply_menu_selection();
        if (s_hooks.sleep_config) {
            s_hooks.sleep_config(s_auto_sleep_s, s_wake_mode, s_sleep_anim);
        }
        panda_ui_toast(index == 3 ? "自动休眠已关闭" : "自动休眠已更新", 900);
        break;
    case MENU_GROUP_WAKE:
        s_wake_mode = index;
        apply_menu_selection();
        if (s_hooks.sleep_config) {
            s_hooks.sleep_config(s_auto_sleep_s, s_wake_mode, s_sleep_anim);
        }
        panda_ui_toast(index == 1 ? "语音唤醒暂不支持，触摸仍可唤醒"
                                 : "唤醒方式已保存", 1400);
        break;
    case MENU_GROUP_SLEEP_ANIM:
        s_sleep_anim = !s_sleep_anim;
        apply_menu_selection();
        if (s_hooks.sleep_config) {
            s_hooks.sleep_config(s_auto_sleep_s, s_wake_mode, s_sleep_anim);
        }
        panda_ui_toast(s_sleep_anim ? "睡眠动画已开启" : "睡眠动画已关闭", 900);
        break;
    case MENU_GROUP_WIFI: {
        bool radio_on = s_wifi_connected || s_wifi_connecting || s_wifi_ap;
        bool enable = !radio_on;
        s_wifi_connected = false;
        s_wifi_ap = false;
        s_wifi_connecting = enable;
        update_wifi();
        if (s_hooks.wifi) {
            s_hooks.wifi(enable);
        }
        panda_ui_toast(enable ? "正在开启 Wi-Fi" : "Wi-Fi 已关闭", 1000);
        break;
    }
    case MENU_GROUP_DND:
        s_dnd = !s_dnd;
        update_dnd();
        if (s_hooks.dnd) {
            s_hooks.dnd(s_dnd);
        }
        panda_ui_toast(s_dnd ? "勿扰已开启（扬声器静音）" : "勿扰已关闭", 1100);
        break;
    case MENU_GROUP_SLEEP_NOW:
        if (s_hooks.sleep_now) {
            s_hooks.sleep_now();
        }
        break;
    default:
        break;
    }
}

static void bind_menu_click(int group, int index, lv_obj_t *a, lv_obj_t *b,
                            lv_obj_t *c, lv_obj_t *d)
{
    void *user_data = (void *)(uintptr_t)(((uintptr_t)group << 4) | (uintptr_t)index);
    lv_obj_t *objects[] = {a, b, c, d};
    for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); i++) {
        if (objects[i] != NULL) {
            lv_obj_add_event_cb(objects[i], menu_click_cb, LV_EVENT_CLICKED, user_data);
        }
    }
}

static void slider_event_cb(lv_event_t *e)
{
    if (s_menu_syncing) {
        return;
    }
    int which = (int)(uintptr_t)lv_event_get_user_data(e);
    bool commit = lv_event_get_code(e) == LV_EVENT_RELEASED;
    lv_obj_t *slider = lv_event_get_target(e);
    int value = (int)lv_slider_get_value(slider);

    if (which == 1) {
        s_volume = value;
        s_muted = value == 0;
        update_volume();
        if (s_hooks.volume) {
            s_hooks.volume(value, commit);
        }
    } else {
        s_brightness = value;
        update_brightness();
        if (s_hooks.brightness) {
            s_hooks.brightness(value, commit);
        }
    }
}

static lv_obj_t *create_menu_slider(int y, int which)
{
    lv_obj_t *slider = lv_slider_create(ui_system_settings);
    lv_obj_set_width(slider, 236);
    lv_obj_set_height(slider, 8);
    lv_obj_set_align(slider, LV_ALIGN_CENTER);
    lv_obj_set_y(slider, y);
    lv_obj_set_ext_click_area(slider, 18);
    lv_slider_set_range(slider, 0, 100);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x24304A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x40CAFB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, 255, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, 255, LV_PART_KNOB);
    lv_obj_set_style_width(slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    lv_slider_set_value(slider, which == 1 ? s_volume : s_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)which);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)which);
    return slider;
}

static void hide_widget(lv_obj_t *obj)
{
    if (obj != NULL) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_menu_controls(void)
{
    /* The SquareLine export draws the sliders as static track/fill/knob
     * objects. Replace them with real sliders at the same coordinates so
     * touch and keyboard input work, then hide the decorative copies. */
    hide_widget(ui_bri_s_track);
    hide_widget(ui_bri_s_fill);
    hide_widget(ui_bri_s_knob);
    hide_widget(ui_volm_s_track);
    hide_widget(ui_volm_s_fill);
    hide_widget(ui_volm_s_knob);
    s_bri_slider = create_menu_slider(-72, 0);
    s_vol_slider = create_menu_slider(-14, 1);

    /* No Bluetooth stack is linked, so hide the tile instead of pretending. */
    hide_widget(ui_bt_off);
    hide_widget(ui_bt_on);
    hide_widget(ui_bt_ic);
    hide_widget(ui_bt_lbl);
    if (ui_bright_btn != NULL) lv_obj_set_x(ui_bright_btn, 0);
    if (ui_bright_ic != NULL) lv_obj_set_x(ui_bright_ic, 0);
    if (ui_bright_lbl != NULL) lv_obj_set_x(ui_bright_lbl, 0);

    lv_obj_t *pers_off[3] = {ui_pers_0_off, ui_pers_1_off, ui_pers_2_off};
    lv_obj_t *pers_on[3] = {ui_pers_0_on, ui_pers_1_on, ui_pers_2_on};
    lv_obj_t *pers_t[3] = {ui_pers_0_t, ui_pers_1_t, ui_pers_2_t};
    lv_obj_t *fb_off[3] = {ui_fb_0_off, ui_fb_1_off, ui_fb_2_off};
    lv_obj_t *fb_on[3] = {ui_fb_0_on, ui_fb_1_on, ui_fb_2_on};
    lv_obj_t *fb_t[3] = {ui_fb_0_t, ui_fb_1_t, ui_fb_2_t};
    lv_obj_t *act_off[3] = {ui_act_0_off, ui_act_1_off, ui_act_2_off};
    lv_obj_t *act_on[3] = {ui_act_0_on, ui_act_1_on, ui_act_2_on};
    lv_obj_t *act_t[3] = {ui_act_0_t, ui_act_1_t, ui_act_2_t};
    lv_obj_t *auto_off[4] = {ui_auto_0_off, ui_auto_1_off, ui_auto_2_off, ui_auto_3_off};
    lv_obj_t *auto_on[4] = {ui_auto_0_on, ui_auto_1_on, ui_auto_2_on, ui_auto_3_on};
    lv_obj_t *auto_t[4] = {ui_auto_0_t, ui_auto_1_t, ui_auto_2_t, ui_auto_3_t};
    lv_obj_t *wake_off[3] = {ui_wake_0_off, ui_wake_1_off, ui_wake_2_off};
    lv_obj_t *wake_on[3] = {ui_wake_0_on, ui_wake_1_on, ui_wake_2_on};
    lv_obj_t *wake_t[3] = {ui_wake_0_t, ui_wake_1_t, ui_wake_2_t};

    for (int i = 0; i < 3; i++) {
        bind_menu_click(MENU_GROUP_PERSONALITY, i, pers_off[i], pers_on[i], pers_t[i], NULL);
        bind_menu_click(MENU_GROUP_FEEDBACK, i, fb_off[i], fb_on[i], fb_t[i], NULL);
        bind_menu_click(MENU_GROUP_ANIMATION, i, act_off[i], act_on[i], act_t[i], NULL);
        bind_menu_click(MENU_GROUP_WAKE, i, wake_off[i], wake_on[i], wake_t[i], NULL);
    }
    for (int i = 0; i < 4; i++) {
        bind_menu_click(MENU_GROUP_AUTO_SLEEP, i, auto_off[i], auto_on[i], auto_t[i], NULL);
    }
    bind_menu_click(MENU_GROUP_SLEEP_ANIM, 0, ui_sw_anim_off, ui_sw_anim_on,
                    ui_sw_anim_knob_off, ui_sw_anim_knob_on);
    bind_menu_click(MENU_GROUP_WIFI, 0, ui_wifi_off, ui_wifi_on, ui_wifi_ic, ui_wifi_lbl);
    bind_menu_click(MENU_GROUP_DND, 0, ui_dnd_off, ui_dnd_on, ui_dnd_ic, ui_dnd_lbl);
    bind_menu_click(MENU_GROUP_SLEEP_NOW, 0, ui_sleep_btn, ui_sleep_ic, ui_sleep_lbl, NULL);
}

static panda_state_emotion_t state_emotion(panda_emotion_t emotion)
{
    switch (emotion) {
    case PANDA_EMOTION_HAPPY: return PANDA_STATE_EMOTION_HAPPY;
    case PANDA_EMOTION_SURPRISE: return PANDA_STATE_EMOTION_SURPRISED;
    case PANDA_EMOTION_SAD: return PANDA_STATE_EMOTION_SAD;
    case PANDA_EMOTION_SLEEPY: return PANDA_STATE_EMOTION_SLEEPY;
    case PANDA_EMOTION_CURIOUS: return PANDA_STATE_EMOTION_CURIOUS;
    case PANDA_EMOTION_SHY: return PANDA_STATE_EMOTION_SHY;
    case PANDA_EMOTION_ANGRY: return PANDA_STATE_EMOTION_ANGRY;
    default: return PANDA_STATE_EMOTION_NEUTRAL;
    }
}

static panda_emotion_t public_emotion(panda_state_emotion_t emotion)
{
    switch (emotion) {
    case PANDA_STATE_EMOTION_HAPPY: return PANDA_EMOTION_HAPPY;
    case PANDA_STATE_EMOTION_SURPRISED: return PANDA_EMOTION_SURPRISE;
    case PANDA_STATE_EMOTION_SAD: return PANDA_EMOTION_SAD;
    case PANDA_STATE_EMOTION_SLEEPY: return PANDA_EMOTION_SLEEPY;
    case PANDA_STATE_EMOTION_CURIOUS: return PANDA_EMOTION_CURIOUS;
    case PANDA_STATE_EMOTION_SHY: return PANDA_EMOTION_SHY;
    case PANDA_STATE_EMOTION_ANGRY: return PANDA_EMOTION_ANGRY;
    default: return PANDA_EMOTION_NEUTRAL;
    }
}

static panda_voice_ui_t public_voice(panda_state_voice_t voice)
{
    switch (voice) {
    case PANDA_STATE_VOICE_CONNECTING: return PANDA_VOICE_CONNECTING;
    case PANDA_STATE_VOICE_LISTENING: return PANDA_VOICE_LISTENING;
    case PANDA_STATE_VOICE_THINKING: return PANDA_VOICE_THINKING;
    case PANDA_STATE_VOICE_SPEAKING: return PANDA_VOICE_SPEAKING;
    case PANDA_STATE_VOICE_DISCONNECTED: return PANDA_VOICE_OFFLINE;
    case PANDA_STATE_VOICE_READY:
    case PANDA_STATE_VOICE_CANCELLING:
    default: return PANDA_VOICE_IDLE;
    }
}

static void dispatch_event(panda_state_event_type_t type, int value, const char *text)
{
    panda_state_event_t ev = {.type = type, .value = value, .text = text};
    panda_state_dispatch(&s_state, &ev, lv_tick_get());
}

/* Caller must hold the display lock (or already be in the LVGL task). */
static void apply_view_locked(void)
{
    panda_state_view(&s_state, &s_view);

    if (s_view.screen != s_last_screen) {
        ESP_LOGI(TAG, "screen %s -> %s", panda_state_screen_name(s_last_screen),
                 panda_state_screen_name(s_view.screen));
        s_last_screen = s_view.screen;
    }
    lv_obj_t *target = screen_for(s_view.screen);
    /* Voice reconnects and offline states must not yank the user out of a
     * menu page; the home screen still shows them as soon as it is loaded. */
    bool urgent = screen_is_urgent(s_view.screen) &&
                  s_view.screen != PANDA_STATE_SCREEN_OFFLINE &&
                  s_view.screen != PANDA_STATE_SCREEN_RECONNECTING;
    if (target != NULL && (!screen_is_user_page(lv_screen_active()) || urgent)) {
        show_screen(target);
    }

    const char *status = s_view.status;
    if (screen_has_tip(s_view.screen) || s_view.subtitle[0] != '\0') {
        status = "";
    }
    if (s_status_override && lv_tick_get() < s_status_override_until) {
        status = s_status;
    } else {
        s_status_override = false;
    }
    if (strcmp(status, s_status) != 0) {
        snprintf(s_status, sizeof(s_status), "%s", status);
        update_status_box();
    }
    if (strcmp(s_view.subtitle, s_subtitle) != 0) {
        snprintf(s_subtitle, sizeof(s_subtitle), "%s", s_view.subtitle);
        update_subtitle_box();
    }
}

static void apply_view(void)
{
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    apply_view_locked();
    ui_unlock();
}

static void state_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ui_ready) {
        return;
    }
    panda_state_tick(&s_state, lv_tick_get());
    apply_view_locked();
}

typedef struct {
    const char *name;
    lv_obj_t *scr;
} named_screen_t;


typedef struct {
    const char *screen;
    int objects;
    int failures;
    int logged;
    const lv_image_dsc_t *seen[128];
    int seen_count;
} image_check_t;

static uint32_t expected_image_size(const lv_image_dsc_t *dsc)
{
    uint32_t px = (uint32_t)dsc->header.w * dsc->header.h;
    switch ((lv_color_format_t)dsc->header.cf) {
    case LV_COLOR_FORMAT_RGB565:    return px * 2;
    case LV_COLOR_FORMAT_RGB565A8:  return px * 3;
    case LV_COLOR_FORMAT_I8:        return px + 1024;
    case LV_COLOR_FORMAT_I4:        return (px + 1) / 2 + 64;
    case LV_COLOR_FORMAT_I2:        return (px + 3) / 4 + 16;
    default:                        return 0; /* unchecked format */
    }
}

static void image_fail(image_check_t *ctx, int ordinal, const char *why)
{
    ctx->failures++;
    if (ctx->logged < 8) {
        ctx->logged++;
        ESP_LOGE(TAG, "image check: screen=%s image#%d: %s", ctx->screen, ordinal, why);
    }
}

static void check_image_tree(lv_obj_t *obj, image_check_t *ctx)
{
    if (obj == NULL) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_image_class)) {
        ctx->objects++;
        int ordinal = ctx->objects;
        const void *src = lv_image_get_src(obj);
        if (src == NULL) {
            image_fail(ctx, ordinal, "no source set");
        } else if (lv_image_src_get_type(src) != LV_IMAGE_SRC_VARIABLE) {
            image_fail(ctx, ordinal, "source is not a compiled image");
        } else {
            const lv_image_dsc_t *dsc = (const lv_image_dsc_t *)src;
            bool ok = true;
            if (dsc->header.magic != LV_IMAGE_HEADER_MAGIC) {
                image_fail(ctx, ordinal, "bad header magic");
                ok = false;
            }
            if (dsc->header.w == 0 || dsc->header.h == 0) {
                image_fail(ctx, ordinal, "zero width/height");
                ok = false;
            }
            if (dsc->data == NULL || dsc->data_size == 0) {
                image_fail(ctx, ordinal, "empty pixel data");
                ok = false;
            } else {
                uint32_t want = expected_image_size(dsc);
                if (want != 0 && dsc->data_size != want) {
                    image_fail(ctx, ordinal, "data_size does not match format/size");
                    ok = false;
                }
            }
            lv_image_header_t info;
            if (lv_image_decoder_get_info(src, &info) != LV_RESULT_OK) {
                image_fail(ctx, ordinal, "decoder rejected the image");
                ok = false;
            } else if (info.w != dsc->header.w || info.h != dsc->header.h) {
                image_fail(ctx, ordinal, "decoder size differs from descriptor");
                ok = false;
            }
            if (ok) {
                bool seen = false;
                for (int i = 0; i < ctx->seen_count; i++) {
                    if (ctx->seen[i] == dsc) {
                        seen = true;
                        break;
                    }
                }
                if (!seen && ctx->seen_count < (int)(sizeof(ctx->seen) / sizeof(ctx->seen[0]))) {
                    ctx->seen[ctx->seen_count++] = dsc;
                }
            }
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        check_image_tree(lv_obj_get_child(obj, i), ctx);
    }
}

esp_err_t panda_ui_verify_images(panda_ui_image_report_t *report)
{
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
    }
    if (!s_ui_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const named_screen_t s_all_screens[] = {
        {"home", ui_home}, {"wakeup", ui_wakeup}, {"listening", ui_listening},
        {"thinking", ui_thinking}, {"speaking", ui_speaking},
        {"asr_failed", ui_asr_failed}, {"llm_error", ui_llm_error},
        {"tts_error", ui_tts_error}, {"network_lost", ui_network_lost},
        {"reconnecting", ui_reconnecting}, {"battery_low", ui_battery_low},
        {"face_happy", ui_face_happy}, {"face_laugh", ui_face_laugh},
        {"face_excited", ui_face_excited}, {"face_curious", ui_face_curious},
        {"face_surprised", ui_face_surprised}, {"face_shy", ui_face_shy},
        {"face_sad", ui_face_sad}, {"face_confused", ui_face_confused},
        {"face_angry", ui_face_angry}, {"face_love", ui_face_love},
        {"face_sleepy", ui_face_sleepy}, {"face_sleep", ui_face_sleep},
        {"face_confident", ui_face_confident}, {"face_cried", ui_face_cried},
        {"time_weather", ui_time_weather}, {"notification", ui_notification},
        {"quick_settings", ui_quick_settings}, {"personality", ui_personality},
        {"sleep_settings", ui_sleep_settings}, {"system_settings", ui_system_settings},
        {"system_info", ui_system_info}, {"preview_1", ui_character_preview_1},
        {"preview_2", ui_character_preview_2}, {"state_preview", ui_state_preview},
        {"expression_manager", ui_expression_manager},
        {"animation_manager", ui_animation_manager},
    };
    image_check_t ctx = {0};
    ui_lock();
    int screens = (int)(sizeof(s_all_screens) / sizeof(s_all_screens[0]));
    for (int i = 0; i < screens; i++) {
        ctx.screen = s_all_screens[i].name;
        if (s_all_screens[i].scr == NULL) {
            image_fail(&ctx, 0, "screen not created");
            continue;
        }
        check_image_tree(s_all_screens[i].scr, &ctx);
    }
    ui_unlock();

    if (report != NULL) {
        report->screens = screens;
        report->image_objects = ctx.objects;
        report->unique_images = ctx.seen_count;
        report->failures = ctx.failures;
    }
    ESP_LOGI(TAG, "image check: %d screens, %d image objects, %d unique images, %d failures",
             screens, ctx.objects, ctx.seen_count, ctx.failures);
    return ctx.failures == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t panda_ui_init(void)
{
    if (!panda_display_ready()) {
        ESP_LOGW(TAG, "display not ready, UI disabled");
        return ESP_ERR_INVALID_STATE;
    }
    panda_state_init(&s_state, lv_tick_get());
    ui_lock();
    ui_init();
    create_menu_controls();
    s_music_screen = create_music_screen();
    add_gesture_bubble(ui_home);
    add_gesture_bubble(ui_wakeup);
    add_gesture_bubble(ui_listening);
    add_gesture_bubble(ui_thinking);
    add_gesture_bubble(ui_speaking);
    add_gesture_bubble(ui_asr_failed);
    add_gesture_bubble(ui_llm_error);
    add_gesture_bubble(ui_tts_error);
    add_gesture_bubble(ui_network_lost);
    add_gesture_bubble(ui_reconnecting);
    add_gesture_bubble(ui_battery_low);
    add_gesture_bubble(ui_face_happy);
    add_gesture_bubble(ui_face_laugh);
    add_gesture_bubble(ui_face_excited);
    add_gesture_bubble(ui_face_curious);
    add_gesture_bubble(ui_face_surprised);
    add_gesture_bubble(ui_face_shy);
    add_gesture_bubble(ui_face_sad);
    add_gesture_bubble(ui_face_confused);
    add_gesture_bubble(ui_face_angry);
    add_gesture_bubble(ui_face_love);
    add_gesture_bubble(ui_face_sleepy);
    add_gesture_bubble(ui_face_sleep);
    add_gesture_bubble(ui_face_confident);
    add_gesture_bubble(ui_face_cried);
    add_gesture_bubble(ui_time_weather);
    add_gesture_bubble(ui_notification);
    add_gesture_bubble(ui_quick_settings);
    add_gesture_bubble(ui_personality);
    add_gesture_bubble(ui_sleep_settings);
    add_gesture_bubble(ui_system_settings);
    add_gesture_bubble(ui_system_info);
    create_overlays();
    update_status_box();
    update_subtitle_box();
    update_wifi();
    update_volume();
    update_brightness();
    update_dnd();
    update_time_sync();
    update_weather();
    update_sd();
    update_version();
    apply_menu_selection();
    s_ui_ready = true;
    ui_unlock();

    s_state_timer = lv_timer_create(state_timer_cb, 50, NULL);
    if (s_state_timer == NULL) {
        ESP_LOGW(TAG, "state timer unavailable, UI will not auto-refresh");
    }
    apply_view();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "SquareLine UI ready (%d screens), lv_mem used=%u/%u KB max=%u KB (%u%%), frag=%u%%",
             37, (unsigned)((mon.total_size - mon.free_size) / 1024),
             (unsigned)(mon.total_size / 1024), (unsigned)(mon.max_used / 1024),
             mon.used_pct, mon.frag_pct);
    return ESP_OK;
}

void panda_ui_set_voice_state(panda_voice_ui_t state)
{
    switch (state) {
    case PANDA_VOICE_CONNECTING: dispatch_event(PANDA_STATE_EVT_VOICE_CONNECTING, 0, NULL); break;
    case PANDA_VOICE_LISTENING:  dispatch_event(PANDA_STATE_EVT_VOICE_LISTENING, 0, NULL); break;
    case PANDA_VOICE_THINKING:   dispatch_event(PANDA_STATE_EVT_VOICE_THINKING, 0, NULL); break;
    case PANDA_VOICE_SPEAKING:   dispatch_event(PANDA_STATE_EVT_VOICE_SPEAKING, 0, NULL); break;
    case PANDA_VOICE_OFFLINE:    dispatch_event(PANDA_STATE_EVT_VOICE_DISCONNECTED, 0, NULL); break;
    case PANDA_VOICE_ERROR:      dispatch_event(PANDA_STATE_EVT_ERROR, PANDA_STATE_ERROR_GENERIC, NULL); break;
    case PANDA_VOICE_IDLE:
    default:                     dispatch_event(PANDA_STATE_EVT_VOICE_IDLE, 0, NULL); break;
    }
    apply_view();
}

void panda_ui_set_emotion(panda_emotion_t emotion)
{
    dispatch_event(PANDA_STATE_EVT_EMOTION, (int)state_emotion(emotion), NULL);
    apply_view();
}

void panda_ui_set_device_state(panda_device_ui_t state)
{
    panda_state_event_t ev = {
        .type = PANDA_STATE_EVT_BATTERY,
        .flag = true,
        .flag2 = false,
        .value = 100,
    };
    switch (state) {
    case PANDA_DEVICE_BOOT:
        dispatch_event(PANDA_STATE_EVT_BOOT, 0, NULL);
        break;
    case PANDA_DEVICE_LOW_BATTERY:
        ev.value = 10;
        panda_state_dispatch(&s_state, &ev, lv_tick_get());
        break;
    case PANDA_DEVICE_CHARGING:
        ev.value = s_battery_pct >= 0 ? s_battery_pct : 100;
        ev.flag2 = true;
        panda_state_dispatch(&s_state, &ev, lv_tick_get());
        break;
    case PANDA_DEVICE_READY:
    default:
        dispatch_event(PANDA_STATE_EVT_READY, 0, NULL);
        break;
    }
    apply_view();
}

static panda_state_error_t state_error(panda_ui_error_t error)
{
    switch (error) {
    case PANDA_UI_ERROR_ASR: return PANDA_STATE_ERROR_ASR;
    case PANDA_UI_ERROR_LLM: return PANDA_STATE_ERROR_LLM;
    case PANDA_UI_ERROR_TTS: return PANDA_STATE_ERROR_TTS;
    case PANDA_UI_ERROR_AUDIO: return PANDA_STATE_ERROR_AUDIO;
    case PANDA_UI_ERROR_NETWORK: return PANDA_STATE_ERROR_NETWORK;
    case PANDA_UI_ERROR_AUTH: return PANDA_STATE_ERROR_AUTH;
    default: return PANDA_STATE_ERROR_GENERIC;
    }
}

void panda_ui_set_error(panda_ui_error_t error)
{
    dispatch_event(PANDA_STATE_EVT_ERROR, (int)state_error(error), NULL);
    apply_view();
}

void panda_ui_clear_error(void)
{
    dispatch_event(PANDA_STATE_EVT_CLEAR_ERROR, 0, NULL);
    apply_view();
}

void panda_ui_sleep(void)
{
    dispatch_event(PANDA_STATE_EVT_SLEEP, 0, NULL);
    apply_view();
}

void panda_ui_wake(void)
{
    dispatch_event(PANDA_STATE_EVT_WAKE, 0, NULL);
    apply_view();
}

void panda_ui_set_audio_level(uint8_t level_0_100)
{
    if (!s_ui_ready) {
        return;
    }
    static const int base[9] = {10, 18, 28, 16, 34, 20, 30, 14, 8};
    if (level_0_100 > 100) {
        level_0_100 = 100;
    }
    lv_obj_t *waves[9] = {
        ui_wave_1, ui_wave_2, ui_wave_3, ui_wave_4, ui_wave_5,
        ui_wave_6, ui_wave_7, ui_wave_8, ui_wave_9,
    };
    ui_lock();
    for (int i = 0; i < 9; i++) {
        if (waves[i] == NULL) {
            continue;
        }
        int scale = 40 + (level_0_100 * 120) / 100;
        int h = (base[i] * scale) / 100;
        if (h < 6) {
            h = 6;
        }
        if (h > 72) {
            h = 72;
        }
        lv_obj_set_height(waves[i], h);
    }
    ui_unlock();
}

void panda_ui_set_wifi(bool connected, bool ap_mode, bool connecting)
{
    s_wifi_connected = connected;
    s_wifi_ap = ap_mode;
    s_wifi_connecting = connecting;
    panda_state_event_t ev = {
        .type = PANDA_STATE_EVT_WIFI,
        .flag = connected,
        .flag2 = ap_mode,
        .flag3 = connecting,
    };
    panda_state_dispatch(&s_state, &ev, lv_tick_get());
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    update_wifi();
    ui_unlock();
    apply_view();
}

void panda_ui_set_battery(int percent, bool charging, bool present)
{
    s_battery_pct = percent;
    s_charging = charging;
    s_battery_present = present;
    panda_state_event_t ev = {
        .type = PANDA_STATE_EVT_BATTERY,
        .value = percent,
        .flag = present,
        .flag2 = charging,
    };
    panda_state_dispatch(&s_state, &ev, lv_tick_get());
    apply_view();
}

void panda_ui_set_time(int year, int month, int day, int hour, int minute, int second, bool valid)
{
    (void)second;
    s_time_valid = valid;
    if (!s_ui_ready) {
        return;
    }
    static const char *weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char clock[16] = "--:--";
    char date[48] = "时间未同步";

    if (valid) {
        snprintf(clock, sizeof(clock), "%02d:%02d", hour, minute);
        /* Zeller's congruence, 1=Sunday. */
        int y = year;
        int m = month;
        if (m < 3) {
            m += 12;
            y--;
        }
        int k = y % 100;
        int j = y / 100;
        int w = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        if (w < 0) {
            w += 7;
        }
        snprintf(date, sizeof(date), "%d月%d日 %s", month, day, weekdays[w]);
    }

    ui_lock();
    if (ui_clock) {
        lv_label_set_text(ui_clock, clock);
    }
    if (ui_date) {
        lv_label_set_text(ui_date, date);
    }
    update_time_sync();
    ui_unlock();
}

void panda_ui_set_volume(int percent, bool muted)
{
    s_volume = percent;
    s_muted = muted;
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    update_volume();
    ui_unlock();
}

void panda_ui_set_brightness(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    update_brightness();
    ui_unlock();
}

void panda_ui_set_weather(bool valid, int temp_c, panda_ui_weather_t icon)
{
    s_weather_valid = valid;
    s_weather_temp = temp_c;
    s_weather_icon = icon;
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    update_weather();
    ui_unlock();
}

void panda_ui_set_menu_hooks(const panda_ui_menu_hooks_t *hooks)
{
    if (hooks == NULL) {
        memset(&s_hooks, 0, sizeof(s_hooks));
        return;
    }
    s_hooks = *hooks;
}

void panda_ui_menu_sync(const panda_ui_menu_state_t *state)
{
    if (state == NULL) {
        return;
    }
    s_volume = state->volume < 0 ? 0 : state->volume > 100 ? 100 : state->volume;
    s_muted = s_volume == 0;
    s_brightness = state->brightness < 0 ? 0 : state->brightness > 100 ? 100 : state->brightness;
    s_personality = state->personality < 0 || state->personality > 2 ? 1 : state->personality;
    s_feedback = state->feedback < 0 || state->feedback > 2 ? 1 : state->feedback;
    set_anim_level(state->animation < 0 || state->animation > 2 ? 1 : state->animation);
    s_auto_sleep_s = state->auto_sleep_s;
    s_wake_mode = state->wake_mode < 0 || state->wake_mode > 2 ? 2 : state->wake_mode;
    s_sleep_anim = state->sleep_anim;
    s_dnd = state->dnd;
    s_wifi_connected = state->wifi_connected;
    s_wifi_connecting = state->wifi_connecting;
    s_wifi_ap = state->wifi_ap;
    s_sd_mounted = state->sd_mounted;
    s_sd_free_kb = state->sd_free_kb;
    s_sd_total_kb = state->sd_total_kb;

    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    s_menu_syncing = true;
    update_volume();
    update_brightness();
    update_wifi();
    update_dnd();
    update_time_sync();
    update_weather();
    update_sd();
    update_version();
    apply_menu_selection();
    s_menu_syncing = false;
    ui_unlock();
}

bool panda_ui_is_sleeping(void)
{
    panda_state_view_t view;
    panda_state_view(&s_state, &view);
    return view.screen == PANDA_STATE_SCREEN_SLEEP;
}

bool panda_ui_is_home_screen(void)
{
    if (!s_ui_ready) {
        return false;
    }
    bool home;
    ui_lock();
    home = lv_screen_active() == ui_home;
    ui_unlock();
    return home;
}

const char *panda_ui_menu_next(void)
{
    if (!s_ui_ready) {
        return NULL;
    }
    lv_obj_t *const screens[] = {
        ui_quick_settings, ui_personality, ui_sleep_settings,
        ui_system_settings, ui_system_info, ui_time_weather, s_music_screen,
    };
    static const char *const names[] = {
        "快捷设置", "人格设置", "睡眠设置", "系统设置", "设备信息", "时间天气",
        "音乐播放",
    };
    const int count = (int)(sizeof(screens) / sizeof(screens[0]));

    ui_lock();
    lv_obj_t *current = lv_screen_active();
    int next = 0;
    for (int i = 0; i < count; i++) {
        if (screens[i] == current) {
            next = (i + 1) % count;
            break;
        }
    }
    show_screen(screens[next]);
    ui_unlock();
    return names[next];
}

void panda_ui_menu_home(void)
{
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    show_screen(ui_home);
    ui_unlock();
}

void panda_ui_set_status(const char *status)
{
    snprintf(s_status, sizeof(s_status), "%s", status ? status : "");
    s_status_override = s_status[0] != '\0';
    s_status_override_until = lv_tick_get() + 5000;
    if (!s_ui_ready) {
        return;
    }
    ui_lock();
    update_status_box();
    ui_unlock();
}

void panda_ui_set_subtitle(const char *text)
{
    dispatch_event(PANDA_STATE_EVT_SUBTITLE, 0, text ? text : "");
    apply_view();
}

void panda_ui_toast(const char *text, uint32_t duration_ms)
{
    if (text == NULL || !s_ui_ready || s_toast_box == NULL) {
        return;
    }
    ui_lock();
    lv_label_set_text(s_toast_lbl, text);
    lv_obj_remove_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = lv_tick_get() + duration_ms;
    ui_unlock();
}

panda_voice_ui_t panda_ui_get_voice_state(void)
{
    panda_state_view_t v;
    panda_state_view(&s_state, &v);
    return public_voice(v.voice);
}

panda_emotion_t panda_ui_get_emotion(void)
{
    panda_state_view_t v;
    panda_state_view(&s_state, &v);
    return public_emotion(v.emotion);
}

const char *panda_ui_current_screen(void)
{
    panda_state_view_t v;
    panda_state_view(&s_state, &v);
    return panda_state_screen_name(v.screen);
}

const char *panda_ui_voice_state_name(panda_voice_ui_t state)
{
    switch (state) {
    case PANDA_VOICE_IDLE: return "待机";
    case PANDA_VOICE_CONNECTING: return "连接中";
    case PANDA_VOICE_LISTENING: return "聆听";
    case PANDA_VOICE_THINKING: return "思考";
    case PANDA_VOICE_SPEAKING: return "说话";
    case PANDA_VOICE_OFFLINE: return "离线";
    case PANDA_VOICE_ERROR: return "错误";
    default: return "未知";
    }
}

const char *panda_ui_emotion_name(panda_emotion_t emotion)
{
    switch (emotion) {
    case PANDA_EMOTION_NEUTRAL: return "平静";
    case PANDA_EMOTION_HAPPY: return "开心";
    case PANDA_EMOTION_SURPRISE: return "惊讶";
    case PANDA_EMOTION_SAD: return "难过";
    case PANDA_EMOTION_SLEEPY: return "困倦";
    case PANDA_EMOTION_CURIOUS: return "好奇";
    case PANDA_EMOTION_SHY: return "害羞";
    case PANDA_EMOTION_ANGRY: return "生气";
    default: return "未知";
    }
}
