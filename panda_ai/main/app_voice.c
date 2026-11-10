#include "app_core.h"
#include "app_settings.h"
#include "panda_audio.h"
#include "panda_connectivity.h"
#include "voice_resampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "voice";

#define VOICE_WSS_URI "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#define DEFAULT_VOICE "zh_female_xiaohe_uranus_bigtts"
#define FRAME_SAMPLES 640
#define WIRE_SAMPLES 320
#define RING_SAMPLES (256 * 1024)
#define RX_CAP (192 * 1024)
#define PCM24_CAP (96 * 1024)
#define PCM16_CAP (64 * 1024)
#define TX_CAP 8192
/* Playback pacing, ported from the vendor demo. The server streams TTS in
 * bursts with gaps up to ~1-2 s, so 200 ms of pre-buffer started playback
 * too early and the ring emptied mid-sentence (audible gaps + clicks). */
#define PLAY_PREBUFFER_SAMPLES 8000  /* 500 ms */
#define PLAY_PAUSE_SAMPLES 1600      /* 100 ms: stop feeding before starvation */
#define PLAY_RESUME_SAMPLES 4800     /* 300 ms: resume once this much is queued */
#define FADE_START_SAMPLES 800       /* 50 ms fade-in on response start */
#define FADE_RESUME_SAMPLES 160      /* 10 ms fade-in after a gap */
#define DECLICK_SAMPLES 160          /* 10 ms ramp to zero before silence */
#define FEED_GAP_US 170000           /* I2S DMA depth (12x240 frames) minus 10 ms */
#define PAUSE_FLUSH_US 700000        /* ring static this long: flush the tail */
/* No-speech guard: a PTT turn with fewer than PTT_SPEECH_FRAMES 40 ms frames
 * above PTT_SPEECH_RMS is an accidental touch. Peak alone misclassified room
 * noise (measured peaks 100-950 with the fan running); a sustained RMS is
 * what separates speech from clicks and hiss. Commit anyway (to clear the
 * server buffer), but suppress that turn's ASR/reply/TTS and answer locally. */
#define PTT_SPEECH_RMS 150
#define PTT_SPEECH_FRAMES 3
#define NO_SPEECH_HINT "我在呢～没听到你说话，再说一次好吗？"
/* Uplink is producer/consumer decoupled (vendor demo design): capture and
 * event handlers only enqueue, one TX task owns the ws send lock. Calling
 * esp_websocket_client_send_text() from the capture pump blocked the RX
 * task on the same lock while a TTS burst was downloading ("Could not lock
 * ws-client" every 1 s, ring drained, reply stalled). */
#define TX_FRAME_CAP 1024
#define TX_QUEUE_DEPTH 8
#define TX_LOCK_WAIT_MS 500
#define MUTE_PUMP_IDLE_MS 50

static esp_websocket_client_handle_t s_ws;
static bool s_ws_connected;
/* Only a link that worked once can tell us the wifi path later wedged:
 * retries after a bad api_key must not bounce the interface. */
static bool s_ever_connected;
static bool s_session_active;
static bool s_session_pending;
static bool s_pending_capture;
static volatile bool s_capture;
static volatile bool s_play_running;
static volatile bool s_play_drain;
static volatile bool s_audio_started;
static volatile bool s_play_active;
static volatile bool s_cancel_play;
static TaskHandle_t s_cap_task;
static TaskHandle_t s_play_task;
static TaskHandle_t s_tx_task;
static SemaphoreHandle_t s_ring_lock;
static SemaphoreHandle_t s_tx_lock;
static QueueHandle_t s_tx_q;

typedef struct {
    uint16_t len;
    uint8_t critical; /* commit/cancel must not be dropped; retried */
    char data[TX_FRAME_CAP];
} tx_frame_t;
static StaticQueue_t s_tx_q_struct;
static uint8_t *s_tx_q_storage;

static int16_t *s_ring;
static size_t s_ring_head;
static size_t s_ring_tail;
static size_t s_ring_count;

static char *s_rx;
static size_t s_rx_len;
static char *s_tx;
static uint8_t *s_pcm24;
static int16_t *s_pcm16;
static char s_headers[320];
static char s_session_id[40];
static uint32_t s_event_seq = 1;
static uint32_t s_audio_deltas;
static uint32_t s_tx_frames;
static bool s_reply_started;
static bool s_reply_done;
static bool s_asr_done;
static uint32_t s_asr_deltas;
static volatile bool s_clip_active;
static volatile bool s_cancel_pending;
static volatile bool s_rx_skip;
static const uint8_t *s_clip_pcm;
static size_t s_clip_samples;
static size_t s_clip_pos;
static char s_asr_text[256];
static char s_reply_text[1024];
/* Per-PTT-turn speech detection. s_ptt_peak is written by the capture task
 * and read on release; both are 32-bit aligned so a torn read can't happen. */
static volatile bool s_no_speech_turn;
static volatile uint32_t s_ptt_peak;
static volatile uint32_t s_ptt_rms_max;
static volatile uint32_t s_ptt_speech_frames;
static int64_t s_ptt_press_us;
static volatile bool s_pump_enabled = true;

/* Resampler state carried across TTS deltas; resetting it per delta added a
 * click and a rate wobble at every chunk boundary. */
static voice_resampler_t s_rs;
static bool s_rs_ready;

/* Declick bookkeeping: last sample handed to the codec and when. Only the
 * play task touches these. */
static int16_t s_last_out;
static int64_t s_last_feed_us;
static bool s_feed_broken;

static panda_voice_ui_t s_ui_state;
static bool s_ui_state_valid;
static volatile uint32_t s_stat_decode_fail;
static volatile uint32_t s_stat_ring_drops;
static volatile uint32_t s_stat_underruns;
static volatile uint32_t s_stat_write_retries;
static volatile uint32_t s_stat_rx_overflows;

static void voice_state(panda_voice_ui_t state)
{
    if (s_ui_state_valid && state == s_ui_state) {
        return;
    }
    s_ui_state = state;
    s_ui_state_valid = true;
    app_core_on_voice_state(state);
}

