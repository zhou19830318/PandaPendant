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

/* Switch the shared I2S/codec clock (16 k for voice, 22.05/24/32/44.1/48 k for MP3).
 * The mic shares the I2S clock, so capture is down while music plays and
 * comes back when the rate returns to 16 kHz. */
esp_err_t panda_audio_set_rate(uint32_t sample_rate);
uint32_t panda_audio_get_rate(void);

esp_err_t panda_audio_set_volume(int percent);
esp_err_t panda_audio_get_volume(int *percent);
esp_err_t panda_audio_mute(bool mute);
/* Cuts the PA (the idle noise source) once OUTPUT_IDLE_MS passed without a
 * write. Safe to call from a periodic loop; does nothing while audio plays. */
void panda_audio_poll(void);
bool panda_audio_output_on(void);
/* Peak of the last written PCM block, 0..100 (pre-volume, 0 while muted).
 * Updated by every panda_audio_play() call, so TTS and MP3 share it. */
int panda_audio_get_output_level(void);
/* Raw ES8311 register read (diagnostics: 0x31 mute, 0x32 DAC volume). */
esp_err_t panda_audio_get_codec_reg(uint8_t reg, int *value);
esp_err_t panda_audio_self_test(void);

#ifdef __cplusplus
}
#endif
