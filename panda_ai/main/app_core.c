#include "app_core.h"
#include "app_provisioning.h"
#include "app_settings.h"
#include "panda_audio.h"
#include "panda_board.h"
#include "panda_connectivity.h"
#include "panda_display.h"
#include "panda_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "app";

typedef enum {
    EVT_TOUCH = 0,
    EVT_WIFI,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        struct {
            panda_touch_gesture_t gesture;
            int x;
            int y;
        } touch;
        struct {
            bool connected;
            bool ap;
            bool connecting;
        } wifi;
    } u;
} app_event_t;

static QueueHandle_t s_queue;
/* Voice events arrive from the websocket task, which must never wait for
 * the LVGL lock: a slow render there held the ws client lock long enough
 * to starve the uplink and stall TTS. Coalesce them and let app_core render. */
static SemaphoreHandle_t s_ui_pending_lock;
static bool s_voice_pending;
static panda_voice_ui_t s_pending_voice;
static bool s_subtitle_pending;
static char s_pending_subtitle[512];
static bool s_wifi_connected;
static bool s_wifi_ap;
static bool s_wifi_connecting = true;
static bool s_low_batt_warned;

#define KEY_VOLUME_STEP 10
/* BOOT is the only runtime key wired to provisioning; PTT is touch-only. */
#define BOOT_PROVISION_HOLD_US (4LL * 1000 * 1000)

static bool s_btn_last[4];
static int64_t s_btn_down_us[4];
static bool s_btn_long[4];
static bool s_btn_wake[4];
static bool s_touch_last;
static bool s_touch_wake;
static int64_t s_touch_down_us;
static uint16_t s_touch_start_x;
static uint16_t s_touch_start_y;
static bool s_touch_moved;
static int s_ptt_source; /* 0 none, 2 touch */
static int64_t s_last_active_us;

static void touch_activity(void)
{
    s_last_active_us = esp_timer_get_time();
}

static bool wake_if_sleeping(void)
{
    if (!panda_ui_is_sleeping()) {
        return false;
    }
    panda_ui_wake();
    touch_activity();
    ESP_LOGI(TAG, "wake by input");
    return true;
}

__attribute__((weak)) esp_err_t app_audio_test_start(void)
{
    ESP_LOGW(TAG, "audio pipeline not built yet");
    panda_ui_toast("音频模块待接入", 1500);
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) void app_voice_set_network(bool online)
{
    (void)online;
}

static void ptt_press(int source)
{
    if (s_ptt_source != 0) {
        return;
    }
    s_ptt_source = source;
    ESP_LOGI(TAG, "PTT press (source=touch)");
    app_voice_ptt_press();
}

static void ptt_release(int source)
{
    if (s_ptt_source != source) {
        return;
    }
    s_ptt_source = 0;
    ESP_LOGI(TAG, "PTT release (source=touch)");
    app_voice_ptt_release();
}

static void volume_step(int delta)
{
    panda_settings_t *cfg = panda_settings_get();
    int pct = cfg->volume + delta;
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "音量 %d%%", pct);
    if (pct == cfg->volume) {
        panda_ui_toast(buf, 600);
        return;
    }
    cfg->volume = pct;
    panda_settings_save();
    panda_audio_set_volume(pct);
    panda_ui_set_volume(pct, pct == 0);
    panda_ui_toast(buf, 800);
    ESP_LOGI(TAG, "key volume=%d%%", pct);
}

static void menu_volume_hook(int percent, bool commit)
{
    panda_settings_t *cfg = panda_settings_get();
    cfg->volume = percent;
    panda_audio_set_volume(percent);
    panda_ui_set_volume(percent, percent == 0);
    if (commit) {
        panda_settings_save();
        char buf[24];
        snprintf(buf, sizeof(buf), "音量 %d%%", percent);
        panda_ui_toast(buf, 800);
        ESP_LOGI(TAG, "menu volume=%d%%", percent);
    }
}

static void menu_brightness_hook(int percent, bool commit)
{
    panda_settings_t *cfg = panda_settings_get();
    cfg->brightness = percent;
    panda_display_set_brightness(percent);
    panda_ui_set_brightness(percent);
    if (commit) {
        panda_settings_save();
        char buf[24];
        snprintf(buf, sizeof(buf), "亮度 %d%%", percent);
        panda_ui_toast(buf, 800);
        ESP_LOGI(TAG, "menu brightness=%d%%", percent);
    }
}