static void resample_reset(void)
{
    if (!s_rs_ready) {
        voice_resampler_init(&s_rs);
        s_rs_ready = true;
        return;
    }
    memset(s_rs.history, 0, sizeof(s_rs.history));
    s_rs.wr_idx = 0;
    s_rs.phase = 0;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t out = 0;
    if (cap == 0) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)(src ? src : "");
         *p && out + 1 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (out + 2 >= cap) break;
            dst[out++] = '\\';
            dst[out++] = (char)*p;
        } else if (*p < 0x20) {
            if (out + 6 >= cap) break;
            int n = snprintf(dst + out, cap - out, "\\u%04x", *p);
            if (n < 0) break;
            out += (size_t)n;
        } else {
            dst[out++] = (char)*p;
        }
    }
    dst[out] = '\0';
}

/* Personality/feedback menu choices are mapped into the session instructions
 * so the persisted settings actually change how the model talks. */
static void build_persona(char *out, size_t cap)
{
    static const char *const styles[] = {
        "你是一只活泼好动的熊猫挂坠，说话轻快有活力，喜欢用简短的感叹句。",
        "你是一只温柔体贴的熊猫挂坠，说话舒缓耐心，语气亲切，会关心用户。",
        "你是一只安静内敛的熊猫挂坠，说话简洁平和，不主动滔滔不绝。",
    };
    static const char *const feedbacks[] = {
        "回复保持克制，每次一到两句，尽量少追问。",
        "回复保持简洁自然，每次两到三句。",
        "回复可以更热情主动，适当追问并表达情绪，但每次不超过四句。",
    };
    const panda_settings_t *cfg = panda_settings_get();
    int p = cfg->personality >= 0 && cfg->personality <= 2 ? cfg->personality : 1;
    int f = cfg->feedback >= 0 && cfg->feedback <= 2 ? cfg->feedback : 1;
    snprintf(out, cap, "%s%s%s", styles[p], feedbacks[f], cfg->persona);
}

static void gen_uuid(char out[37])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static size_t ring_count(void)
{
    size_t n;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    n = s_ring_count;
    xSemaphoreGive(s_ring_lock);
    return n;
}

static void ring_reset(void)
{
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_ring_head = s_ring_tail = s_ring_count = 0;
    xSemaphoreGive(s_ring_lock);
}

static void ring_push(const int16_t *pcm, size_t samples)
{
    size_t dropped = 0;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    size_t free_n = RING_SAMPLES - s_ring_count;
    if (samples > free_n) {
        dropped = samples - free_n;
        samples = free_n;
    }
    size_t first = RING_SAMPLES - s_ring_head;
    if (first > samples) {
        first = samples;
    }
    memcpy(&s_ring[s_ring_head], pcm, first * sizeof(int16_t));
    if (samples > first) {
        memcpy(&s_ring[0], pcm + first, (samples - first) * sizeof(int16_t));
    }
    s_ring_head = (s_ring_head + samples) % RING_SAMPLES;
    s_ring_count += samples;
    xSemaphoreGive(s_ring_lock);
    if (dropped) {
        s_stat_ring_drops += (uint32_t)dropped;
        ESP_LOGW(TAG, "ring full, dropped %u samples", (unsigned)dropped);
    }
}

static size_t ring_pop(int16_t *pcm, size_t max_samples)
{
    size_t n = 0;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    while (n < max_samples && s_ring_count > 0) {
        pcm[n++] = s_ring[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) % RING_SAMPLES;
        s_ring_count--;
    }
    xSemaphoreGive(s_ring_lock);
    return n;
}

static void apply_fade(int16_t *pcm, size_t n, size_t fade_total,
                       size_t fade_pos, bool fade_in)
{
    if (fade_total == 0) {
        return;
    }
    for (size_t i = 0; i < n && fade_pos + i < fade_total; i++) {
        float gain = (float)(fade_pos + i) / (float)fade_total;
        if (!fade_in) {
            gain = 1.0f - gain;
        }
        pcm[i] = (int16_t)((float)pcm[i] * gain);
    }
}

/* One I2S write with a single short retry. The codec write can block for up
 * to 1 s internally; retrying it 20 times (the old behaviour) turned a
 * transient stall into a multi-second freeze, so a second failure drops the
 * block and the next block fades back in instead. */
static bool play_write(const int16_t *pcm, size_t n)
{
    size_t written = 0;
    esp_err_t err = panda_audio_play(pcm, n, &written);
    if (err != ESP_OK || written == 0) {
        s_stat_write_retries++;
        vTaskDelay(pdMS_TO_TICKS(5));
        written = 0;
        err = panda_audio_play(pcm, n, &written);
        if (err != ESP_OK || written == 0) {
            if ((s_stat_write_retries % 20) == 1) {
                ESP_LOGW(TAG, "codec write failed (%s, %u total)",
                         esp_err_to_name(err), (unsigned)s_stat_write_retries);
            }
            s_feed_broken = true;
            return false;
        }
    }
    s_last_out = pcm[n - 1];
    s_last_feed_us = esp_timer_get_time();
    s_feed_broken = false;
    return true;
}

/* Ramp from the last fed sample down to zero; the I2S DMA's auto_clear then
 * continues with silence instead of stepping from mid-waveform to 0. */
static void play_declick(int16_t *scratch)
{
    int16_t last = s_last_out;
    s_last_out = 0;
    if (last > -8 && last < 8) {
        return;
    }
    const int32_t den = DECLICK_SAMPLES - 1;
    for (size_t i = 0; i < DECLICK_SAMPLES; i++) {
        scratch[i] = (int16_t)((int32_t)last * (int32_t)(den - (int32_t)i) / den);
    }
    size_t written = 0;
    if (panda_audio_play(scratch, DECLICK_SAMPLES, &written) == ESP_OK && written > 0) {
        s_last_feed_us = esp_timer_get_time();
    }
}

