#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything here is best-effort: callers never block and never get an error
 * back, so the LED ring can not stall voice, keys or the UI. */
typedef enum {
    PANDA_LED_MODE_OFF = 0,  /* all dark */
    PANDA_LED_MODE_STATE,    /* device state colour only */
    PANDA_LED_MODE_AUDIO,    /* state + audio-reactive VU */
    PANDA_LED_MODE_FULL,     /* state + VU + key/wake/network events */
    PANDA_LED_MODE_MANUAL,   /* fixed colour, used by `rgb <r> <g> <b>` */
} panda_led_mode_t;

typedef enum {
    PANDA_LED_VOICE_IDLE = 0,
    PANDA_LED_VOICE_CONNECTING,
    PANDA_LED_VOICE_LISTENING,
    PANDA_LED_VOICE_THINKING,
    PANDA_LED_VOICE_SPEAKING,
    PANDA_LED_VOICE_OFFLINE,
    PANDA_LED_VOICE_ERROR,
} panda_led_voice_t;

typedef enum {
    PANDA_LED_PLAY_IDLE = 0,
    PANDA_LED_PLAY_PLAYING,
    PANDA_LED_PLAY_PAUSED,
} panda_led_play_t;

/* Starts the animation task on core 0 (audio owns core 1). Idempotent. */
esp_err_t panda_led_init(void);

void panda_led_set_mode(panda_led_mode_t mode);
panda_led_mode_t panda_led_get_mode(void);
void panda_led_set_brightness(int percent);
int panda_led_get_brightness(void);
void panda_led_set_manual(uint8_t r, uint8_t g, uint8_t b);

void panda_led_set_voice(panda_led_voice_t voice);
void panda_led_set_play(panda_led_play_t play, int track_index);
void panda_led_set_network(bool connected, bool ap);
void panda_led_set_battery(int percent, bool charging, bool present);
void panda_led_set_sleeping(bool sleeping);
/* Freeze the animation task and latch the single dim sleep indicator.
 * Call once, immediately before esp_deep_sleep_start(). */
esp_err_t panda_led_prepare_deep_sleep(void);

void panda_led_notify_volume(int direction, int volume_percent);
void panda_led_notify_wake(void);
void panda_led_notify_network_connected(void);

const char *panda_led_mode_name(panda_led_mode_t mode);
/* Diagnostics: how many RMT frames were actually pushed (unchanged frames and
 * idle/sleep ticks are skipped on purpose). */
uint32_t panda_led_refresh_count(void);

#ifdef __cplusplus
}
#endif
