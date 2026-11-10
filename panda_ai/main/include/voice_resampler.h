#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 3:2 polyphase resampler (24 kHz TTS -> 16 kHz codec), ported from the
 * vendor demo: 32 taps per phase, Kaiser beta=8, Q15 coefficients. */
#define VOICE_RESAMPLE_TAPS 32
#define VOICE_RESAMPLE_PHASES 2

typedef struct {
    int32_t coeffs[VOICE_RESAMPLE_PHASES * VOICE_RESAMPLE_TAPS];
    int16_t history[VOICE_RESAMPLE_TAPS];
    int wr_idx;
    int phase;
} voice_resampler_t;

void voice_resampler_init(voice_resampler_t *r);

/* Consumes the whole input; phase and history survive across calls, so
 * consecutive TTS deltas join without a seam. cap_out must hold
 * floor(2*n_in/3)+1 samples. */
size_t voice_resampler_process(voice_resampler_t *r, const int16_t *in24,
                               size_t n_in, int16_t *out16, size_t cap_out);

#ifdef __cplusplus
}
#endif
