#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANDA_MP3_MAX_FILES 64
#define PANDA_MP3_NAME_MAX 96

typedef enum {
    PANDA_MP3_IDLE = 0,
    PANDA_MP3_PLAYING,
    PANDA_MP3_PAUSED,
} panda_mp3_state_t;

/* Allocates the control primitives; no SD or audio access yet. */
esp_err_t panda_mp3_init(void);

/* Rescans <sd>/Music for .mp3 files and returns the track count. */
int panda_mp3_scan(void);
int panda_mp3_count(void);
const char *panda_mp3_title(int index);
const char *panda_mp3_dir(void);

/* Starts the decode task on one track (index < 0 keeps the current one).
 * Switches the audio clock to the MP3 sample rate for the duration. */
esp_err_t panda_mp3_play(int index);
esp_err_t panda_mp3_toggle(void);
esp_err_t panda_mp3_next(void);
esp_err_t panda_mp3_prev(void);

/* Stops playback, restores the 16 kHz voice clock and waits for the worker
 * to free the decoder/buffers before returning. */
esp_err_t panda_mp3_stop(void);

panda_mp3_state_t panda_mp3_state(void);
bool panda_mp3_active(void);
int panda_mp3_current(void);
const char *panda_mp3_current_title(void);

/* Diagnostics: audible gaps caused by the decoder not keeping up, and how
 * many PCM samples are queued ahead of the I2S writer. */
uint32_t panda_mp3_underruns(void);
size_t panda_mp3_buffered(void);
/* bit0 primed, bit1 eof, bit2 tx_idle, bit3 abort. */
uint32_t panda_mp3_flags(void);

#ifdef __cplusplus
}
#endif