static void menu_personality_hook(int personality, int feedback, int animation)
{
    panda_settings_t *cfg = panda_settings_get();
    cfg->personality = personality;
    cfg->feedback = feedback;
    cfg->animation = animation;
    panda_settings_save();
    ESP_LOGI(TAG, "menu personality=%d feedback=%d animation=%d",
             personality, feedback, animation);
}

static void menu_sleep_config_hook(int auto_sleep_s, int wake_mode, bool sleep_anim)
{
    panda_settings_t *cfg = panda_settings_get();
    cfg->auto_sleep_s = auto_sleep_s;
    cfg->wake_mode = wake_mode;
    cfg->sleep_anim = sleep_anim ? 1 : 0;
    panda_settings_save();
    ESP_LOGI(TAG, "menu sleep=%ds wake=%d anim=%d",
             auto_sleep_s, wake_mode, sleep_anim);
}

static void menu_sleep_now_hook(void)
{
    if (panda_ui_get_voice_state() != PANDA_VOICE_IDLE) {
        app_voice_cancel();
    }
    panda_ui_sleep();
    ESP_LOGI(TAG, "sleep requested from menu");
}

static void menu_wifi_hook(bool enable)
{
    panda_settings_t *cfg = panda_settings_get();
    if (!enable) {
        panda_connectivity_ap(false);
        panda_connectivity_disconnect();
        ESP_LOGI(TAG, "menu wifi off");
        return;
    }
    if (cfg->wifi_ssid[0]) {
        panda_connectivity_connect(cfg->wifi_ssid, cfg->wifi_pass);
        ESP_LOGI(TAG, "menu wifi connect '%s'", cfg->wifi_ssid);
    } else {
        panda_connectivity_ap(true);
        panda_ui_toast("无 WiFi 凭证，已开启配网", 1600);
        ESP_LOGI(TAG, "menu wifi on without credentials -> AP");
    }
}

static void menu_dnd_hook(bool enable)
{
    panda_settings_t *cfg = panda_settings_get();
    cfg->dnd = enable ? 1 : 0;
    panda_audio_mute(enable);
    panda_settings_save();
    ESP_LOGI(TAG, "menu dnd=%d", enable);
}

static bool read_sd_usage(uint64_t *free_kb, uint64_t *total_kb)
{
    *free_kb = 0;
    *total_kb = 0;
    if (!panda_board_sdcard_mounted()) {
        return false;
    }
    const char *mount = panda_board_sdcard_mount_point();
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (mount == NULL || esp_vfs_fat_info(mount, &total, &free_bytes) != ESP_OK) {
        return false;
    }
    *total_kb = total / 1024;
    *free_kb = free_bytes / 1024;
    return *total_kb > 0;
}

static void sync_menu_state(void)
{
    const panda_settings_t *cfg = panda_settings_get();
    panda_ui_menu_state_t state = {
        .volume = cfg->volume,
        .brightness = cfg->brightness,
        .wifi_connected = panda_connectivity_is_connected(),
        .wifi_connecting = panda_connectivity_is_connecting(),
        .wifi_ap = panda_connectivity_is_ap(),
        .personality = cfg->personality,
        .feedback = cfg->feedback,
        .animation = cfg->animation,
        .auto_sleep_s = cfg->auto_sleep_s,
        .wake_mode = cfg->wake_mode,
        .sleep_anim = cfg->sleep_anim != 0,
        .dnd = cfg->dnd != 0,
    };
    state.sd_mounted = read_sd_usage(&state.sd_free_kb, &state.sd_total_kb);
    panda_ui_menu_sync(&state);
}

void app_core_settings_changed(void)
{
    const panda_settings_t *cfg = panda_settings_get();
    panda_display_set_brightness(cfg->brightness);
    panda_audio_set_volume(cfg->volume);
    panda_audio_mute(cfg->dnd != 0);
    panda_ui_set_volume(cfg->volume, cfg->volume == 0);
    panda_ui_set_brightness(cfg->brightness);
    sync_menu_state();
}

static void boot_provision_toggle(void)
{
    if (panda_connectivity_is_ap()) {
        esp_err_t err = panda_connectivity_ap(false);
        panda_ui_toast("配网模式已关闭", 1600);
        ESP_LOGI(TAG, "boot long: provisioning off (%s)", esp_err_to_name(err));
        return;
    }
    esp_err_t err = panda_connectivity_ap(true);
    panda_ui_toast(err == ESP_OK ? "配网模式已开启" : "配网开启失败", 1600);
    ESP_LOGI(TAG, "boot long: provisioning on (%s)", esp_err_to_name(err));
}

