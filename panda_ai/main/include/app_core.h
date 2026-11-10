#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "panda_touch.h"
#include "panda_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_core_start(void);

void app_core_post_touch(panda_touch_gesture_t gesture, int x, int y);
/* Runs one KEY1/KEY2/KEY3 action; also used by the `key` CLI for testing. */
void app_core_key_action(int button, bool is_long);
void app_core_set_wifi_status(bool connected, bool ap_mode, bool connecting);
void app_core_set_time_from_epoch(time_t epoch);
/* Re-apply persisted settings to hardware and refresh the menu UI. Called
 * after CLI / provisioning changes settings outside the UI callbacks. */
void app_core_settings_changed(void);
void app_core_on_voice_state(panda_voice_ui_t state);
void app_core_on_emotion(panda_emotion_t emotion);
void app_core_on_subtitle(const char *text);

/* Implemented by app_voice once the voice pipeline is enabled. Weak stubs
 * live in app_core so the UI milestone can build standalone. */
esp_err_t app_voice_start(void);
esp_err_t app_voice_cancel(void);
esp_err_t app_voice_send_test_clip(void);
void app_voice_ptt_press(void);
void app_voice_ptt_release(void);
void app_voice_set_pump(bool enabled);
bool app_voice_is_active(void);
void app_voice_set_network(bool online);

typedef struct {
    bool connected;
    bool session;
    uint32_t deltas;
    uint32_t decode_failures;
    uint32_t ring_drops;
    uint32_t underruns;
    uint32_t write_retries;
    uint32_t rx_overflows;
    size_t ring_samples;
    uint32_t tx_frames;
} app_voice_stats_t;

esp_err_t app_voice_get_stats(app_voice_stats_t *stats);

esp_err_t app_audio_test_start(void);

#ifdef __cplusplus
}
#endif
