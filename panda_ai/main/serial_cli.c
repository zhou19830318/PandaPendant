#include "serial_cli.h"

#include "sdkconfig.h"

#if CONFIG_PANDA_SERIAL_CLI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "app_core.h"
#include "app_settings.h"
#include "app_weather.h"
#include "panda_audio.h"
#include "panda_board.h"
#include "panda_connectivity.h"
#include "app_provisioning.h"
#include "panda_display.h"
#include "panda_touch.h"
#include "panda_led.h"
#include "panda_mp3.h"
#include "panda_ui.h"

static const char *TAG = "cli";

static const char *voice_names[] = {
    "idle", "connecting", "listening", "thinking", "speaking", "offline", "error",
};

static const char *emotion_names[] = {
    "neutral", "happy", "surprise", "sad", "sleepy", "curious", "shy", "angry",
};

static const char *error_names[] = {
    "asr", "llm", "tts", "audio", "network", "auth", "generic",
};

static int parse_int(const char *s, int min, int max, int *out)
{
    if (s == NULL || out == NULL) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || v < min || v > max) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static const char *join_args(int argc, char **argv, int start)
{
    static char buf[256];
    buf[0] = '\0';
    size_t used = 0;
    for (int i = start; i < argc; i++) {
        int n = snprintf(buf + used, sizeof(buf) - used, "%s%s",
                         used ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(buf) - used) {
            break;
        }
        used += (size_t)n;
    }
    return buf;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const panda_board_info_t *info = panda_board_get_info();
    const panda_touch_state_t *ts = panda_touch_get_state();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    printf("firmware : %s\n", "panda_ai");
    printf("uptime   : %" PRId64 " s\n", uptime_s);
    printf("voice    : %s\n", panda_ui_voice_state_name(panda_ui_get_voice_state()));
    printf("emotion  : %s\n", panda_ui_emotion_name(panda_ui_get_emotion()));
    if (info->battery_adc_ok) {
        printf("battery  : %" PRIu32 " mV, %d%%, %s\n", panda_board_battery_mv(),
               panda_board_battery_percent(),
               panda_board_battery_charging() ? "charging" : "discharging");
    } else {
        printf("battery  : unknown (ADC not wired)\n");
    }
    printf("display  : brightness=%d%%\n", panda_display_get_brightness());
    printf("audio    : ready=%d mic=%d volume=%d%%\n",
           panda_audio_ready(), panda_audio_has_mic(), panda_settings_get()->volume);
    printf("wifi     : %s ip=%s ssid=%s ap=%s\n",
           panda_connectivity_is_connected() ? "connected" :
           panda_connectivity_is_connecting() ? "connecting" :
           panda_connectivity_is_ap() ? "ap" : "idle",
           panda_connectivity_ip()[0] ? panda_connectivity_ip() : "-",
           panda_connectivity_ssid()[0] ? panda_connectivity_ssid() : "-",
           panda_connectivity_is_ap() ? panda_connectivity_ap_ssid() : "-");
    app_weather_info_t wx;
    if (app_weather_get_info(&wx)) {
        printf("weather  : %s %d°C %s code=%d\n", wx.city, wx.temp_c, wx.condition, wx.weather_code);
    } else {
        printf("weather  : unavailable (city=%s)\n", panda_settings_get()->weather_city);
    }
    printf("menu     : personality=%d feedback=%d animation=%d auto_sleep=%ds wake=%d anim=%d dnd=%d\n",
           panda_settings_get()->personality, panda_settings_get()->feedback,
           panda_settings_get()->animation, panda_settings_get()->auto_sleep_s,
           panda_settings_get()->wake_mode, panda_settings_get()->sleep_anim,
           panda_settings_get()->dnd);
    printf("touch    : x=%u y=%u taps=%" PRIu32 " gestures=%" PRIu32 "\n",
           ts->x, ts->y, ts->tap_count, ts->gesture_count);
    printf("board    : i2c=%s ioexp=%s rtc=%s sd=%s rgb=%s boot=%" PRIu32 "\n",
           info->i2c_ok ? "ok" : "fail", info->ioexp_ok ? "ok" : "fail",
           info->rtc_ok ? "ok" : "fail", info->sdcard_ok ? "mounted" : "none",
           info->rgb_ok ? "ok" : "fail", info->boot_count);
    uint32_t loops = 0;
    uint32_t max_gap = 0;
    uint32_t age = 0;
    app_core_loop_stats(&loops, &max_gap, &age);
    printf("app      : loops=%" PRIu32 " max_gap=%" PRIu32 "ms age=%" PRIu32 "ms\n",
           loops, max_gap, age);
    printf("heap     : free=%zu internal=%zu psram=%zu\n",
           (size_t)esp_get_free_heap_size(),
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_ui(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ui state|emotion|error|device|screen|images|sleep|wake|status|subtitle|toast|audio|volume ...\n");
        return 1;
    }

    if (!strcmp(argv[1], "screen")) {
        printf("screen: %s\n", panda_ui_current_screen());
        return 0;
    }
    if (!strcmp(argv[1], "images")) {
        panda_ui_image_report_t r;
        esp_err_t err = panda_ui_verify_images(&r);
        if (err == ESP_ERR_INVALID_STATE) {
            printf("UI not ready (display disabled)\n");
            return 1;
        }
        printf("images: screens=%d objects=%d unique=%d failures=%d (%s)\n",
               r.screens, r.image_objects, r.unique_images, r.failures,
               err == ESP_OK ? "ok" : "failed");
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "sleep")) {
        panda_ui_sleep();
        panda_led_set_sleeping(true);
        return 0;
    }
    if (!strcmp(argv[1], "wake")) {
        panda_ui_wake();
        panda_led_set_sleeping(false);
        panda_led_notify_wake();
        return 0;
    }

    if (!strcmp(argv[1], "state") && argc >= 3) {
        for (int i = 0; i < (int)(sizeof(voice_names) / sizeof(voice_names[0])); i++) {
            if (!strcmp(argv[2], voice_names[i])) {
                app_core_on_voice_state((panda_voice_ui_t)i);
                return 0;
            }
        }
        printf("unknown state '%s'\n", argv[2]);
        return 1;
    }
    if (!strcmp(argv[1], "emotion") && argc >= 3) {
        for (int i = 0; i < (int)(sizeof(emotion_names) / sizeof(emotion_names[0])); i++) {
            if (!strcmp(argv[2], emotion_names[i])) {
                app_core_on_emotion((panda_emotion_t)i);
                return 0;
            }
        }
        printf("unknown emotion '%s'\n", argv[2]);
        return 1;
    }
    if (!strcmp(argv[1], "error") && argc >= 3) {
        if (!strcmp(argv[2], "clear")) {
            panda_ui_clear_error();
            return 0;
        }
        for (int i = 0; i < (int)(sizeof(error_names) / sizeof(error_names[0])); i++) {
            if (!strcmp(argv[2], error_names[i])) {
                panda_ui_set_error((panda_ui_error_t)i);
                return 0;
            }
        }
        printf("unknown error '%s' (asr|llm|tts|audio|network|auth|generic|clear)\n", argv[2]);
        return 1;
    }
    if (!strcmp(argv[1], "device") && argc >= 3) {
        if (!strcmp(argv[2], "boot")) {
            panda_ui_set_device_state(PANDA_DEVICE_BOOT);
        } else if (!strcmp(argv[2], "ready")) {
            panda_ui_set_device_state(PANDA_DEVICE_READY);
        } else if (!strcmp(argv[2], "low")) {
            panda_ui_set_device_state(PANDA_DEVICE_LOW_BATTERY);
        } else if (!strcmp(argv[2], "charging")) {
            panda_ui_set_device_state(PANDA_DEVICE_CHARGING);
        } else {
            printf("unknown device state '%s'\n", argv[2]);
            return 1;
        }
        return 0;
    }
    if (!strcmp(argv[1], "status")) {
        panda_ui_set_status(join_args(argc, argv, 2));
        return 0;
    }
    if (!strcmp(argv[1], "subtitle")) {
        panda_ui_set_subtitle(join_args(argc, argv, 2));
        return 0;
    }
    if (!strcmp(argv[1], "toast") && argc >= 3) {
        panda_ui_toast(join_args(argc, argv, 2), 2000);
        return 0;
    }
    if (!strcmp(argv[1], "audio") && argc >= 3) {
        int level;
        if (parse_int(argv[2], 0, 100, &level)) {
            printf("level must be 0..100\n");
            return 1;
        }
        panda_ui_set_audio_level((uint8_t)level);
        return 0;
    }
    if (!strcmp(argv[1], "volume") && argc >= 3) {
        int pct;
        if (parse_int(argv[2], 0, 100, &pct)) {
            printf("volume must be 0..100\n");
            return 1;
        }
        bool muted = false;
        if (argc >= 4) {
            muted = !strcmp(argv[3], "mute") || !strcmp(argv[3], "muted");
        }
        panda_settings_get()->volume = pct;
        panda_settings_save();
        panda_audio_set_volume(pct);
        panda_ui_set_volume(pct, muted);
        return 0;
    }

    printf("usage: ui state|emotion|error|device|screen|images|sleep|wake|status|subtitle|toast|audio|volume ...\n");
    return 1;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: touch tap|double|long|left|right|up|down [x y] | touch diag\n");
        return 1;
    }
    if (!strcmp(argv[1], "diag")) {
        panda_touch_diag();
        return 0;
    }
    panda_touch_gesture_t g;
    if (!strcmp(argv[1], "tap")) {
        g = PANDA_TOUCH_TAP;
    } else if (!strcmp(argv[1], "double")) {
        g = PANDA_TOUCH_DOUBLE_TAP;
    } else if (!strcmp(argv[1], "long")) {
        g = PANDA_TOUCH_LONG_PRESS;
    } else if (!strcmp(argv[1], "left")) {
        g = PANDA_TOUCH_SWIPE_LEFT;
    } else if (!strcmp(argv[1], "right")) {
        g = PANDA_TOUCH_SWIPE_RIGHT;
    } else if (!strcmp(argv[1], "up")) {
        g = PANDA_TOUCH_SWIPE_UP;
    } else if (!strcmp(argv[1], "down")) {
        g = PANDA_TOUCH_SWIPE_DOWN;
    } else {
        printf("unknown gesture '%s'\n", argv[1]);
        return 1;
    }
    int x = 180;
    int y = 180;
    if (argc >= 4) {
        if (parse_int(argv[2], 0, 359, &x) || parse_int(argv[3], 0, 359, &y)) {
            printf("coordinates must be 0..359\n");
            return 1;
        }
    }
    app_core_post_touch(g, x, y);
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    int btn;
    if (argc < 2) {
        printf("usage: key 1|2|3|boot [long]\n");
        return 1;
    }
    if (!strcmp(argv[1], "boot")) {
        bool is_long = argc >= 3 && !strcmp(argv[2], "long");
        app_core_key_action(PANDA_BTN_BOOT, is_long);
        printf("key boot %s\n", is_long ? "long" : "short");
        return 0;
    }
    if (parse_int(argv[1], 1, 3, &btn)) {
        printf("usage: key 1|2|3|boot [long]\n");
        return 1;
    }
    bool is_long = argc >= 3 && !strcmp(argv[2], "long");
    app_core_key_action(btn, is_long);
    printf("key %d %s\n", btn, is_long ? "long" : "short");
    return 0;
}

