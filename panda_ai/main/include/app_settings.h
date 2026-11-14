#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "panda_connectivity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANDA_SSID_MAX 33
#define PANDA_PASS_MAX 65
#define PANDA_KEY_MAX 256
#define PANDA_ID_MAX 96
#define PANDA_MODEL_MAX 32
#define PANDA_SPEAKER_MAX 64
#define PANDA_PERSONA_MAX 512
#define PANDA_CITY_MAX 32

typedef struct {
    /* Saved WiFi networks, strongest one in range wins at connect time. */
    panda_wifi_cred_t wifi[PANDA_WIFI_CRED_MAX];
    int wifi_count;

    /* Volcengine Doubao realtime voice */
    char api_mode[12];              /* "o2" (official binary) or "json" */
    char app_id[PANDA_ID_MAX];
    char access_key[PANDA_KEY_MAX];
    char resource_id[PANDA_ID_MAX];
    char app_key[PANDA_ID_MAX];
    char api_key[PANDA_KEY_MAX];    /* JSON fallback mode only */
    char model[PANDA_MODEL_MAX];
    char speaker[PANDA_SPEAKER_MAX];

    int volume;                     /* 0..100 */
    int brightness;                 /* 0..100 */
    int auto_continue;              /* 0/1 */
    int silence_timeout_ms;         /* server VAD end window */
    int max_record_seconds;

    /* Personality menu */
    int personality;                /* 0活泼 1温柔 2安静 */
    int feedback;                   /* 0弱 1中 2强 */
    int animation;                  /* 0低 1中 2高 */

    /* Sleep menu */
    int auto_sleep_s;               /* 0=never, else 60/300/600 */
    int wake_mode;                  /* 0触摸 1语音 2都可以 */
    int sleep_anim;                 /* 0/1 */

    int dnd;                        /* 0/1, mute speaker output */
    char weather_city[PANDA_CITY_MAX];
    /* Localized name returned by the geocoder (e.g. "常州市"); cache only,
     * refreshed on every successful fetch, never edited by the user. */
    char weather_city_label[PANDA_CITY_MAX];

    char persona[PANDA_PERSONA_MAX];
} panda_settings_t;

esp_err_t panda_settings_init(void);
panda_settings_t *panda_settings_get(void);
esp_err_t panda_settings_save(void);
esp_err_t panda_settings_reset(void);
void panda_settings_print(bool include_secrets);
const char *panda_settings_masked(const char *s);

int panda_settings_wifi_count(void);
const panda_wifi_cred_t *panda_settings_wifi_at(int index);
int panda_settings_wifi_index(const char *ssid);
/* Upsert by SSID; persists only after panda_settings_save(). */
esp_err_t panda_settings_wifi_put(const char *ssid, const char *pass);
esp_err_t panda_settings_wifi_remove(const char *ssid);
void panda_settings_wifi_clear(void);

#ifdef __cplusplus
}
#endif
