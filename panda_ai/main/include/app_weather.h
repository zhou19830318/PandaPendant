#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "panda_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int temp_c;
    int weather_code;
    panda_ui_weather_t icon;
    char city[32];
    char condition[16];
    time_t updated;
} app_weather_info_t;

/* Starts the background refresh task (idempotent). Fetches only while the
 * device has a station-mode WiFi connection. */
esp_err_t app_weather_start(void);
/* Request an immediate refresh (city change, web save, CLI). */
void app_weather_refresh(void);
bool app_weather_get_info(app_weather_info_t *info);
/* Returns true once after the geocoder supplied a new localized city name.
 * Must be consumed by a task with an internal stack: persisting it writes
 * NVS, which cannot run on the weather task (its stack is in PSRAM). */
bool app_weather_take_city_label(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