static void play_task(void *arg)
{
    (void)arg;
    int16_t block[FRAME_SAMPLES];
    bool started = false;
    bool paused = false;
    bool flushing = false;
    size_t fade_pos = 0;
    size_t fade_len = FADE_START_SAMPLES;
    size_t pause_depth = 0;
    int64_t pause_change_us = 0;
    uint32_t loop_cnt = 0;
    int16_t win_peak = 0;
    uint32_t win_clip = 0;

    while (s_play_running) {
        int64_t now = esp_timer_get_time();

        if (s_cancel_play) {
            s_cancel_play = false;
            play_declick(block);
            ring_reset();
            started = false;
            paused = false;
            flushing = false;
            s_play_active = false;
            s_play_drain = false;
            s_last_feed_us = 0;
            ESP_LOGI(TAG, "playback canceled (ring flushed)");
            continue;
        }

        size_t queued = ring_count();

        /* A new response replaced the ring; drop any pause state. */
        if (s_play_drain) {
            paused = false;
        }
        if (flushing && queued >= PLAY_RESUME_SAMPLES) {
            flushing = false;
            fade_pos = 0;
            fade_len = FADE_RESUME_SAMPLES;
            ESP_LOGI(TAG, "play flush ended, ring refilled=%u", (unsigned)queued);
        }

        if (!started) {
            if (queued == 0) {
                if (s_play_drain) {
                    s_play_drain = false;
                    s_play_active = false;
                    if (!s_audio_started) {
                        voice_state(PANDA_VOICE_IDLE);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!s_play_drain && queued < PLAY_PREBUFFER_SAMPLES) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            started = true;
            paused = false;
            flushing = false;
            fade_pos = 0;
            fade_len = FADE_START_SAMPLES;
            s_last_feed_us = 0; /* DMA is silent here: start from zero */
            ESP_LOGI(TAG, "play start ring=%u", (unsigned)queued);
        }

        if (queued == 0) {
            if (s_play_drain) {
                play_declick(block);
                s_play_drain = false;
                s_play_active = false;
                started = false;
                s_last_feed_us = 0;
                if (!s_audio_started) {
                    voice_state(PANDA_VOICE_IDLE);
                }
                ESP_LOGI(TAG, "play drain complete");
            } else if (s_audio_started) {
                /* Server gap outlived the DMA queue: re-buffer rather than
                 * replaying fragments around the hole. */
                s_stat_underruns++;
                play_declick(block);
                s_play_active = false;
                started = false;
                s_last_feed_us = 0;
                ESP_LOGW(TAG, "playback underrun, re-buffering (%u)",
                         (unsigned)s_stat_underruns);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Pause instead of playing partial blocks when the ring is about to
         * run dry while the server is still streaming. */
        if (!s_play_drain && !flushing && s_audio_started && queued < PLAY_PAUSE_SAMPLES) {
            if (!paused) {
                play_declick(block);
                paused = true;
                pause_depth = queued;
                pause_change_us = now;
                s_play_active = false;
                ESP_LOGI(TAG, "play pause ring=%u", (unsigned)queued);
            } else if (queued != pause_depth) {
                pause_depth = queued;
                pause_change_us = now;
            }
            if (paused) {
                if (queued >= PLAY_RESUME_SAMPLES) {
                    paused = false;
                    fade_pos = 0;
                    fade_len = FADE_RESUME_SAMPLES;
                    ESP_LOGI(TAG, "play resume ring=%u", (unsigned)queued);
                } else if (now - pause_change_us > PAUSE_FLUSH_US) {
                    paused = false;
                    flushing = true;
                    fade_pos = 0;
                    fade_len = FADE_RESUME_SAMPLES;
                    ESP_LOGW(TAG, "play pause stall (ring=%u static 700ms), flush tail",
                             (unsigned)queued);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
            }
        }

        bool tail = s_play_drain || flushing;
        if (queued < FRAME_SAMPLES && !tail) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t n = ring_pop(block, FRAME_SAMPLES);
        if (n == 0) {
            continue;
        }

        if (s_feed_broken) {
            fade_pos = 0;
            fade_len = FADE_RESUME_SAMPLES;
        } else if (s_last_feed_us != 0 && now - s_last_feed_us > FEED_GAP_US) {
            /* DMA ran dry since the last write; resuming at full amplitude
             * would step out of silence. */
            fade_pos = 0;
            fade_len = FADE_RESUME_SAMPLES;
            ESP_LOGW(TAG, "DMA gap %lld ms, fade-in",
                     (long long)(now - s_last_feed_us) / 1000);
        }

        if (fade_pos < fade_len) {
            apply_fade(block, n, fade_len, fade_pos, true);
            fade_pos += n;
        }

        if (tail && queued <= FRAME_SAMPLES) {
            /* Last (possibly partial) block: fade out instead of letting the
             * DMA zero-fill cut the waveform. */
            apply_fade(block, n, n, 0, false);
            play_write(block, n);
            if (ring_count() == 0) {
                flushing = false;
                s_play_active = false;
                started = false;
                if (s_play_drain) {
                    s_play_drain = false;
                    s_last_feed_us = 0;
                    if (!s_audio_started) {
                        voice_state(PANDA_VOICE_IDLE);
                    }
                    ESP_LOGI(TAG, "play drain complete (tail)");
                }
                continue;
            }
        } else {
            play_write(block, n);
            s_play_active = true;
        }

        for (size_t i = 0; i < n; i++) {
            int32_t v = block[i] < 0 ? -block[i] : block[i];
            if (v > win_peak) {
                win_peak = (int16_t)(v > 32767 ? 32767 : v);
            }
            if (v >= 32000) {
                win_clip++;
            }
        }
        if ((++loop_cnt % 125) == 0) {
            ESP_LOGI(TAG, "play loop #%u: ring=%u peak=%d nearclip=%u",
                     (unsigned)loop_cnt, (unsigned)ring_count(), win_peak,
                     (unsigned)win_clip);
            win_peak = 0;
            win_clip = 0;
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t ws_send_now(const char *buf, size_t len)
{
    if (!s_ws || !s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(s_ws, buf, (int)len, pdMS_TO_TICKS(TX_LOCK_WAIT_MS));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

/* Producers never touch the ws client lock: enqueue a copy and let the TX
 * task drain it. Returns TIMEOUT when the queue is full (dropped frame). */
static esp_err_t tx_enqueue(const char *buf, size_t len, bool critical)
{
    if (!s_tx_q) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || len >= TX_FRAME_CAP) {
        return ESP_ERR_INVALID_SIZE;
    }
    tx_frame_t frame;
    frame.len = (uint16_t)len;
    frame.critical = critical;
    memcpy(frame.data, buf, len);
    return xQueueSend(s_tx_q, &frame, pdMS_TO_TICKS(10)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void tx_task(void *arg)
{
    (void)arg;
    tx_frame_t frame;
    uint32_t drops = 0;
    while (true) {
        if (xQueueReceive(s_tx_q, &frame, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (!s_ws || !s_ws_connected) {
            continue;
        }
        int sent = -1;
        int attempts = frame.critical ? 4 : 1;
        for (int i = 0; i < attempts; i++) {
            sent = esp_websocket_client_send_text(s_ws, frame.data, frame.len,
                                                  pdMS_TO_TICKS(TX_LOCK_WAIT_MS));
            if (sent == (int)frame.len) {
                break;
            }
            if (i + 1 < attempts) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (sent != (int)frame.len) {
            if ((++drops % 20) == 1) {
                ESP_LOGW(TAG, "tx frame dropped (%u bytes%s, %u total)",
                         (unsigned)frame.len, frame.critical ? " critical" : "",
                         (unsigned)drops);
            }
        }
    }
}

static esp_err_t send_session_create(void)
{
    const panda_settings_t *cfg = panda_settings_get();
    char persona[2200];
    char persona_plain[800];
    char uuid[37];
    build_persona(persona_plain, sizeof(persona_plain));
    json_escape(persona, sizeof(persona), persona_plain);
    gen_uuid(uuid);

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = snprintf(s_tx, TX_CAP,
        "{\"type\":\"session.create\",\"event_id\":\"event_%u\","
        "\"session\":{\"type\":\"realtime\",\"id\":\"%s\",\"model\":\"%s\","
        "\"instructions\":\"%s\",\"audio\":{"
        "\"input\":{\"format\":{\"type\":\"pcm\",\"rate\":16000}},"
        "\"output\":{\"format\":{\"type\":\"pcm_s16le\",\"rate\":24000},\"voice\":\"%s\"},"
        "\"speed\":0,\"loudness\":0}},"
        "\"extension\":{\"asr\":{\"extra\":{}},\"tts\":{\"extra\":{}},"
        "\"dialog\":{\"extra\":{\"enable_loudness_norm\":true,\"enable_music\":false}}}}",
        (unsigned)s_event_seq++, uuid,
        cfg->model[0] ? cfg->model : "1.2.6.1",
        persona,
        cfg->speaker[0] ? cfg->speaker : DEFAULT_VOICE);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (n > 0 && n < TX_CAP) {
        s_session_pending = true;
        ESP_LOGI(TAG, "session.create %d bytes", n);
        err = ws_send_now(s_tx, (size_t)n);
    } else {
        ESP_LOGE(TAG, "session.create too long (%d)", n);
    }
    xSemaphoreGive(s_tx_lock);
    return err;
}

static esp_err_t send_audio_append(const int16_t *pcm, size_t samples)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = snprintf(s_tx, TX_CAP,
                     "{\"event_id\":\"event_%u\",\"type\":\"input_audio_buffer.append\",\"audio\":\"",
                     (unsigned)s_event_seq++);
    if (n <= 0 || (size_t)n + 4 >= TX_CAP) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_NO_MEM;
    }
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)s_tx + n, TX_CAP - (size_t)n - 3,
                                   &olen, (const unsigned char *)pcm,
                                   samples * sizeof(int16_t));
    if (rc != 0) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_NO_MEM;
    }
    n += (int)olen;
    s_tx[n++] = '"';
    s_tx[n++] = '}';
    esp_err_t err = tx_enqueue(s_tx, (size_t)n, false);
    xSemaphoreGive(s_tx_lock);
    return err;
}

static esp_err_t send_commit(void)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = snprintf(s_tx, TX_CAP,
                     "{\"event_id\":\"event_%u\",\"type\":\"input_audio_buffer.commit\"}",
                     (unsigned)s_event_seq++);
    esp_err_t err = (n > 0 && n < TX_CAP) ? tx_enqueue(s_tx, (size_t)n, true) : ESP_ERR_NO_MEM;
    xSemaphoreGive(s_tx_lock);
    return err;
}

static esp_err_t send_cancel(void)
{
    static const char cancel[] = "{\"type\":\"response.cancel\"}";
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t err = tx_enqueue(cancel, sizeof(cancel) - 1, true);
    xSemaphoreGive(s_tx_lock);
    return err;
}

static void append_text(char *dst, size_t cap, const char *src)
{
    size_t used = strlen(dst);
    if (used + 1 >= cap) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - used - 1) {
        n = cap - used - 1;
    }
    memcpy(dst + used, src, n);
    dst[used + n] = '\0';
}

/* Transcription deltas are not consistently shaped: some servers send only
 * the new tail, others repeat the whole transcript each time. Appending
 * blindly showed "hellohellohello", so detect both shapes: a delta that
 * starts with the current text is cumulative (replace), a delta already at
 * the end is a repeat (ignore), anything else is a tail (append). */
static void merge_text(char *dst, size_t cap, const char *delta)
{
    size_t used = strlen(dst);
    size_t len = strlen(delta);
    if (len == 0) {
        return;
    }
    if (used > 0 && len >= used && memcmp(delta, dst, used) == 0) {
        if (len >= cap) {
            len = cap - 1;
        }
        memcpy(dst, delta, len);
        dst[len] = '\0';
        return;
    }
    if (used > 0 && len <= used && memcmp(dst + used - len, delta, len) == 0) {
        return;
    }
    append_text(dst, cap, delta);
}

static void show_no_speech_hint(const char *why)
{
    ESP_LOGI(TAG, "no speech (%s); answering locally", why);
    app_core_on_subtitle(NO_SPEECH_HINT);
    voice_state(PANDA_VOICE_IDLE);
}

static void handle_json(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "bad json (%u bytes): %.80s", (unsigned)len, json);
        return;
    }
    const char *type = json_str(root, "type");

    if (!strcmp(type, "input_audio_buffer.committed")) {
        ESP_LOGI(TAG, "commit acked");
    } else if (!strcmp(type, "session.updated")) {
        ESP_LOGD(TAG, "session updated");
    } else if (!strcmp(type, "session.created")) {
        cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
        const char *id = session ? json_str(session, "id") : "";
        snprintf(s_session_id, sizeof(s_session_id), "%s", id);
        s_session_active = true;
        s_session_pending = false;
        ESP_LOGI(TAG, "session created id=%.36s", s_session_id);
        if (s_pending_capture) {
            s_pending_capture = false;
            s_capture = true;
            voice_state(PANDA_VOICE_LISTENING);
        } else {
            voice_state(PANDA_VOICE_IDLE);
        }
    } else if (!strcmp(type, "session.closed")) {
        s_session_active = false;
        s_session_pending = false;
        ESP_LOGI(TAG, "session closed");
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.started")) {
        if (s_no_speech_turn) {
            ESP_LOGI(TAG, "asr started (muted: no-speech turn)");
            cJSON_Delete(root);
            return;
        }
        s_asr_text[0] = '\0';
        s_asr_done = false;
        s_asr_deltas = 0;
        voice_state(PANDA_VOICE_LISTENING);
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.delta")) {
        /* Deltas are merged but never rendered: the server may send the whole
         * transcript again per delta, and late deltas arrive after `completed`
         * (the old code appended them, showing "hellohellohello"). The demo
         * fills the transcript once, on `completed`. Keep the merged text as a
         * fallback because this server sometimes sends an empty `transcript`. */
        const char *delta = json_str(root, "delta");
        if (s_no_speech_turn) {
            ESP_LOGD(TAG, "asr delta muted: '%s'", delta);
        } else {
            if (!s_asr_done) {
                merge_text(s_asr_text, sizeof(s_asr_text), delta);
            }
            if (++s_asr_deltas <= 12) {
                ESP_LOGI(TAG, "asr delta #%u '%s'", (unsigned)s_asr_deltas, delta);
            }
        }
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
        if (s_no_speech_turn) {
            ESP_LOGI(TAG, "asr completed (muted: no-speech turn)");
            cJSON_Delete(root);
            return;
        }
        const char *final_text = json_str(root, "transcript");
        if (final_text[0]) {
            snprintf(s_asr_text, sizeof(s_asr_text), "%s", final_text);
        }
        s_asr_done = true;
        if (s_asr_text[0] == '\0') {
            /* Recognized nothing: an empty bubble looks broken, and any
             * server response to silence is noise. Answer locally instead. */
            s_no_speech_turn = true;
            show_no_speech_hint("empty transcript");
        } else {
            app_core_on_subtitle(s_asr_text);
            voice_state(PANDA_VOICE_THINKING);
            ESP_LOGI(TAG, "asr: %s", s_asr_text);
        }
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.failed")) {
        if (s_no_speech_turn) {
            ESP_LOGI(TAG, "asr failed (muted: no-speech turn)");
        } else {
            ESP_LOGW(TAG, "asr failed");
            voice_state(PANDA_VOICE_ERROR);
        }
    } else if (!strcmp(type, "response.output_text.delta")) {
        if (!s_reply_done && !s_no_speech_turn) {
            if (!s_reply_started) {
                s_reply_text[0] = '\0';
                s_reply_started = true;
            }
            merge_text(s_reply_text, sizeof(s_reply_text), json_str(root, "delta"));
            /* Render once on output_text.done (vendor demo); redrawing the
             * subtitle per delta kept the ws event handler busy while TTS
             * audio was arriving. */
        }
    } else if (!strcmp(type, "response.output_text.done")) {
        if (s_no_speech_turn) {
            ESP_LOGI(TAG, "reply text muted: %s", json_str(root, "text"));
            cJSON_Delete(root);
            return;
        }
        const char *text = json_str(root, "text");
        if (text[0]) {
            snprintf(s_reply_text, sizeof(s_reply_text), "%s", text);
        }
        s_reply_done = true;
        app_core_on_subtitle(s_reply_text);
        ESP_LOGI(TAG, "reply(%u bytes): %s", (unsigned)strlen(s_reply_text), s_reply_text);
    } else if (!strcmp(type, "response.output_audio.started")) {
        if (s_no_speech_turn) {
            /* The server generated speech from silence; drop it and stay on
             * the local polite hint. */
            s_cancel_pending = true;
            s_cancel_play = true;
            resample_reset();
            send_cancel();
            ESP_LOGI(TAG, "tts started on no-speech turn; cancel sent");
            cJSON_Delete(root);
            return;
        }
        s_cancel_pending = false;
        s_cancel_play = false;
        s_audio_started = true;
        s_play_drain = false;
        s_play_active = false;
        s_audio_deltas = 0;
        ring_reset();
        resample_reset();
        voice_state(PANDA_VOICE_SPEAKING);
        ESP_LOGI(TAG, "tts started voice=%s", json_str(root, "tts_type"));
    } else if (!strcmp(type, "response.output_audio.delta")) {
        if (s_cancel_pending) {
            /* Late audio from the canceled response: drop instead of replaying. */
            cJSON_Delete(root);
            return;
        }
        const char *delta = json_str(root, "delta");
        size_t olen = 0;
        int rc = mbedtls_base64_decode(s_pcm24, PCM24_CAP, &olen,
                                       (const unsigned char *)delta, strlen(delta));
        if (rc == 0 && olen >= 2) {
            size_t out = voice_resampler_process(&s_rs, (const int16_t *)s_pcm24,
                                                 olen / 2, s_pcm16,
                                                 PCM16_CAP / sizeof(int16_t));
            ring_push(s_pcm16, out);
            s_audio_deltas++;
            s_audio_started = true;
            /* No UI call per delta: SPEAKING is set once in started, and
             * interleaved text/audio deltas used to flap the screen
             * speaking↔thinking, each transition rendering under the ws
             * client lock and starving the uplink. */
        } else {
            s_stat_decode_fail++;
            ESP_LOGW(TAG, "tts decode failed rc=%d len=%u", rc, (unsigned)olen);
        }
        if ((s_audio_deltas % 25) == 0 && s_audio_deltas) {
            ESP_LOGI(TAG, "tts deltas=%u ring=%u", (unsigned)s_audio_deltas,
                     (unsigned)ring_count());
        }
    } else if (!strcmp(type, "response.output_audio.done")) {
        /* No more audio will arrive for this response; the play task drains
         * the ring and then reports idle. */
        s_audio_started = false;
        s_play_drain = true;
        ESP_LOGI(TAG, "tts done deltas=%u ring=%u", (unsigned)s_audio_deltas,
                 (unsigned)ring_count());
    } else if (!strcmp(type, "response.done")) {
        s_audio_started = false;
        s_play_drain = true;
        s_cancel_pending = false;
        s_reply_started = false;
        s_reply_done = false;
        s_no_speech_turn = false;
    } else if (!strcmp(type, "response.canceled")) {
        s_cancel_play = true;
        resample_reset();
        s_play_drain = false;
        s_play_active = false;
        s_audio_started = false;
        s_reply_done = true;
        s_no_speech_turn = false;
        voice_state(PANDA_VOICE_IDLE);
    } else if (!strcmp(type, "error")) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *msg = err ? json_str(err, "message") : "";
        const char *code = err ? json_str(err, "code") : "";
        if (s_no_speech_turn) {
            /* Empty committed audio can make the server answer with an error;
             * that is expected for an accidental touch, not a device fault. */
            ESP_LOGI(TAG, "server error muted (no-speech turn): %s %s", code, msg);
            s_no_speech_turn = false;
            cJSON_Delete(root);
            return;
        }
        ESP_LOGE(TAG, "server error %s %s", code, msg);
        voice_state(PANDA_VOICE_ERROR);
        panda_ui_toast(msg[0] ? msg : "语音服务错误", 2000);
        s_session_pending = false;
    } else {
        ESP_LOGD(TAG, "event %s", type);
    }
    cJSON_Delete(root);
}

static void feed_json(const char *data, size_t len)
{
    if (s_rx_skip) {
        const char *close = memchr(data, '}', len);
        if (!close) {
            return;
        }
        len -= (size_t)(close + 1 - data);
        data = close + 1;
        s_rx_skip = false;
    }

    if (s_rx_len + len > RX_CAP) {
        s_stat_rx_overflows++;
        ESP_LOGW(TAG, "rx overflow, resyncing after next event");
        s_rx_len = 0;
        const char *close = memchr(data, '}', len);
        if (!close) {
            s_rx_skip = true;
            return;
        }
        len -= (size_t)(close + 1 - data);
        data = close + 1;
    }
    if (len == 0) {
        return;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;

    size_t start = 0;
    while (start < s_rx_len) {
        while (start < s_rx_len && s_rx[start] != '{') {
            start++;
        }
        if (start >= s_rx_len) {
            s_rx_len = 0;
            return;
        }
        int depth = 0;
        bool in_str = false;
        bool esc = false;
        size_t end = 0;
        for (size_t i = start; i < s_rx_len; i++) {
            char c = s_rx[i];
            if (in_str) {
                if (esc) {
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    in_str = false;
                }
            } else if (c == '"') {
                in_str = true;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    end = i + 1;
                    break;
                }
            }
        }
        if (!end) {
            break;
        }
        handle_json(s_rx + start, end - start);
        start = end;
    }
    if (start > 0) {
        memmove(s_rx, s_rx + start, s_rx_len - start);
        s_rx_len -= start;
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_connected = true;
        s_ever_connected = true;
        panda_connectivity_notify_net_ok();
        s_session_active = false;
        s_session_pending = false;
        s_cancel_pending = false;
        s_rx_len = 0;
        s_rx_skip = false;
        s_audio_started = false;
        s_play_drain = false;
        s_play_active = false;
        s_cancel_play = true;
        resample_reset();
        if (s_tx_q) {
            xQueueReset(s_tx_q);
        }
        ESP_LOGI(TAG, "websocket connected");
        if (s_ws) {
            /* Handshake is done: the library holds its client lock while it
             * sends a PING, so the 1 s handshake cadence turns any stalled
             * write into a lock hog that also blocks our uplink. The vendor
             * demo relaxes it to 30 s once connected for the same reason. */
            esp_websocket_client_set_ping_interval_sec(s_ws, 30);
        }
        voice_state(PANDA_VOICE_CONNECTING);
        send_session_create();
        break;
    case WEBSOCKET_EVENT_ERROR:
        /* The component fires ERROR for every error-level log, including a
         * benign 500 ms send timeout; the link is still up and the ws task
         * keeps receiving. Tearing the session down here stopped the uplink
         * pump and made the server close the session with
         * AudioServerNoAudioInputTooLongError ~70 s later. */
        ESP_LOGD(TAG, "websocket error event (link kept)");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_connected = false;
        s_session_active = false;
        s_session_pending = false;
        s_pending_capture = false;
        s_capture = false;
        s_cancel_pending = false;
        s_audio_started = false;
        s_play_drain = false;
        s_play_active = false;
        s_cancel_play = true;
        resample_reset();
        if (s_tx_q) {
            xQueueReset(s_tx_q);
        }
        ESP_LOGW(TAG, "websocket down (event %" PRId32 ")", id);
        if (s_ws) {
            esp_websocket_client_set_ping_interval_sec(s_ws, 1);
        }
        /* A failed reconnect attempt emits DISCONNECTED while wifi still
         * reports online; a run of them means the data path wedged. */
        if (id == WEBSOCKET_EVENT_DISCONNECTED && s_ever_connected &&
            panda_connectivity_is_connected()) {
            panda_connectivity_notify_net_error("ws");
        }
        voice_state(PANDA_VOICE_OFFLINE);
        break;
    case WEBSOCKET_EVENT_DATA:
        /* 0x1 = text frame, 0x0 = continuation of a fragmented frame. The
         * old code only accepted 0x1, so large JSON events split by the
         * server were silently discarded. */
        if (ev && (ev->op_code == 0x1 || ev->op_code == 0x0) && ev->data_len > 0) {
            feed_json(ev->data_ptr, (size_t)ev->data_len);
        }
        break;
    default:
        break;
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    static const int16_t silence[WIRE_SAMPLES];
    int16_t *buf = heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t append_fails = 0;
    uint32_t clip_stalls = 0;
    if (!buf) {
        ESP_LOGE(TAG, "capture buffer alloc failed");
        s_cap_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "uplink pump started (50ms silence keepalive)");
    while (true) {
        if (!s_session_active) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s_clip_active && s_capture) {
            size_t left = s_clip_samples - s_clip_pos;
            size_t n = left < WIRE_SAMPLES ? left : WIRE_SAMPLES;
            bool sent = n > 0 &&
                        send_audio_append((const int16_t *)s_clip_pcm + s_clip_pos, n) == ESP_OK;
            if (sent) {
                s_clip_pos += n;
                clip_stalls = 0;
            } else if (++clip_stalls > 200) {
                /* ~4 s of backpressure: the turn is unusable, commit what
                 * we have rather than hanging in LISTENING forever. */
                s_clip_active = false;
                s_capture = false;
                send_commit();
                voice_state(PANDA_VOICE_THINKING);
                ESP_LOGW(TAG, "test clip stalled (%u samples sent), commit forced",
                         (unsigned)s_clip_pos);
                clip_stalls = 0;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (s_clip_pos >= s_clip_samples) {
                s_clip_active = false;
                s_capture = false;
                send_commit();
                voice_state(PANDA_VOICE_THINKING);
                ESP_LOGI(TAG, "test clip done (%u samples); commit sent, pump keeps running",
                         (unsigned)s_clip_samples);
                vTaskDelay(pdMS_TO_TICKS(20));
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }
        if (s_capture) {
            size_t read = 0;
            esp_err_t err = panda_audio_record(buf, FRAME_SAMPLES, &read);
            if (err == ESP_OK && read == FRAME_SAMPLES) {
                int32_t peak = 0;
                uint64_t sumsq = 0;
                for (size_t i = 0; i < read; i++) {
                    int32_t v = buf[i];
                    int32_t a = v < 0 ? -v : v;
                    if (a > peak) {
                        peak = a;
                    }
                    sumsq += (uint32_t)(v * v);
                }
                uint32_t rms = (uint32_t)sqrtf((float)sumsq / (float)read);
                if (peak > (int32_t)s_ptt_peak) {
                    s_ptt_peak = (uint32_t)peak;
                }
                if (rms > s_ptt_rms_max) {
                    s_ptt_rms_max = rms;
                }
                if (rms >= PTT_SPEECH_RMS) {
                    s_ptt_speech_frames++;
                }
                bool ok = true;
                for (size_t base = 0; base < read; base += WIRE_SAMPLES) {
                    size_t n = (read - base) < WIRE_SAMPLES ? (read - base) : WIRE_SAMPLES;
                    if (send_audio_append(buf + base, n) != ESP_OK) {
                        ok = false;
                        break;
                    }
                    if ((++s_tx_frames % 250) == 0) {
                        ESP_LOGI(TAG, "uplink frames=%u (live)", (unsigned)s_tx_frames);
                    }
                }
                if (!ok && (++append_fails % 50) == 1) {
                    /* Drop this sub-frame and keep the turn alive: aborting
                     * capture here lost the commit/release entirely. */
                    ESP_LOGW(TAG, "uplink frame dropped (queue busy, %u total)",
                             (unsigned)append_fails);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }
        /* Silence keepalive: the server stalls TTS / kills the session if
         * the input stream goes dead, so keep it flowing during playback.
         * 50 ms cadence (vendor demo) and an adaptive skip while the TX
         * queue is backed up, so a TTS burst is never starved by uplink. */
        if (!s_pump_enabled) {
            vTaskDelay(pdMS_TO_TICKS(MUTE_PUMP_IDLE_MS));
            continue;
        }
        if (uxQueueMessagesWaiting(s_tx_q) > (TX_QUEUE_DEPTH / 2)) {
            vTaskDelay(pdMS_TO_TICKS(MUTE_PUMP_IDLE_MS));
            continue;
        }
        if (send_audio_append(silence, WIRE_SAMPLES) != ESP_OK) {
            ESP_LOGD(TAG, "silence append failed");
        }
        if ((++s_tx_frames % 250) == 0) {
            ESP_LOGI(TAG, "uplink frames=%u (silence)", (unsigned)s_tx_frames);
        }
        vTaskDelay(pdMS_TO_TICKS(MUTE_PUMP_IDLE_MS));
    }
}

esp_err_t app_voice_start(void)
{
    if (s_ws) {
        return ESP_OK;
    }
    const panda_settings_t *cfg = panda_settings_get();
    if (strcmp(cfg->api_mode, "o2") == 0) {
        ESP_LOGW(TAG, "api_mode=o2 not implemented; set api_mode=json");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!cfg->api_key[0]) {
        ESP_LOGW(TAG, "api_key empty; configure settings first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!panda_connectivity_is_connected()) {
        ESP_LOGW(TAG, "wifi not connected yet; websocket will retry");
    }

    s_ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_rx = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM);
    s_tx = heap_caps_malloc(TX_CAP, MALLOC_CAP_SPIRAM);
    s_pcm24 = heap_caps_malloc(PCM24_CAP, MALLOC_CAP_SPIRAM);
    s_pcm16 = heap_caps_malloc(PCM16_CAP, MALLOC_CAP_SPIRAM);
    s_ring_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    /* Static queue in PSRAM: 8 pending frames would otherwise cost ~8 KB of
     * internal RAM, which the TLS handshake and LVGL also need. */
    s_tx_q_storage = heap_caps_aligned_alloc(8, TX_QUEUE_DEPTH * sizeof(tx_frame_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tx_q_storage) {
        s_tx_q = xQueueCreateStatic(TX_QUEUE_DEPTH, sizeof(tx_frame_t),
                                    s_tx_q_storage, &s_tx_q_struct);
    }
    if (!s_ring || !s_rx || !s_tx || !s_pcm24 || !s_pcm16 || !s_ring_lock ||
        !s_tx_lock || !s_tx_q) {
        ESP_LOGE(TAG, "PSRAM buffers unavailable");
        return ESP_ERR_NO_MEM;
    }
    ring_reset();
    resample_reset();
    s_rx_len = 0;
    s_rx_skip = false;
    s_cancel_play = false;
    s_play_running = true;
    xTaskCreate(play_task, "voice_play", 4096, NULL, 10, &s_play_task);
    xTaskCreate(capture_task, "voice_cap", 4096, NULL, 9, &s_cap_task);
    xTaskCreate(tx_task, "voice_tx", 4096, NULL, 8, &s_tx_task);

    snprintf(s_headers, sizeof(s_headers), "X-Api-Key: %s\r\n", cfg->api_key);
    esp_websocket_client_config_t ws_cfg = {
        .uri = VOICE_WSS_URI,
        .headers = s_headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
        .buffer_size = 4096,
        .task_stack = 8192,
        /* Vendor demo raised this from the default 5 to 7: at 5 the ws task
         * is starved by play(10)/capture(9)/tx(8), RX backs up and playback
         * gaps appear. Keep it above UI (5) and below the audio tasks. */
        .task_prio = 7,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 5000,
        .ping_interval_sec = 1,
        .disable_auto_reconnect = false,
    };
    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "websocket init failed");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return err;
    }
    ESP_LOGI(TAG, "voice client started (JSON mode, model=%s)", cfg->model);
    return ESP_OK;
}


extern const uint8_t whoareyou_wav_start[] asm("_binary_whoareyou_wav_start");
extern const uint8_t whoareyou_wav_end[] asm("_binary_whoareyou_wav_end");

static bool load_test_clip(void)
{
    const uint8_t *p = whoareyou_wav_start;
    size_t len = (size_t)(whoareyou_wav_end - whoareyou_wav_start);
    if (len < 44 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        return false;
    }
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t sz = (uint32_t)p[off + 4] | ((uint32_t)p[off + 5] << 8) |
                      ((uint32_t)p[off + 6] << 16) | ((uint32_t)p[off + 7] << 24);
        if (memcmp(p + off, "data", 4) == 0 && off + 8 + sz <= len) {
            s_clip_pcm = p + off + 8;
            s_clip_samples = sz / sizeof(int16_t);
            return s_clip_samples > 0;
        }
        off += 8 + sz + (sz & 1);
    }
    return false;
}

void app_voice_set_pump(bool enabled)
{
    s_pump_enabled = enabled;
    ESP_LOGI(TAG, "uplink silence pump %s", enabled ? "on" : "off");
}

esp_err_t app_voice_send_test_clip(void)
{
    if (!s_ws_connected || !s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_clip_pcm && !load_test_clip()) {
        return ESP_ERR_NOT_FOUND;
    }
    s_clip_pos = 0;
    s_pending_capture = false;
    s_no_speech_turn = false;
    s_clip_active = true;
    s_capture = true;
    voice_state(PANDA_VOICE_LISTENING);
    ESP_LOGI(TAG, "test clip: %u samples", (unsigned)s_clip_samples);
    return ESP_OK;
}

void app_voice_ptt_press(void)
{
    s_ptt_press_us = esp_timer_get_time();
    s_ptt_peak = 0;
    s_ptt_rms_max = 0;
    s_ptt_speech_frames = 0;
    s_no_speech_turn = false;
    if (!s_ws) {
        esp_err_t err = app_voice_start();
        if (err != ESP_OK) {
            panda_ui_toast("语音未配置", 1500);
            return;
        }
    }
    if (!s_ws_connected) {
        s_pending_capture = true;
        voice_state(PANDA_VOICE_CONNECTING);
        return;
    }
    if (!s_session_active) {
        if (!s_session_pending) {
            send_session_create();
        }
        s_pending_capture = true;
        voice_state(PANDA_VOICE_CONNECTING);
        return;
    }
    s_clip_active = false;
    if (s_audio_started || ring_count() > 0 || s_play_active) {
        /* Barge-in: let the play task ramp down and flush instead of cutting
         * the waveform at an arbitrary sample (an audible click). */
        s_cancel_pending = true;
        send_cancel();
        s_cancel_play = true;
        resample_reset();
    }
    s_asr_text[0] = '\0';
    s_reply_text[0] = '\0';
    s_reply_started = false;
    s_reply_done = false;
    s_asr_done = false;
    s_asr_deltas = 0;
    s_capture = true;
    s_pending_capture = false;
    voice_state(PANDA_VOICE_LISTENING);
}

void app_voice_ptt_release(void)
{
    bool was_capturing = s_capture;
    s_capture = false;
    s_pending_capture = false;
    if (!was_capturing) {
        return;
    }
    uint32_t held_ms = (uint32_t)((esp_timer_get_time() - s_ptt_press_us) / 1000);
    uint32_t peak = s_ptt_peak;
    uint32_t rms = s_ptt_rms_max;
    uint32_t speech_frames = s_ptt_speech_frames;
    ESP_LOGI(TAG, "ptt release: held=%" PRIu32 "ms peak=%" PRIu32 " rms=%" PRIu32
             " speech_frames=%" PRIu32, held_ms, peak, rms, speech_frames);
    if (speech_frames < PTT_SPEECH_FRAMES) {
        /* Accidental touch or silent hold: commit to clear the server buffer,
         * then answer on-device and mute this turn's server events. */
        s_no_speech_turn = true;
        if (s_session_active) {
            send_commit();
        }
        show_no_speech_hint("no sustained speech");
        return;
    }
    if (s_session_active) {
        send_commit();
        voice_state(PANDA_VOICE_THINKING);
    }
}


bool app_voice_is_active(void)
{
    return s_ws_connected;
}

esp_err_t app_voice_cancel(void)
{
    s_capture = false;
    s_pending_capture = false;
    s_clip_active = false;
    s_cancel_pending = true;
    s_cancel_play = true;
    resample_reset();
    s_play_drain = false;
    s_play_active = false;
    s_audio_started = false;
    s_asr_done = true;
    s_reply_done = true;
    s_no_speech_turn = false;
    voice_state(PANDA_VOICE_IDLE);
    if (!s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_cancel();
}

/* Called from app_core when the STA connection goes up or down. The websocket
 * client manages its own retries; on a drop we only clear stale session and
 * audio state so a reconnect never replays old TTS or a stale capture. */
void app_voice_set_network(bool online)
{
    if (!s_ws) {
        return;
    }
    if (online) {
        voice_state(PANDA_VOICE_CONNECTING);
        return;
    }
    s_capture = false;
    s_pending_capture = false;
    s_session_active = false;
    s_session_pending = false;
    s_cancel_pending = false;
    s_audio_started = false;
    s_play_drain = false;
    s_play_active = false;
    s_cancel_play = true;
    s_no_speech_turn = false;
    resample_reset();
    voice_state(PANDA_VOICE_OFFLINE);
}

esp_err_t app_voice_get_stats(app_voice_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    stats->connected = s_ws_connected;
    stats->session = s_session_active;
    stats->deltas = s_audio_deltas;
    stats->decode_failures = s_stat_decode_fail;
    stats->ring_drops = s_stat_ring_drops;
    stats->underruns = s_stat_underruns;
    stats->write_retries = s_stat_write_retries;
    stats->rx_overflows = s_stat_rx_overflows;
    stats->ring_samples = ring_count();
    stats->tx_frames = s_tx_frames;
    return ESP_OK;
}