void app_core_key_action(int button, bool is_long)
{
    switch (button) {
    case PANDA_BTN_BOOT:
        if (is_long) {
            boot_provision_toggle();
        } else {
            panda_ui_toast("长按 4 秒进入配网", 1200);
            ESP_LOGI(TAG, "boot short: hold 4 s for provisioning");
        }
        break;
    case PANDA_BTN_1:
        if (!is_long) {
            volume_step(-KEY_VOLUME_STEP);
        }
        break;
    case PANDA_BTN_2:
        if (!is_long) {
            volume_step(KEY_VOLUME_STEP);
        }
        break;
    case PANDA_BTN_3:
        if (is_long) {
            panda_ui_menu_home();
            panda_ui_toast("返回主屏", 800);
            ESP_LOGI(TAG, "menu page: home");
        } else {
            const char *page = panda_ui_menu_next();
            if (page != NULL) {
                panda_ui_toast(page, 800);
                ESP_LOGI(TAG, "menu page: %s", page);
            }
        }
        break;
    default:
        break;
    }
}

static void handle_touch(panda_touch_gesture_t g, int x, int y)
{
    touch_activity();
    ESP_LOGI(TAG, "touch action %s (%d,%d)", panda_touch_gesture_name(g), x, y);
    switch (g) {
    case PANDA_TOUCH_TAP: {
        char buf[64];
        int pct = panda_board_battery_percent();
        if (pct >= 0) {
            snprintf(buf, sizeof(buf), "%s · 电量%d%%",
                     panda_ui_voice_state_name(panda_ui_get_voice_state()), pct);
        } else {
            snprintf(buf, sizeof(buf), "%s · 电量未知",
                     panda_ui_voice_state_name(panda_ui_get_voice_state()));
        }
        panda_ui_toast(buf, 1500);
        break;
    }
    case PANDA_TOUCH_DOUBLE_TAP: {
        char buf[64];
        int pct = panda_board_battery_percent();
        snprintf(buf, sizeof(buf), "电量 %s%d%%  触摸 x%d y%d",
                 pct >= 0 ? "" : "未知 ", pct >= 0 ? pct : 0, x, y);
        panda_ui_toast(buf, 2000);
        break;
    }
    case PANDA_TOUCH_SWIPE_LEFT:
    case PANDA_TOUCH_SWIPE_RIGHT:
    case PANDA_TOUCH_SWIPE_UP:
    case PANDA_TOUCH_SWIPE_DOWN:
        /* Screen navigation is handled by the SquareLine UI. */
        break;
    case PANDA_TOUCH_LONG_PRESS:
        /* PTT is driven by polled press state in the task loop. */
        break;
    default:
        break;
    }
}

