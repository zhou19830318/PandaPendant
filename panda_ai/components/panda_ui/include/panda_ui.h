#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANDA_VOICE_IDLE = 0,
    PANDA_VOICE_CONNECTING,
    PANDA_VOICE_LISTENING,
    PANDA_VOICE_THINKING,
    PANDA_VOICE_SPEAKING,
    PANDA_VOICE_OFFLINE,
    PANDA_VOICE_ERROR,
} panda_voice_ui_t;

typedef enum {
    PANDA_EMOTION_NEUTRAL = 0,
    PANDA_EMOTION_HAPPY,
    PANDA_EMOTION_SURPRISE,
    PANDA_EMOTION_SAD,
    PANDA_EMOTION_SLEEPY,
    PANDA_EMOTION_CURIOUS,
    PANDA_EMOTION_SHY,
    PANDA_EMOTION_ANGRY,
} panda_emotion_t;

typedef enum {
    PANDA_DEVICE_BOOT = 0,
    PANDA_DEVICE_READY,
    PANDA_DEVICE_LOW_BATTERY,
    PANDA_DEVICE_CHARGING,
} panda_device_ui_t;

typedef enum {
    PANDA_UI_ERROR_ASR = 0,
    PANDA_UI_ERROR_LLM,
    PANDA_UI_ERROR_TTS,
    PANDA_UI_ERROR_AUDIO,
    PANDA_UI_ERROR_NETWORK,
    PANDA_UI_ERROR_AUTH,
    PANDA_UI_ERROR_GENERIC,
} panda_ui_error_t;

typedef struct {
    int screens;
    int image_objects;
    int unique_images;
    int failures;
} panda_ui_image_report_t;

typedef enum {
    PANDA_UI_WEATHER_SUN = 0,
    PANDA_UI_WEATHER_PARTLY,
    PANDA_UI_WEATHER_CLOUDY,
} panda_ui_weather_t;

/* Registered by app_core; panda_ui never touches settings/NVS itself. */
typedef struct {
    void (*volume)(int percent, bool commit);
    void (*brightness)(int percent, bool commit);
    void (*personality)(int personality, int feedback, int animation);
    void (*sleep_config)(int auto_sleep_s, int wake_mode, bool sleep_anim);
    void (*sleep_now)(void);
    void (*wifi)(bool enable);
    void (*dnd)(bool enable);
} panda_ui_menu_hooks_t;

typedef struct {
    int volume;
    int brightness;
    bool wifi_connected;
    bool wifi_connecting;
    bool wifi_ap;
    int personality;
    int feedback;
    int animation;
    int auto_sleep_s;
    int wake_mode;
    bool sleep_anim;
    bool dnd;
    bool sd_mounted;
    uint64_t sd_free_kb;
    uint64_t sd_total_kb;
} panda_ui_menu_state_t;

esp_err_t panda_ui_init(void);

/* Walk every generated screen and verify each image object's source decodes.
 * Safe to call from any task while the UI is running; used by `ui images`. */
esp_err_t panda_ui_verify_images(panda_ui_image_report_t *report);

void panda_ui_set_voice_state(panda_voice_ui_t state);
void panda_ui_set_emotion(panda_emotion_t emotion);
void panda_ui_set_device_state(panda_device_ui_t state);
void panda_ui_set_error(panda_ui_error_t error);
void panda_ui_clear_error(void);
void panda_ui_sleep(void);
void panda_ui_wake(void);
void panda_ui_set_audio_level(uint8_t level_0_100);

void panda_ui_set_wifi(bool connected, bool ap_mode, bool connecting);
void panda_ui_set_battery(int percent, bool charging, bool present);
void panda_ui_set_brightness(int percent);
void panda_ui_set_time(int year, int month, int day, int hour, int minute, int second, bool valid);
void panda_ui_set_volume(int percent, bool muted);
void panda_ui_set_weather(bool valid, int temp_c, panda_ui_weather_t icon);

void panda_ui_set_menu_hooks(const panda_ui_menu_hooks_t *hooks);
void panda_ui_menu_sync(const panda_ui_menu_state_t *state);
bool panda_ui_is_sleeping(void);
bool panda_ui_is_home_screen(void);

/* Hardware-key menu: advance through the generated setting pages and return
 * the page title (for a toast), or NULL when the UI is not ready. */
const char *panda_ui_menu_next(void);
void panda_ui_menu_home(void);

/* Provisioning QR overlay: joins the SoftAP in one scan. */
void panda_ui_show_provisioning_qr(const char *ap_ssid, const char *ap_pass, const char *url);
void panda_ui_hide_provisioning_qr(void);
bool panda_ui_provisioning_qr_visible(void);

/* True while the active LVGL screen is the MP3 player page. */
bool panda_ui_is_music_page(void);
/* Music page content: title plus playback state. */
void panda_ui_set_music(const char *title, bool playing, int index, int count);
/* Registers the touch controls on the music page (prev/play/next/exit). */
void panda_ui_set_music_hooks(void (*prev)(void), void (*toggle)(void),
                              void (*next)(void), void (*exit_page)(void));

void panda_ui_set_status(const char *status);
void panda_ui_set_subtitle(const char *text);

/* Called from the UI timer for simple "toast" text. */
void panda_ui_toast(const char *text, uint32_t duration_ms);

panda_voice_ui_t panda_ui_get_voice_state(void);
panda_emotion_t panda_ui_get_emotion(void);
const char *panda_ui_current_screen(void);
const char *panda_ui_voice_state_name(panda_voice_ui_t state);
const char *panda_ui_emotion_name(panda_emotion_t emotion);

#ifdef __cplusplus
}
#endif
