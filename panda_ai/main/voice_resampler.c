#include "voice_resampler.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define KAISER_BETA 8.0f
#define CUTOFF_NORM (1.0f / 3.0f) /* 8 kHz at 24 kHz */
#define TAPS VOICE_RESAMPLE_TAPS
#define DELAY_A (TAPS / 2.0f)
#define DELAY_B (TAPS / 2.0f + 0.5f)

static float bessel_i0(float x)
{
    float sum = 1.0f;
    float term = 1.0f;
    for (int k = 1; k <= 20; k++) {
        term *= (x * x) / (4.0f * k * k);
        sum += term;
        if (term < 1e-7f * sum) {
            break;
        }
    }
    return sum;
}

void voice_resampler_init(voice_resampler_t *r)
{
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));

    const float i0_beta = bessel_i0(KAISER_BETA);
    const int m = TAPS - 1;

    for (int p = 0; p < VOICE_RESAMPLE_PHASES; p++) {
        const float delay = (p == 0) ? DELAY_A : DELAY_B;
        for (int j = 0; j < TAPS; j++) {
            const float n = (float)j - delay;
            const float sinc = (fabsf(n) < 1e-6f)
                                   ? 2.0f * CUTOFF_NORM
                                   : sinf(2.0f * (float)M_PI * CUTOFF_NORM * n) /
                                         ((float)M_PI * n);
            const float u = ((float)j - delay) / (TAPS / 2.0f);
            const float w = (fabsf(u) >= 1.0f)
                                ? 0.0f
                                : bessel_i0(KAISER_BETA * sqrtf(1.0f - u * u)) / i0_beta;
            /* Delay-reversed per phase so the inner loop is a dot product
             * against the history ring (most recent sample last). */
            const int t = m - j;
            r->coeffs[p * TAPS + t] = (int32_t)(sinc * w * 32768.0f);
        }
    }
}

static inline int16_t produce_output(const voice_resampler_t *r, int phase)
{
    int64_t acc = 0;
    const int32_t *c = &r->coeffs[phase * TAPS];
    for (int t = 0; t < TAPS; t++) {
        const int idx = (r->wr_idx - (TAPS - 1 - t)) & (TAPS - 1);
        acc += (int64_t)r->history[idx] * c[t];
    }
    int32_t s = (int32_t)(acc >> 15);
    if (s > 32767) {
        s = 32767;
    }
    if (s < -32768) {
        s = -32768;
    }
    return (int16_t)s;
}

static inline void shift_in(voice_resampler_t *r, int16_t sample)
{
    r->wr_idx = (r->wr_idx + 1) & (TAPS - 1);
    r->history[r->wr_idx] = sample;
}

size_t voice_resampler_process(voice_resampler_t *r, const int16_t *in24,
                               size_t n_in, int16_t *out16, size_t cap_out)
{
    if (!r || (!in24 && n_in > 0) || !out16) {
        return 0;
    }

    size_t out_count = 0;
    for (size_t i = 0; i < n_in; i++) {
        shift_in(r, in24[i]);
        switch (r->phase) {
        case 0:
            r->phase = 1;
            break;
        case 1:
            if (out_count < cap_out) {
                out16[out_count++] = produce_output(r, 1);
            }
            r->phase = 2;
            break;
        default:
            if (out_count < cap_out) {
                out16[out_count++] = produce_output(r, 0);
            }
            r->phase = 0;
            break;
        }
    }
    return out_count;
}