static void app_core_task(void *arg)
{
    (void)arg;
    int64_t next_time = 0;
    int64_t next_batt = 0;
    int64_t next_heap = 0;
    int64_t next_sync = 0;

    app_core_set_wifi_status(false, false, true);
    panda_ui_set_voice_state(PANDA_VOICE_CONNECTING);
    vTaskDelay(pdMS_TO_TICKS(600));
    panda_ui_set_device_state(PANDA_DEVICE_READY);
    app_core_settings_changed();
    touch_activity();
    app_voice_start();

    while (true) {
        panda_board_buttons_scan();
        int64_t now = esp_timer_get_time();

        for (int i = 0; i < 4; i++) {
            bool pressed = panda_board_button_pressed((panda_button_t)i);
            if (pressed && !s_btn_last[i]) {
                s_btn_down_us[i] = now;
                s_btn_long[i] = false;
                s_btn_wake[i] = wake_if_sleeping();
                touch_activity();
                ESP_LOGI(TAG, "button %d down", i);
            } else if (pressed && !s_btn_long[i] &&
                       (now - s_btn_down_us[i]) > (i == PANDA_BTN_BOOT ? BOOT_PROVISION_HOLD_US : 600000)) {
                s_btn_long[i] = true;
                ESP_LOGI(TAG, "button %d long", i);
                /* BOOT still works after waking the screen: the 4 s hold is deliberate. */
                if (!s_btn_wake[i] || i == PANDA_BTN_BOOT) {
                    app_core_key_action(i, true);
                }
            } else if (!pressed && s_btn_last[i]) {
                ESP_LOGI(TAG, "button %d up (%s)", i, s_btn_long[i] ? "long" : "short");
                if (!s_btn_long[i] && !s_btn_wake[i]) {
                    app_core_key_action(i, false);
                }
                s_btn_wake[i] = false;
            }
            s_btn_last[i] = pressed;
        }

        const panda_touch_state_t *ts = panda_touch_get_state();
        if (ts->pressed && !s_touch_last) {
            s_touch_down_us = now;
            s_touch_start_x = ts->x;
            s_touch_start_y = ts->y;
            s_touch_moved = false;
            s_touch_wake = wake_if_sleeping();
            touch_activity();
        } else if (ts->pressed) {
            int dx = (int)ts->x - (int)s_touch_start_x;
            int dy = (int)ts->y - (int)s_touch_start_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > 25 || dy > 25) {
                s_touch_moved = true;
            }
            if (!s_touch_moved && !s_ptt_source && !s_touch_wake &&
                (now - s_touch_down_us) > 600000) {
                ptt_press(2);
            }
        } else if (!ts->pressed && s_touch_last) {
            ptt_release(2);
            s_touch_wake = false;
        }
        s_touch_last = ts->pressed;

        app_event_t ev;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(20)) == pdPASS) {
            if (ev.type == EVT_TOUCH) {
                handle_touch(ev.u.touch.gesture, ev.u.touch.x, ev.u.touch.y);
            } else if (ev.type == EVT_WIFI) {
                bool was_connected = s_wifi_connected;
                bool was_ap = s_wifi_ap;
                s_wifi_connected = ev.u.wifi.connected;
                s_wifi_ap = ev.u.wifi.ap;
                s_wifi_connecting = ev.u.wifi.connecting;
                panda_ui_set_wifi(s_wifi_connected, s_wifi_ap, s_wifi_connecting);
#if CONFIG_PANDA_PROVISIONING
                if (s_wifi_ap != was_ap) {
                    if (s_wifi_ap) {
                        ESP_LOGI(TAG, "provisioning page enabled (SoftAP on)");
                        app_provisioning_start();
                    } else {
                        ESP_LOGI(TAG, "provisioning page disabled (SoftAP off)");
                        app_provisioning_stop();
                    }
                }
#endif
                if (was_connected != s_wifi_connected) {
                    app_voice_set_network(s_wifi_connected);
                }
            }
        }

        if (s_ui_pending_lock) {
            bool have_voice = false;
            bool have_subtitle = false;
            panda_voice_ui_t pending_voice = PANDA_VOICE_IDLE;
            char pending_subtitle[512] = {0};
            xSemaphoreTake(s_ui_pending_lock, portMAX_DELAY);
            if (s_voice_pending) {
                pending_voice = s_pending_voice;
                s_voice_pending = false;
                have_voice = true;
            }
            if (s_subtitle_pending) {
                memcpy(pending_subtitle, s_pending_subtitle, sizeof(pending_subtitle));
                s_subtitle_pending = false;
                have_subtitle = true;
            }
            xSemaphoreGive(s_ui_pending_lock);
            if (have_voice) {
                panda_ui_set_voice_state(pending_voice);
            }
            if (have_subtitle) {
                panda_ui_set_subtitle(pending_subtitle);
            }
        }

        if (now >= next_time) {
            next_time = now + 1000000;
            struct tm tm_now;
            time_t sys = time(NULL);
            bool valid = false;
            if (sys > 1700000000) {
                struct tm lt;
                localtime_r(&sys, &lt);
                tm_now = lt;
                valid = true;
            } else if (panda_board_rtc_read(&tm_now) == ESP_OK) {
                valid = true;
            }
            if (valid) {
                panda_ui_set_time(tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, true);
            } else {
                panda_ui_set_time(0, 0, 0, 0, 0, 0, false);
            }

            const panda_settings_t *cfg = panda_settings_get();
            panda_voice_ui_t voice = panda_ui_get_voice_state();
            bool voice_idle = voice == PANDA_VOICE_IDLE || voice == PANDA_VOICE_OFFLINE;
            if (cfg->auto_sleep_s > 0 && !panda_ui_is_sleeping() && voice_idle &&
                (now - s_last_active_us) >= (int64_t)cfg->auto_sleep_s * 1000000) {
                panda_ui_sleep();
                ESP_LOGI(TAG, "auto sleep after %ds idle", cfg->auto_sleep_s);
            }
        }

        if (now >= next_sync) {
            next_sync = now + 60000000;
            sync_menu_state();
        }

        if (now >= next_batt) {
            next_batt = now + 5000000;
            bool present = panda_board_get_info()->battery_adc_ok;
            uint32_t mv = panda_board_battery_mv();
            int pct = panda_board_battery_percent();
            bool charging = panda_board_battery_charging();
            ESP_LOGD(TAG, "battery present=%d mv=%" PRIu32 " pct=%d chg=%d", present, mv, pct, charging);
            panda_ui_set_battery(pct, charging, present);
            if (present && pct >= 0 && pct <= 10 && !s_low_batt_warned) {
                s_low_batt_warned = true;
                panda_ui_set_device_state(PANDA_DEVICE_LOW_BATTERY);
                panda_ui_toast("电量低，请充电", 3000);
            } else if (present && pct > 15) {
                s_low_batt_warned = false;
                if (panda_ui_get_voice_state() != PANDA_VOICE_ERROR) {
                    panda_ui_set_device_state(PANDA_DEVICE_READY);
                }
            }
        }

