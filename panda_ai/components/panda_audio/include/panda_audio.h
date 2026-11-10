#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t panda_audio_init(void);
bool panda_audio_ready(void);
bool panda_audio_has_mic(void);

esp_err_t panda_audio_record(int16_t *buffer, size_t samples, size_t *samples_read);
esp_err_t panda_audio_play(const int16_t *buffer, size_t samples, size_t *samples_written);

esp_err_t panda_audio_set_volume(int percent);
esp_err_t panda_audio_get_volume(int *percent);
esp_err_t panda_audio_mute(bool mute);
esp_err_t panda_audio_self_test(void);

#ifdef __cplusplus
}
#endif