static int cmd_brightness(int argc, char **argv)
{
    if (argc < 2) {
        printf("brightness=%d%%\n", panda_display_get_brightness());
        return 0;
    }
    int pct;
    if (parse_int(argv[1], 0, 100, &pct)) {
        printf("brightness must be 0..100\n");
        return 1;
    }
    esp_err_t err = panda_display_set_brightness(pct);
    if (err != ESP_OK) {
        printf("set brightness failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    panda_settings_get()->brightness = panda_display_get_brightness();
    panda_settings_save();
    app_core_settings_changed();
    printf("brightness=%d%%\n", panda_display_get_brightness());
    return 0;
}

static int cmd_volume(int argc, char **argv)
{
    if (argc < 2) {
        printf("volume=%d%%\n", panda_settings_get()->volume);
        return 0;
    }
    int pct;
    if (parse_int(argv[1], 0, 100, &pct)) {
        printf("volume must be 0..100\n");
        return 1;
    }
    panda_settings_get()->volume = pct;
    panda_settings_save();
    app_core_settings_changed();
    printf("volume=%d%%\n", pct);
    return 0;
}

static int cmd_weather(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "refresh")) {
        app_weather_refresh();
        printf("weather refresh requested\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        panda_settings_t *cfg = panda_settings_get();
        const char *city = join_args(argc, argv, 2);
        snprintf(cfg->weather_city, sizeof(cfg->weather_city), "%s", city);
        esp_err_t err = panda_settings_save();
        app_core_settings_changed();
        app_weather_refresh();
        printf("weather_city: %s (%s)\n", cfg->weather_city, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    app_weather_info_t info;
    if (!app_weather_get_info(&info)) {
        printf("weather: no data yet (city=%s)\n", panda_settings_get()->weather_city);
        return 0;
    }
    printf("weather: %s %d°C %s code=%d icon=%d updated=%lld\n",
           info.city, info.temp_c, info.condition, info.weather_code, (int)info.icon,
           (long long)info.updated);
    return 0;
}

static int cmd_battery(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const panda_board_info_t *info = panda_board_get_info();
    printf("adc=%s mv=%" PRIu32 " percent=%d charging=%d\n",
           info->battery_adc_ok ? "enabled" : "disabled",
           panda_board_battery_mv(), panda_board_battery_percent(),
           panda_board_battery_charging());
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "sync")) {
        esp_err_t err = panda_connectivity_sync_time();
        printf("sntp: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        char *end = NULL;
        long long epoch = strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || epoch < 1700000000LL) {
            printf("epoch must be >= 1700000000\n");
            return 1;
        }
        struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
        settimeofday(&tv, NULL);
        app_core_set_time_from_epoch((time_t)epoch);
        printf("system time set to %lld\n", epoch);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "get")) {
        printf("usage: time [get] | time set <epoch> | time sync\n");
        return 1;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    bool valid = now > 1700000000;
    printf("epoch=%lld %04d-%02d-%02d %02d:%02d:%02d (%s)\n",
           (long long)now, tm_now.tm_year + 1900, tm_now.tm_mon + 1,
           tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
           valid ? "valid" : "not synced");
    struct tm rtc;
    if (panda_board_rtc_read(&rtc) == ESP_OK) {
        printf("rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
               rtc.tm_year + 1900, rtc.tm_mon + 1, rtc.tm_mday,
               rtc.tm_hour, rtc.tm_min, rtc.tm_sec);
    } else {
        printf("rtc=unavailable\n");
    }
    return 0;
}

static void set_string(char *dst, size_t size, const char *value)
{
    snprintf(dst, size, "%s", value ? value : "");
}

static int cmd_settings(int argc, char **argv)
{
    panda_settings_t *cfg = panda_settings_get();
    if (argc < 2) {
        panda_settings_print(false);
        return 0;
    }
    if (!strcmp(argv[1], "show")) {
        panda_settings_print(false);
        return 0;
    }
    if (!strcmp(argv[1], "save")) {
        esp_err_t err = panda_settings_save();
        printf("save: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "reset")) {
        esp_err_t err = panda_settings_reset();
        app_core_settings_changed();
        app_weather_refresh();
        printf("reset: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "set") || argc < 4) {
        printf("usage: settings show|save|reset|set <key> <value>\n");
        return 1;
    }

    const char *key = argv[2];
    const char *value = join_args(argc, argv, 3);
    int iv;
    if (!strcmp(key, "wifi_ssid")) {
        if (panda_settings_wifi_put(value, "") != ESP_OK) {
            printf("wifi list full (max %d)\n", PANDA_WIFI_CRED_MAX);
            return 1;
        }
    } else if (!strcmp(key, "wifi_pass")) {
        if (cfg->wifi_count == 0) {
            printf("add a network first: settings set wifi_ssid <name>\n");
            return 1;
        }
        /* Applies to the most recently added/edited network. */
        snprintf(cfg->wifi[cfg->wifi_count - 1].pass,
                 sizeof(cfg->wifi[0].pass), "%s", value);
    } else if (!strcmp(key, "wifi_clear")) {
        panda_settings_wifi_clear();
    } else if (!strcmp(key, "api_mode")) {
        set_string(cfg->api_mode, sizeof(cfg->api_mode), value);
    } else if (!strcmp(key, "app_id")) {
        set_string(cfg->app_id, sizeof(cfg->app_id), value);
    } else if (!strcmp(key, "access_key")) {
        set_string(cfg->access_key, sizeof(cfg->access_key), value);
    } else if (!strcmp(key, "resource_id")) {
        set_string(cfg->resource_id, sizeof(cfg->resource_id), value);
    } else if (!strcmp(key, "app_key")) {
        set_string(cfg->app_key, sizeof(cfg->app_key), value);
    } else if (!strcmp(key, "api_key")) {
        set_string(cfg->api_key, sizeof(cfg->api_key), value);
    } else if (!strcmp(key, "model")) {
        set_string(cfg->model, sizeof(cfg->model), value);
    } else if (!strcmp(key, "speaker")) {
        set_string(cfg->speaker, sizeof(cfg->speaker), value);
    } else if (!strcmp(key, "persona")) {
        set_string(cfg->persona, sizeof(cfg->persona), value);
    } else if (!strcmp(key, "volume")) {
        if (parse_int(value, 0, 100, &iv)) {
            printf("volume must be 0..100\n");
            return 1;
        }
        cfg->volume = iv;
        panda_audio_set_volume(cfg->volume);
        panda_ui_set_volume(cfg->volume, cfg->volume == 0);
    } else if (!strcmp(key, "brightness")) {
        if (parse_int(value, 0, 100, &iv)) {
            printf("brightness must be 0..100\n");
            return 1;
        }
        cfg->brightness = iv;
        panda_display_set_brightness(cfg->brightness);
    } else if (!strcmp(key, "auto_continue")) {
        if (parse_int(value, 0, 1, &iv)) {
            printf("auto_continue must be 0 or 1\n");
            return 1;
        }
        cfg->auto_continue = iv;
    } else if (!strcmp(key, "silence_timeout_ms")) {
        if (parse_int(value, 200, 10000, &iv)) {
            printf("silence_timeout_ms must be 200..10000\n");
            return 1;
        }
        cfg->silence_timeout_ms = iv;
    } else if (!strcmp(key, "max_record_seconds")) {
        if (parse_int(value, 1, 600, &iv)) {
            printf("max_record_seconds must be 1..600\n");
            return 1;
        }
        cfg->max_record_seconds = iv;
    } else if (!strcmp(key, "personality")) {
        if (parse_int(value, 0, 2, &iv)) {
            printf("personality must be 0(lively) 1(gentle) 2(quiet)\n");
            return 1;
        }
        cfg->personality = iv;
    } else if (!strcmp(key, "feedback")) {
        if (parse_int(value, 0, 2, &iv)) {
            printf("feedback must be 0(weak) 1(medium) 2(strong)\n");
            return 1;
        }
        cfg->feedback = iv;
    } else if (!strcmp(key, "animation")) {
        if (parse_int(value, 0, 2, &iv)) {
            printf("animation must be 0(low) 1(medium) 2(high)\n");
            return 1;
        }
        cfg->animation = iv;
    } else if (!strcmp(key, "auto_sleep_s")) {
        if (parse_int(value, 0, 600, &iv) || (iv != 0 && iv != 60 && iv != 300 && iv != 600)) {
            printf("auto_sleep_s must be 0(never) 60 300 600\n");
            return 1;
        }
        cfg->auto_sleep_s = iv;
    } else if (!strcmp(key, "wake_mode")) {
        if (parse_int(value, 0, 2, &iv)) {
            printf("wake_mode must be 0(touch) 1(voice) 2(both)\n");
            return 1;
        }
        cfg->wake_mode = iv;
    } else if (!strcmp(key, "sleep_anim")) {
        if (parse_int(value, 0, 1, &iv)) {
            printf("sleep_anim must be 0 or 1\n");
            return 1;
        }
        cfg->sleep_anim = iv;
    } else if (!strcmp(key, "dnd")) {
        if (parse_int(value, 0, 1, &iv)) {
            printf("dnd must be 0 or 1\n");
            return 1;
        }
        cfg->dnd = iv;
    } else if (!strcmp(key, "weather_city")) {
        set_string(cfg->weather_city, sizeof(cfg->weather_city), value);
    } else {
        printf("unknown key '%s'\n", key);
        return 1;
    }

    esp_err_t err = panda_settings_save();
    if (err == ESP_OK) {
        app_core_settings_changed();
        if (!strcmp(key, "weather_city")) {
            app_weather_refresh();
        }
        if (!strcmp(key, "wifi_ssid") || !strcmp(key, "wifi_pass") ||
            !strcmp(key, "wifi_clear")) {
            panda_connectivity_set_credentials(cfg->wifi, cfg->wifi_count);
        }
    }
    printf("set %s: %s\n", key, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_voice(int argc, char **argv)
{
    if (argc < 2) {
        printf("active=%d\n", app_voice_is_active());
        return 0;
    }
    if (!strcmp(argv[1], "start")) {
        esp_err_t err = app_voice_start();
        printf("voice start: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "active")) {
        printf("active=%d\n", app_voice_is_active());
        return 0;
    }
    if (!strcmp(argv[1], "ptt") && argc >= 3) {
        if (!strcmp(argv[2], "down")) {
            app_voice_ptt_press();
            return 0;
        }
        if (!strcmp(argv[2], "up")) {
            app_voice_ptt_release();
            return 0;
        }
    }
    if (!strcmp(argv[1], "clip")) {
        esp_err_t err = app_voice_send_test_clip();
        printf("voice clip: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "pump") && argc >= 3) {
        bool on = strcmp(argv[2], "off") != 0;
        app_voice_set_pump(on);
        printf("voice pump %s\n", on ? "on" : "off");
        return 0;
    }
    if (!strcmp(argv[1], "cancel")) {
        esp_err_t err = app_voice_cancel();
        printf("voice cancel: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "stats")) {
        app_voice_stats_t st;
        esp_err_t err = app_voice_get_stats(&st);
        if (err != ESP_OK) {
            printf("voice stats: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("ws=%d session=%d ring=%u deltas=%u decode_fail=%u ring_drops=%u "
               "underruns=%u write_retries=%u rx_overflow=%u tx_frames=%u\n",
               st.connected, st.session, (unsigned)st.ring_samples, (unsigned)st.deltas,
               (unsigned)st.decode_failures, (unsigned)st.ring_drops,
               (unsigned)st.underruns, (unsigned)st.write_retries,
               (unsigned)st.rx_overflows, (unsigned)st.tx_frames);
        return 0;
    }
    printf("usage: voice [start|active|ptt down|ptt up|clip|pump on|off|cancel|stats]\n");
    return 1;
}

static int cmd_audio(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "test")) {
        esp_err_t err = panda_audio_self_test();
        printf("audio test: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "status")) {
        int vol = -1;
        int mute_reg = -1;
        int dac_reg = -1;
        panda_audio_get_volume(&vol);
        panda_audio_get_codec_reg(0x31, &mute_reg);
        panda_audio_get_codec_reg(0x32, &dac_reg);
        printf("ready=%d mic=%d rate=%u volume=%d mute_reg=0x%02x dac_reg=0x%02x\n",
               panda_audio_ready(), panda_audio_has_mic(),
               (unsigned)panda_audio_get_rate(), vol,
               mute_reg < 0 ? 0xFF : mute_reg, dac_reg < 0 ? 0xFF : dac_reg);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "tone")) {
        int secs = 5;
        if (argc >= 3 && parse_int(argv[2], 1, 30, &secs)) {
            printf("seconds must be 1..30\n");
            return 1;
        }
        const size_t n = 1600; /* 100 ms at 16 kHz */
        int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_INTERNAL);
        if (!buf) {
            printf("tone: no memory\n");
            return 1;
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = (int16_t)(9830.0f * sinf(6.2831853f * 880.0f * i / 16000.0f));
        }
        int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
        size_t written = 0;
        while (esp_timer_get_time() < end) {
            if (panda_audio_play(buf, n, &written) != ESP_OK) {
                break;
            }
        }
        heap_caps_free(buf);
        printf("audio tone: %d s\n", secs);
        return 0;
    }
    printf("usage: audio test | audio status | audio tone [seconds]\n");
    return 1;
}

static int cmd_rgb(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: rgb status | rgb mode off|state|audio|full|manual | "
               "rgb brightness [0..100] | rgb <r> <g> <b> | rgb off\n");
        return 1;
    }
    if (!strcmp(argv[1], "off")) {
        panda_led_set_mode(PANDA_LED_MODE_OFF);
        printf("rgb mode off\n");
        return 0;
    }
    if (!strcmp(argv[1], "status")) {
        printf("rgb mode=%s brightness=%d%% level=%d refresh=%" PRIu32 "\n",
               panda_led_mode_name(panda_led_get_mode()),
               panda_led_get_brightness(), panda_audio_get_output_level(),
               panda_led_refresh_count());
        return 0;
    }
    if (!strcmp(argv[1], "mode")) {
        if (argc < 3) {
            printf("usage: rgb mode off|state|audio|full|manual\n");
            return 1;
        }
        static const char *names[] = {"off", "state", "audio", "full", "manual"};
        for (int i = 0; i <= PANDA_LED_MODE_MANUAL; i++) {
            if (!strcmp(argv[2], names[i])) {
                panda_led_set_mode((panda_led_mode_t)i);
                printf("rgb mode %s\n", names[i]);
                return 0;
            }
        }
        printf("unknown mode '%s'\n", argv[2]);
        return 1;
    }
    if (!strcmp(argv[1], "brightness")) {
        int pct;
        if (argc < 3) {
            printf("rgb brightness=%d%%\n", panda_led_get_brightness());
            return 0;
        }
        if (parse_int(argv[2], 0, 100, &pct)) {
            printf("brightness must be 0..100\n");
            return 1;
        }
        panda_led_set_brightness(pct);
        printf("rgb brightness=%d%%\n", panda_led_get_brightness());
        return 0;
    }
    if (argc >= 4) {
        int r, g, b;
        if (parse_int(argv[1], 0, 255, &r) || parse_int(argv[2], 0, 255, &g) ||
            parse_int(argv[3], 0, 255, &b)) {
            printf("rgb values must be 0..255\n");
            return 1;
        }
        panda_led_set_manual((uint8_t)r, (uint8_t)g, (uint8_t)b);
        printf("rgb manual %d %d %d\n", r, g, b);
        return 0;
    }
    printf("usage: rgb status | rgb mode ... | rgb brightness [0..100] | "
           "rgb <r> <g> <b> | rgb off\n");
    return 1;
}

static int cmd_i2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = panda_board_i2c_scan();
    printf("i2c scan: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_sdcard(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "status")) {
        printf("sdcard=%s\n", panda_board_sdcard_mounted() ? "mounted" : "not mounted");
        return 0;
    }
    if (!strcmp(argv[1], "mount")) {
        esp_err_t err = panda_board_sdcard_mount();
        printf("mount: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "unmount")) {
        esp_err_t err = panda_board_sdcard_unmount();
        printf("unmount: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "ls")) {
        const char *path = argc >= 3 ? argv[2] : panda_board_sdcard_mount_point();
        DIR *dir = opendir(path);
        if (dir == NULL) {
            printf("opendir %s failed\n", path);
            return 1;
        }
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char full[512];
            struct stat st;
            snprintf(full, sizeof(full), "%.200s/%s", path, ent->d_name);
            bool have = stat(full, &st) == 0;
            bool is_dir = have && S_ISDIR(st.st_mode);
            if (is_dir) {
                printf("DIR  %s\n", ent->d_name);
            } else {
                printf("FILE %10ld  %s\n", have ? (long)st.st_size : -1L, ent->d_name);
            }
            count++;
        }
        closedir(dir);
        printf("%d entries in %s\n", count, path);
        return 0;
    }
    printf("usage: sdcard status|ls [path]|mount|unmount\n");
    return 1;
}

static int cmd_rtc(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "write")) {
        char *end = NULL;
        long long epoch = strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            printf("invalid epoch\n");
            return 1;
        }
        struct tm tm_local;
        time_t t = (time_t)epoch;
        localtime_r(&t, &tm_local);
        esp_err_t err = panda_board_rtc_write(&tm_local);
        printf("rtc write: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    struct tm tm_now;
    if (panda_board_rtc_read(&tm_now) != ESP_OK) {
        printf("rtc unavailable\n");
        return 1;
    }
    printf("rtc=%04d-%02d-%02d %02d:%02d:%02d\n",
           tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
           tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "status")) {
        int rssi = 0;
        printf("state    : %s\n",
               panda_connectivity_is_connected() ? "connected" :
               panda_connectivity_is_connecting() ? "connecting" :
               panda_connectivity_is_ap() ? "ap" : "idle");
        printf("ssid     : %s\n", panda_connectivity_ssid()[0] ? panda_connectivity_ssid() : "-");
        printf("ip       : %s\n", panda_connectivity_ip()[0] ? panda_connectivity_ip() : "-");
        printf("softap   : %s (%s, ip=%s)\n",
               panda_connectivity_is_ap() ? "on" : "off", panda_connectivity_ap_ssid(),
               panda_connectivity_ap_ip()[0] ? panda_connectivity_ap_ip() : "-");
        printf("provision: %s http://%s/\n",
               app_provisioning_is_running() ? "on" : "off",
               panda_connectivity_ap_ip()[0] ? panda_connectivity_ap_ip() : "192.168.4.1");
        printf("ap_auto  : %s\n", panda_connectivity_ap_auto_started() ? "yes" : "no");
        printf("saved    : %d network(s)\n", panda_settings_wifi_count());
        for (int i = 0; i < panda_settings_wifi_count(); i++) {
            const panda_wifi_cred_t *cred = panda_settings_wifi_at(i);
            printf("  [%d]     : %s\n", i, cred ? cred->ssid : "-");
        }
        printf("reason   : %u (%s)\n", (unsigned)panda_connectivity_disconnect_reason(),
               panda_connectivity_disconnect_hint());
        printf("retry    : %u ms\n", (unsigned)panda_connectivity_retry_delay_ms());
        if (panda_connectivity_get_rssi(&rssi) == ESP_OK) {
            printf("rssi     : %d dBm\n", rssi);
        }
        return 0;
    }
    if (!strcmp(argv[1], "scan")) {
        esp_err_t err = panda_connectivity_scan();
        if (err != ESP_OK) {
            printf("scan failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }
    if (!strcmp(argv[1], "connect")) {
        if (argc < 3) {
            printf("usage: wifi connect <ssid> [password]\n");
            return 1;
        }
        const char *ssid = argv[2];
        const char *pass = argc >= 4 ? argv[3] : "";
        panda_settings_t *cfg = panda_settings_get();
        esp_err_t err = panda_settings_wifi_put(ssid, pass);
        if (err != ESP_OK) {
            printf("save failed: %s (max %d networks)\n", esp_err_to_name(err),
                   PANDA_WIFI_CRED_MAX);
            return 1;
        }
        err = panda_settings_save();
        if (err != ESP_OK) {
            printf("save failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        err = panda_connectivity_set_credentials(cfg->wifi, cfg->wifi_count);
        printf("connect '%s': %s (use 'wifi status')\n", ssid, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "disconnect")) {
        esp_err_t err = panda_connectivity_disconnect();
        printf("disconnect: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "forget")) {
        panda_settings_t *cfg = panda_settings_get();
        panda_settings_wifi_clear();
        esp_err_t err = panda_settings_save();
        if (err == ESP_OK) {
            err = panda_connectivity_set_credentials(cfg->wifi, 0);
        }
        printf("forget: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "ap")) {
        if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
            printf("usage: wifi ap on|off\n");
            return 1;
        }
        esp_err_t err = panda_connectivity_ap(!strcmp(argv[2], "on"));
        printf("softap %s: %s\n", argv[2], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "sync")) {
        esp_err_t err = panda_connectivity_sync_time();
        printf("sntp: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "recover")) {
        esp_err_t err = panda_connectivity_recover("cli");
        printf("recover: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: wifi status|scan|connect <ssid> [pass]|disconnect|forget|ap on|off|sync|recover\n");
    return 1;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("free=%zu internal=%zu psram=%zu min_internal=%zu largest_internal=%zu\n",
           (size_t)esp_get_free_heap_size(),
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return 0;
}

static int cmd_mp3(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "status")) {
        printf("tracks=%d dir=%s state=%s current=%d title=%s audio_rate=%u underruns=%u buffered=%u flags=0x%x\n",
               panda_mp3_count(), panda_mp3_dir(),
               panda_mp3_state() == PANDA_MP3_PLAYING ? "playing" :
               panda_mp3_state() == PANDA_MP3_PAUSED ? "paused" : "idle",
               panda_mp3_current(), panda_mp3_current_title(),
               (unsigned)panda_audio_get_rate(), (unsigned)panda_mp3_underruns(),
               (unsigned)panda_mp3_buffered(), (unsigned)panda_mp3_flags());
        return 0;
    }
    if (!strcmp(argv[1], "list")) {
        int count = panda_mp3_scan();
        printf("found %d track(s) in %s\n", count, panda_mp3_dir());
        for (int i = 0; i < count; i++) {
            printf("%2d. %s\n", i + 1, panda_mp3_title(i));
        }
        return count > 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "play")) {
        int index = argc >= 3 ? atoi(argv[2]) - 1 : -1;
        esp_err_t err = panda_mp3_play(index);
        printf("mp3 play: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "toggle")) {
        esp_err_t err = panda_mp3_toggle();
        printf("mp3 toggle: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "next") || !strcmp(argv[1], "prev")) {
        esp_err_t err = !strcmp(argv[1], "next") ? panda_mp3_next() : panda_mp3_prev();
        printf("mp3 %s: %s\n", argv[1], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "stop")) {
        esp_err_t err = panda_mp3_stop();
        printf("mp3 stop: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: mp3 status|list|play [index]|toggle|next|prev|stop\n");
    return 1;
}

static const esp_console_cmd_t s_commands[] = {
    {.command = "status", .help = "Show firmware, UI, battery and heap status", .func = cmd_status},
    {.command = "ui", .help = "Control UI: ui state|emotion|error|device|screen|images|sleep|wake|status|subtitle|toast|audio|volume", .func = cmd_ui},
    {.command = "touch", .help = "Inject gesture or probe panel: touch tap|double|long|left|right|up|down|diag", .func = cmd_touch},
    {.command = "key", .help = "Inject keys: key 1|2|3|boot [long]", .func = cmd_key},
    {.command = "brightness", .help = "Get/set display brightness 0..100", .func = cmd_brightness},
    {.command = "volume", .help = "Get/set speaker volume 0..100", .func = cmd_volume},
    {.command = "battery", .help = "Show battery voltage and percentage", .func = cmd_battery},
    {.command = "time", .help = "Show/set/sync time: time set <epoch> | time sync", .func = cmd_time},
    {.command = "settings", .help = "Show/save/reset/set NVS settings (secrets masked)", .func = cmd_settings},
    {.command = "weather", .help = "Weather: show | refresh | set <city>", .func = cmd_weather},
    {.command = "voice", .help = "Voice pipeline test: start, active, ptt down/up, clip, pump on/off, cancel", .func = cmd_voice},
    {.command = "audio", .help = "Audio self-test: audio test", .func = cmd_audio},
    {.command = "wifi", .help = "WiFi: status|scan|connect <ssid> [pass]|disconnect|forget|ap on/off|sync|recover", .func = cmd_wifi},
    {.command = "rgb", .help = "RGB LEDs: rgb status|mode|brightness | rgb <r> <g> <b> | rgb off", .func = cmd_rgb},
    {.command = "i2c", .help = "Scan the board I2C bus", .func = cmd_i2c},
    {.command = "sdcard", .help = "SD card: status|ls [path]|mount|unmount", .func = cmd_sdcard},
    {.command = "rtc", .help = "Read RTC or write: rtc write <epoch>", .func = cmd_rtc},
    {.command = "mp3", .help = "MP3: status|list|play [index]|toggle|next|prev|stop", .func = cmd_mp3},
    {.command = "reboot", .help = "Restart the device", .func = cmd_reboot},
    {.command = "heap", .help = "Show heap usage", .func = cmd_heap},
};

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "panda>";
    repl_config.max_history_len = 32;
    repl_config.task_stack_size = 6144;
    repl_config.task_priority = 3;

    esp_err_t err = ESP_OK;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&dev_config, &repl_config, &repl);
#else
    ESP_LOGW(TAG, "serial CLI disabled: no console device");
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        err = esp_console_cmd_register(&s_commands[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register '%s' failed: %s", s_commands[i].command,
                     esp_err_to_name(err));
            return err;
        }
    }

    err = esp_console_start_repl(repl);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "serial CLI ready (type 'help')");
    }
    return err;
}

#endif /* CONFIG_PANDA_SERIAL_CLI */