#if CONFIG_PANDA_DEBUG_VERBOSE
        if (now >= next_heap) {
            next_heap = now + 60000000;
            ESP_LOGI(TAG, "heap: free=%" PRIu32 " internal=%" PRIu32 " psram=%" PRIu32
                     " min_internal=%" PRIu32,
                     esp_get_free_heap_size(),
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
#endif
    }
}

esp_err_t app_core_start(void)
{
    s_queue = xQueueCreate(12, sizeof(app_event_t));
    s_ui_pending_lock = xSemaphoreCreateMutex();
    if (!s_queue || !s_ui_pending_lock) {
        return ESP_ERR_NO_MEM;
    }
    const panda_ui_menu_hooks_t hooks = {
        .volume = menu_volume_hook,
        .brightness = menu_brightness_hook,
        .personality = menu_personality_hook,
        .sleep_config = menu_sleep_config_hook,
        .sleep_now = menu_sleep_now_hook,
        .wifi = menu_wifi_hook,
        .dnd = menu_dnd_hook,
    };
    panda_ui_set_menu_hooks(&hooks);
    BaseType_t ok = xTaskCreatePinnedToCore(app_core_task, "app_core", 6 * 1024, NULL, 5, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_core_post_touch(panda_touch_gesture_t gesture, int x, int y)
{
    if (!s_queue) {
        return;
    }
    app_event_t ev = {
        .type = EVT_TOUCH,
        .u.touch = {.gesture = gesture, .x = x, .y = y},
    };
    xQueueSend(s_queue, &ev, 0);
}

void app_core_set_wifi_status(bool connected, bool ap_mode, bool connecting)
{
    if (!s_queue) {
        panda_ui_set_wifi(connected, ap_mode, connecting);
        return;
    }
    app_event_t ev = {
        .type = EVT_WIFI,
        .u.wifi = {.connected = connected, .ap = ap_mode, .connecting = connecting},
    };
    xQueueSend(s_queue, &ev, 0);
}

void app_core_set_time_from_epoch(time_t epoch)
{
    struct tm tm_local;
    localtime_r(&epoch, &tm_local);
    panda_board_rtc_write(&tm_local);
    panda_ui_set_time(tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                      tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec, true);
}

void app_core_on_voice_state(panda_voice_ui_t state)
{
    touch_activity();
    /* Status text, screen and overlays are derived by panda_state so every
     * input path (voice, touch, CLI, network) goes through one state machine. */
    if (!s_ui_pending_lock) {
        panda_ui_set_voice_state(state);
        return;
    }
    xSemaphoreTake(s_ui_pending_lock, portMAX_DELAY);
    s_pending_voice = state;
    s_voice_pending = true;
    xSemaphoreGive(s_ui_pending_lock);
}

void app_core_on_emotion(panda_emotion_t emotion)
{
    panda_ui_set_emotion(emotion);
}

void app_core_on_subtitle(const char *text)
{
    if (!s_ui_pending_lock) {
        panda_ui_set_subtitle(text);
        return;
    }
    xSemaphoreTake(s_ui_pending_lock, portMAX_DELAY);
    snprintf(s_pending_subtitle, sizeof(s_pending_subtitle), "%s", text ? text : "");
    s_subtitle_pending = true;
    xSemaphoreGive(s_ui_pending_lock);
}
