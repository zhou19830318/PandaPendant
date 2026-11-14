#include "app_core.h"
#include "app_settings.h"
#include "panda_audio.h"
#include "panda_connectivity.h"
#include "voice_resampler.h"
#include "o2_protocol.h"

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
#define O2_WSS_URI "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"
#define O2_RESOURCE_ID_DEFAULT "volc.speech.dialog"
/* Public app key required by the O2 endpoint (same value as the vendor demo). */
#define O2_APP_KEY_DEFAULT "PlgvMymc7f3tQnJ6"
#define O2_MODEL "1.2.1.1"
#define O2_RX_CAP (128 * 1024)
#define O2_INFLATE_CAP (64 * 1024)
#define DEFAULT_VOICE "zh_female_xiaohe_uranus_bigtts"
#define FRAME_SAMPLES 640
#define WIRE_SAMPLES 320
/* O2 TTS arrives much faster than realtime (a 43 s reply burst in ~10 s),
 * so the ring must hold a whole long reply: 768K samples @16 kHz ≈ 48 s. */
#define RING_SAMPLES (768 * 1024)
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
 * what separates speech from clicks and hiss. 250 RMS / 320 ms keeps fan and
 * handling noise out while still catching a normally spoken first word. */
#define PTT_SPEECH_RMS 250
#define PTT_SPEECH_FRAMES 8
/* Pressing PTT pops the mic: a 150 ms tap peaked at ~9500 and looked like
 * speech. Drop the first 120 ms of every PTT turn (stats and uplink). */
#define PTT_POP_GUARD_US 120000
#define NO_SPEECH_HINT "我在呢～没听到你说话，再说一次好吗？"
/* A silent PTT turn used to show a subtitle only, which felt like a dead
 * device. Send a text turn through the same realtime session so the server
 * synthesizes an audible, in-character reminder. */
#define NO_SPEECH_PROMPT \
    "我不小心碰到你了，没来得及说话。你轻轻提醒我一下：想聊天时要按住屏幕说话哦。"
#define SILENT_GRACE_MS 8000
#define NO_SPEECH_PROMPT_COOLDOWN_MS 3000
#define NO_SPEECH_PROMPT_TIMEOUT_MS 10000
/* A response.canceled/done arriving just after the prompt was sent belongs
 * to the canceled silence response, not to the prompt. */
#define NO_SPEECH_PROMPT_ARM_MS 400
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
    uint8_t binary;   /* O2 framing is sent as a binary websocket message */
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
static char s_headers[800];
static char s_session_id[40];
/* O2.0 binary transport state. The bytes are reassembled here because the
 * websocket client hands us 4 KB fragments; one server frame can span many. */
static bool s_o2_mode;
static bool s_o2_conn_ready;
static bool s_o2_restart_pending;
static bool s_o2_prompt_pending;
static uint8_t *s_o2_rx;
static size_t s_o2_rx_len;
static uint8_t *s_o2_payload;
/* 0x0 continuation fragments carry no type; remember the first fragment's. */
static bool s_ws_binary_msg;
static uint32_t s_event_seq = 1;
static uint32_t s_audio_deltas;
static uint32_t s_audio_samples;
static uint32_t s_tx_frames;
static bool s_reply_started;
static bool s_reply_done;
static bool s_asr_done;
static uint32_t s_asr_deltas;
static volatile bool s_clip_active;
static bool s_clip_pending;
static esp_err_t start_test_clip(void);
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
/* 32-bit millisecond deadlines: callers are different tasks (app_core and
 * the websocket event task), and a 64-bit read is not atomic on the ESP32. */
static volatile uint32_t s_silent_until_ms;
static volatile uint32_t s_prompt_sent_ms;
static volatile bool s_prompt_turn;
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

static void append_text(char *dst, size_t cap, const char *src);

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
    out[0] = '\0';
    append_text(out, cap, styles[p]);
    append_text(out, cap, feedbacks[f]);
    append_text(out, cap, cfg->persona);
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

static esp_err_t ws_send_bin_now(const uint8_t *buf, size_t len)
{
    if (!s_ws || !s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(s_ws, (const char *)buf, (int)len,
                                             pdMS_TO_TICKS(TX_LOCK_WAIT_MS));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

/* Producers never touch the ws client lock: enqueue a copy and let the TX
 * task drain it. Returns TIMEOUT when the queue is full (dropped frame). */
static esp_err_t tx_enqueue_ex(const char *buf, size_t len, bool critical, bool binary)
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
    frame.binary = binary;
    memcpy(frame.data, buf, len);
    return xQueueSend(s_tx_q, &frame, pdMS_TO_TICKS(10)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t tx_enqueue(const char *buf, size_t len, bool critical)
{
    return tx_enqueue_ex(buf, len, critical, false);
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
            sent = frame.binary
                       ? esp_websocket_client_send_bin(s_ws, frame.data, frame.len,
                                                       pdMS_TO_TICKS(TX_LOCK_WAIT_MS))
                       : esp_websocket_client_send_text(s_ws, frame.data, frame.len,
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

static esp_err_t o2_send_start_connection(void)
{
    size_t len = 0;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t err = o2_build_start_connection((uint8_t *)s_tx, TX_CAP, &len);
    if (err == ESP_OK) {
        err = ws_send_bin_now((const uint8_t *)s_tx, len);
    }
    xSemaphoreGive(s_tx_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "O2 StartConnection failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "O2 StartConnection sent (%u bytes)", (unsigned)len);
    }
    return err;
}

static esp_err_t send_session_create_o2(void)
{
    if (!s_o2_conn_ready) {
        ESP_LOGW(TAG, "O2 connection not ready; StartSession deferred");
        return ESP_ERR_INVALID_STATE;
    }
    const panda_settings_t *cfg = panda_settings_get();
    /* Called from the 8KB websocket task; keep the big JSON buffers off-stack. */
    enum { PLAIN_CAP = 800, PERSONA_CAP = 2200, CONFIG_CAP = 4096 };
    char *persona_plain = heap_caps_malloc(PLAIN_CAP, MALLOC_CAP_SPIRAM);
    char *persona = heap_caps_malloc(PERSONA_CAP, MALLOC_CAP_SPIRAM);
    char *config = heap_caps_malloc(CONFIG_CAP, MALLOC_CAP_SPIRAM);
    if (!persona_plain || !persona || !config) {
        ESP_LOGE(TAG, "O2 session buffers alloc failed");
        free(persona_plain);
        free(persona);
        free(config);
        return ESP_ERR_NO_MEM;
    }
    build_persona(persona_plain, PLAIN_CAP);
    json_escape(persona, PERSONA_CAP, persona_plain);
    const char *voice = cfg->speaker[0] ? cfg->speaker : DEFAULT_VOICE;

    int n = snprintf(config, CONFIG_CAP,
                     "{\"asr\":{\"extra\":{\"end_smooth_window_ms\":1500}},"
                     "\"tts\":{\"speaker\":\"%s\",\"audio_config\":{"
                     "\"channel\":1,\"format\":\"pcm_s16le\",\"sample_rate\":24000}},"
                     "\"dialog\":{\"bot_name\":\"豆包\",\"system_role\":\"%s\","
                     "\"extra\":{\"input_mod\":\"audio\","
                     "\"enable_loudness_norm\":true,\"recv_timeout\":30,"
                     "\"model\":\"%s\"}}}",
                     voice, persona, O2_MODEL);
    if (n <= 0 || (size_t)n >= CONFIG_CAP) {
        ESP_LOGE(TAG, "O2 session config too long (%d)", n);
        free(persona_plain);
        free(persona);
        free(config);
        return ESP_ERR_NO_MEM;
    }

    gen_uuid(s_session_id);
    size_t frame_len = 0;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t err = o2_build_start_session((uint8_t *)s_tx, TX_CAP, &frame_len,
                                           s_session_id, config);
    if (err == ESP_OK) {
        err = ws_send_bin_now((const uint8_t *)s_tx, frame_len);
    }
    if (err == ESP_OK) {
        s_session_pending = true;
        ESP_LOGI(TAG, "O2 StartSession sent (%u bytes, config %d)", (unsigned)frame_len, n);
    } else {
        ESP_LOGE(TAG, "O2 StartSession failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_tx_lock);
    free(persona_plain);
    free(persona);
    free(config);
    return err;
}

static esp_err_t send_session_create(void)
{
    if (s_o2_mode) {
        return send_session_create_o2();
    }
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
    if (s_o2_mode) {
        if (!s_session_active) {
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        size_t len = 0;
        esp_err_t err = o2_build_audio((uint8_t *)s_tx, TX_CAP, &len, s_session_id,
                                       pcm, samples);
        if (err == ESP_OK) {
            err = tx_enqueue_ex(s_tx, len, false, true);
        }
        xSemaphoreGive(s_tx_lock);
        return err;
    }
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
    if (s_o2_mode) {
        /* The O2 server owns end-of-speech detection; explicit commits are
         * not part of the binary protocol. */
        return ESP_OK;
    }
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
    if (s_o2_mode) {
        if (!s_session_active || !s_session_id[0]) {
            /* FinishSession without a session id is rejected by the server;
             * there is nothing to close before StartSession completes. */
            return ESP_ERR_INVALID_STATE;
        }
        size_t len = 0;
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        esp_err_t err = o2_build_finish_session((uint8_t *)s_tx, TX_CAP, &len,
                                                s_session_id);
        if (err == ESP_OK) {
            err = tx_enqueue_ex(s_tx, len, true, true);
        }
        xSemaphoreGive(s_tx_lock);
        s_session_active = false;
        s_session_pending = false;
        ESP_LOGI(TAG, "O2 FinishSession queued (%s)", esp_err_to_name(err));
        return err;
    }
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool within_silent_grace(void)
{
    return (int32_t)(s_silent_until_ms - now_ms()) > 0;
}

/* The server answers a committed silence turn with a canned apology such as
 * “服务器开小差了，请稍后再试”. Treat that as a silent-turn artifact, not as
 * a device failure that deserves the error screen. */
static bool is_server_fallback_text(const char *text)
{
    static const char *const patterns[] = {
        "服务器开小差", "服务开小差", "系统开小差",
        "服务器繁忙", "系统繁忙", "服务繁忙", "请稍后再试",
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(text, patterns[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void cancel_active_response(const char *why)
{
    if (!s_ws_connected) {
        return;
    }
    s_cancel_pending = true;
    s_cancel_play = true;
    s_play_drain = false;
    s_play_active = false;
    s_audio_started = false;
    resample_reset();
    send_cancel();
    ESP_LOGI(TAG, "response canceled (%s)", why);
}

/* A text turn on the same realtime session. The vendor demo's `say` command
 * uses speech_text_buffer.append/commit with a speech_id; the replacement
 * variant only intervenes in a response that is already generating, so a
 * standalone reminder sent that way was ignored and left the UI in THINKING
 * until the state-machine timeout painted an error screen. */
static esp_err_t send_text_turn(const char *text)
{
    if (!s_ws_connected || !s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_o2_mode) {
        size_t len = 0;
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        esp_err_t err = o2_build_text_query((uint8_t *)s_tx, TX_CAP, &len,
                                            s_session_id, text);
        if (err == ESP_OK) {
            err = tx_enqueue_ex(s_tx, len, true, true);
        }
        xSemaphoreGive(s_tx_lock);
        return err;
    }
    char esc[512];
    char frame[768];
    char speech_id[40];
    json_escape(esc, sizeof(esc), text);
    snprintf(speech_id, sizeof(speech_id), "panda-%08" PRIx32 "-%08" PRIx32,
             esp_random(), esp_random());

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = snprintf(frame, sizeof(frame),
                     "{\"event_id\":\"event_%u\",\"type\":\"speech_text_buffer."
                     "append\",\"speech_id\":\"%s\",\"text\":\"%s\"}",
                     (unsigned)s_event_seq++, speech_id, esc);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (n > 0 && (size_t)n < sizeof(frame)) {
        err = tx_enqueue(frame, (size_t)n, true);
    }
    if (err == ESP_OK) {
        n = snprintf(frame, sizeof(frame),
                     "{\"event_id\":\"event_%u\",\"type\":\"speech_text_buffer."
                     "commit\",\"speech_id\":\"%s\",\"text\":\"\"}",
                     (unsigned)s_event_seq++, speech_id);
        err = (n > 0 && (size_t)n < sizeof(frame)) ? tx_enqueue(frame, (size_t)n, true)
                                                   : ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_tx_lock);
    return err;
}

static bool prompt_no_speech(const char *why)
{
    uint32_t now = now_ms();
    if (s_prompt_turn &&
        (int32_t)(now - s_prompt_sent_ms) > NO_SPEECH_PROMPT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "no-speech prompt timed out; re-arming");
        s_prompt_turn = false;
    }
    if (s_prompt_turn) {
        return false;
    }
    if ((int32_t)(now - s_prompt_sent_ms) < NO_SPEECH_PROMPT_COOLDOWN_MS) {
        return false;
    }
    if (!s_ws_connected || !s_session_active) {
        /* O2 has no response.cancel: a canceled turn closes the session, so
         * the reminder waits for a fresh StartSession instead of being lost. */
        if (s_o2_mode && s_ws_connected && s_o2_conn_ready && !s_session_pending) {
            send_session_create();
            s_o2_prompt_pending = true;
            s_prompt_turn = true;
            s_prompt_sent_ms = now;
            s_silent_until_ms = now + SILENT_GRACE_MS;
            voice_state(PANDA_VOICE_THINKING);
            ESP_LOGI(TAG, "no-speech prompt queued for new O2 session (%s)", why);
            return true;
        }
        ESP_LOGI(TAG, "no-speech prompt skipped (%s)", why);
        return false;
    }
    esp_err_t err = send_text_turn(NO_SPEECH_PROMPT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no-speech prompt failed: %s", esp_err_to_name(err));
        return false;
    }
    s_prompt_turn = true;
    s_prompt_sent_ms = now;
    s_silent_until_ms = now + SILENT_GRACE_MS;
    s_no_speech_turn = false;
    s_asr_text[0] = '\0';
    s_asr_done = true;
    s_reply_text[0] = '\0';
    s_reply_started = false;
    s_reply_done = false;
    voice_state(PANDA_VOICE_THINKING);
    ESP_LOGI(TAG, "no-speech prompt sent (%s)", why);
    return true;
}

static void answer_no_speech(const char *why, bool cancel)
{
    s_silent_until_ms = now_ms() + SILENT_GRACE_MS;
    if (cancel) {
        cancel_active_response(why);
    }
    ESP_LOGI(TAG, "no speech (%s); local hint", why);
    app_core_on_subtitle(NO_SPEECH_HINT);
    voice_state(PANDA_VOICE_IDLE);
    prompt_no_speech(why);
}

static void prompt_response_finished(void)
{
    if (s_prompt_turn &&
        (int32_t)(now_ms() - s_prompt_sent_ms) > NO_SPEECH_PROMPT_ARM_MS) {
        s_prompt_turn = false;
    }
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
        if (s_clip_pending) {
            s_clip_pending = false;
            start_test_clip();
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
             * server response to silence is noise. Cancel whatever the
             * server is already generating, then answer audibly instead. */
            s_no_speech_turn = true;
            answer_no_speech("empty transcript", true);
        } else {
            app_core_on_subtitle(s_asr_text);
            voice_state(PANDA_VOICE_THINKING);
            ESP_LOGI(TAG, "asr: %s", s_asr_text);
        }
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.failed")) {
        if (s_no_speech_turn || (!s_prompt_turn && within_silent_grace())) {
            ESP_LOGI(TAG, "asr failed on a silent turn (muted)");
            s_no_speech_turn = true;
            answer_no_speech("asr failed on silent turn", true);
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
        /* Silence commits make the LLM answer with a canned apology. Drop it
         * instead of showing/spreaking a fake device failure. */
        if (!s_prompt_turn && within_silent_grace() && is_server_fallback_text(text)) {
            ESP_LOGW(TAG, "server fallback reply suppressed: %s", text);
            s_no_speech_turn = true;
            answer_no_speech("server fallback reply", true);
            cJSON_Delete(root);
            return;
        }
        if (text[0]) {
            snprintf(s_reply_text, sizeof(s_reply_text), "%s", text);
        }
        s_reply_done = true;
        app_core_on_subtitle(s_reply_text);
        ESP_LOGI(TAG, "reply(%u bytes): %s", (unsigned)strlen(s_reply_text), s_reply_text);
    } else if (!strcmp(type, "response.output_audio.started")) {
        if (s_no_speech_turn || (!s_prompt_turn && within_silent_grace())) {
            /* The server generated speech from silence; drop it and let the
             * text prompt produce the audible reminder instead. */
            s_no_speech_turn = true;
            answer_no_speech("tts started on silent turn", true);
            cJSON_Delete(root);
            return;
        }
        s_cancel_pending = false;
        s_cancel_play = false;
        s_audio_started = true;
        s_play_drain = false;
        s_play_active = false;
        s_audio_deltas = 0;
        s_audio_samples = 0;
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
            s_audio_samples += (uint32_t)(olen / 2);
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
        ESP_LOGI(TAG, "tts done deltas=%u ring=%u audio=%ums", (unsigned)s_audio_deltas,
                 (unsigned)ring_count(), (unsigned)(s_audio_samples * 1000u / 24000u));
    } else if (!strcmp(type, "response.done")) {
        prompt_response_finished();
        s_audio_started = false;
        s_play_drain = true;
        s_cancel_pending = false;
        s_reply_started = false;
        s_reply_done = false;
        s_no_speech_turn = false;
    } else if (!strcmp(type, "response.canceled")) {
        prompt_response_finished();
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
        if (s_no_speech_turn || (!s_prompt_turn && within_silent_grace())) {
            /* Empty/silent audio can make the server answer with an error;
             * that is expected for an accidental touch, not a device fault. */
            ESP_LOGI(TAG, "server error muted (silent turn): %s %s", code, msg);
            s_no_speech_turn = true;
            s_session_pending = false;
            answer_no_speech("server error on silent turn", true);
            cJSON_Delete(root);
            return;
        }
        if (s_prompt_turn) {
            /* A failure while speaking the reminder is still not worth an
             * error screen; drop back to idle and let the next turn retry. */
            ESP_LOGW(TAG, "no-speech prompt failed: %s %s", code, msg);
            s_prompt_turn = false;
            s_session_pending = false;
            voice_state(PANDA_VOICE_IDLE);
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

static const uint8_t *o2_decode(const o2_frame_t *f, size_t *len)
{
    if (f->compression == O2_COMP_GZIP) {
        if (!s_o2_payload) {
            return NULL;
        }
        int n = o2_inflate(f->payload, f->payload_len, s_o2_payload, O2_INFLATE_CAP - 1);
        if (n < 0) {
            return NULL;
        }
        s_o2_payload[n] = 0;
        *len = (size_t)n;
        return s_o2_payload;
    }
    *len = f->payload_len;
    return f->payload;
}

static cJSON *o2_json(const o2_frame_t *f)
{
    if (f->serialization != O2_SERIAL_JSON || f->payload_len == 0) {
        return NULL;
    }
    size_t len = 0;
    const uint8_t *p = o2_decode(f, &len);
    return p ? cJSON_ParseWithLength((const char *)p, len) : NULL;
}

static void o2_on_asr_info(void)
{
    if (s_no_speech_turn) {
        return;
    }
    s_asr_text[0] = '\0';
    s_asr_done = false;
    s_asr_deltas = 0;
    if (s_audio_started || s_play_active || ring_count() > 0) {
        /* User barged in: drop the rest of the current reply locally. The
         * server stops its own response on the new utterance. */
        s_cancel_pending = true;
        s_cancel_play = true;
        s_play_drain = false;
        s_play_active = false;
        s_audio_started = false;
        resample_reset();
    }
    voice_state(PANDA_VOICE_LISTENING);
}

static void o2_on_asr_response(const char *text)
{
    if (s_no_speech_turn || s_asr_done || !text[0] || !strcmp(text, s_asr_text)) {
        return;
    }
    size_t prev = strlen(s_asr_text);
    const char *delta = text;
    if (prev && strncmp(text, s_asr_text, prev) == 0) {
        delta = text + prev;
    }
    snprintf(s_asr_text, sizeof(s_asr_text), "%s", text);
    if (delta[0] && ++s_asr_deltas <= 12) {
        ESP_LOGI(TAG, "asr delta #%u '%s'", (unsigned)s_asr_deltas, delta);
    }
}

static void o2_on_asr_ended(void)
{
    if (s_no_speech_turn) {
        ESP_LOGI(TAG, "asr ended (muted: no-speech turn)");
        return;
    }
    s_asr_done = true;
    if (s_asr_text[0] == '\0') {
        s_no_speech_turn = true;
        answer_no_speech("empty transcript", true);
    } else {
        app_core_on_subtitle(s_asr_text);
        voice_state(PANDA_VOICE_THINKING);
        ESP_LOGI(TAG, "asr: %s", s_asr_text);
    }
}

static void o2_on_audio_started(cJSON *root)
{
    if (s_no_speech_turn || (!s_prompt_turn && within_silent_grace())) {
        s_no_speech_turn = true;
        answer_no_speech("tts started on silent turn", true);
        return;
    }
    s_cancel_pending = false;
    s_cancel_play = false;
    s_audio_started = true;
    s_play_drain = false;
    s_play_active = false;
    s_audio_deltas = 0;
    s_audio_samples = 0;
    ring_reset();
    resample_reset();
    voice_state(PANDA_VOICE_SPEAKING);
    cJSON *tts_type = root ? cJSON_GetObjectItemCaseSensitive(root, "tts_type") : NULL;
    ESP_LOGI(TAG, "tts started voice=%s",
             (tts_type && cJSON_IsString(tts_type)) ? tts_type->valuestring : "o2");
}

static void o2_on_audio(const o2_frame_t *f)
{
    if (s_cancel_pending || s_no_speech_turn) {
        return;
    }
    size_t len = 0;
    const uint8_t *p = o2_decode(f, &len);
    if (!p || len < 2) {
        return;
    }
    if (((uintptr_t)p & 1) != 0) {
        ESP_LOGW(TAG, "o2 audio misaligned, dropped");
        return;
    }
    size_t out = voice_resampler_process(&s_rs, (const int16_t *)p, len / 2,
                                         s_pcm16, PCM16_CAP / sizeof(int16_t));
    ring_push(s_pcm16, out);
    s_audio_samples += (uint32_t)(len / 2);
    s_audio_deltas++;
    s_audio_started = true;
    if ((s_audio_deltas % 25) == 0) {
        ESP_LOGI(TAG, "tts deltas=%u ring=%u", (unsigned)s_audio_deltas,
                 (unsigned)ring_count());
    }
}

static void o2_on_audio_done(cJSON *root)
{
    int status = 0;
    cJSON *sc = root ? cJSON_GetObjectItemCaseSensitive(root, "status_code") : NULL;
    if (sc && cJSON_IsNumber(sc)) {
        status = sc->valueint;
    } else if (sc && cJSON_IsString(sc)) {
        status = atoi(sc->valuestring);
    }
    s_audio_started = false;
    s_play_drain = true;
    prompt_response_finished();
    ESP_LOGI(TAG, "tts done deltas=%u ring=%u audio=%ums status=%d",
             (unsigned)s_audio_deltas, (unsigned)ring_count(),
             (unsigned)(s_audio_samples * 1000u / 24000u), status);
}

static void o2_on_chat_response(const char *content)
{
    if (s_no_speech_turn || !content[0]) {
        return;
    }
    if (!s_reply_started) {
        s_reply_text[0] = '\0';
        s_reply_started = true;
    }
    merge_text(s_reply_text, sizeof(s_reply_text), content);
    app_core_on_subtitle(s_reply_text);
    ESP_LOGI(TAG, "reply(%u bytes): %s", (unsigned)strlen(s_reply_text), s_reply_text);
}

static void o2_on_chat_ended(void)
{
    s_reply_done = true;
    prompt_response_finished();
    if (s_reply_text[0]) {
        app_core_on_subtitle(s_reply_text);
    }
    ESP_LOGI(TAG, "chat ended reply(%u bytes)", (unsigned)strlen(s_reply_text));
}

static void o2_on_session_started(void)
{
    s_session_active = true;
    s_session_pending = false;
    ESP_LOGI(TAG, "O2 session started id=%.36s", s_session_id);
    if (s_pending_capture) {
        s_pending_capture = false;
        s_capture = true;
        voice_state(PANDA_VOICE_LISTENING);
    } else {
        voice_state(PANDA_VOICE_IDLE);
    }
    if (s_clip_pending) {
        s_clip_pending = false;
        start_test_clip();
    }
    if (s_o2_prompt_pending) {
        s_o2_prompt_pending = false;
        if (send_text_turn(NO_SPEECH_PROMPT) == ESP_OK) {
            ESP_LOGI(TAG, "no-speech prompt sent on new O2 session");
        } else {
            ESP_LOGW(TAG, "no-speech prompt send failed after session start");
            s_prompt_turn = false;
        }
    }
}

static void o2_on_session_finished(void)
{
    s_session_active = false;
    s_session_pending = false;
    s_capture = false;
    ESP_LOGI(TAG, "O2 session finished");
    if (s_o2_restart_pending) {
        s_o2_restart_pending = false;
        if (send_session_create() != ESP_OK) {
            s_pending_capture = false;
            voice_state(PANDA_VOICE_IDLE);
        }
    } else {
        voice_state(PANDA_VOICE_IDLE);
    }
}

static void o2_on_error(uint32_t code, const char *msg)
{
    if (s_no_speech_turn || (!s_prompt_turn && within_silent_grace())) {
        ESP_LOGI(TAG, "O2 error muted (silent turn): %" PRIu32 " %s", code, msg);
        s_no_speech_turn = true;
        s_session_pending = false;
        answer_no_speech("server error on silent turn", true);
        return;
    }
    if (s_prompt_turn) {
        ESP_LOGW(TAG, "no-speech prompt failed: %" PRIu32 " %s", code, msg);
        s_prompt_turn = false;
        s_session_pending = false;
        voice_state(PANDA_VOICE_IDLE);
        return;
    }
    ESP_LOGE(TAG, "O2 error %" PRIu32 " %s", code, msg);
    voice_state(PANDA_VOICE_ERROR);
    panda_ui_toast(msg[0] ? msg : "语音服务错误", 2000);
    s_session_pending = false;
}

static void o2_handle_frame(const o2_frame_t *f)
{
    if (f->message_type == O2_MSG_SERVER_ERROR) {
        char msg[160];
        size_t n = f->payload_len < sizeof(msg) - 1 ? f->payload_len : sizeof(msg) - 1;
        memcpy(msg, f->payload, n);
        msg[n] = '\0';
        o2_on_error(f->event_id, msg);
        return;
    }
    if (f->message_type != O2_MSG_SERVER_FULL && f->message_type != O2_MSG_SERVER_ACK) {
        ESP_LOGD(TAG, "o2 frame type=%u event=%" PRIu32, f->message_type, f->event_id);
        return;
    }
    if (f->serialization == O2_SERIAL_NONE) {
        o2_on_audio(f);
        return;
    }

    ESP_LOGD(TAG, "o2 event %" PRIu32 " (%u bytes)", f->event_id,
             (unsigned)f->payload_len);
    cJSON *root = o2_json(f);
    switch (f->event_id) {
    case O2_EVT_CONN_STARTED:
        ESP_LOGI(TAG, "O2 connection started");
        s_o2_conn_ready = true;
        if (!s_session_active && !s_session_pending) {
            send_session_create();
        }
        break;
    case O2_EVT_CONN_FAILED:
        ESP_LOGW(TAG, "O2 connection failed");
        o2_on_error(O2_EVT_CONN_FAILED, "connection failed");
        break;
    case O2_EVT_CONN_FINISHED:
        ESP_LOGI(TAG, "O2 connection finished");
        s_o2_conn_ready = false;
        break;
    case O2_EVT_SESSION_STARTED:
        o2_on_session_started();
        break;
    case O2_EVT_SESSION_FINISHED:
        o2_on_session_finished();
        break;
    case O2_EVT_SESSION_FAILED:
        s_session_pending = false;
        o2_on_error(O2_EVT_SESSION_FAILED, "session failed");
        break;
    case O2_EVT_USAGE:
        ESP_LOGD(TAG, "O2 usage response");
        break;
    case O2_EVT_ASR_INFO:
        o2_on_asr_info();
        break;
    case O2_EVT_ASR_RESPONSE:
        if (root) {
            cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
            cJSON *first = (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0)
                               ? cJSON_GetArrayItem(results, 0)
                               : NULL;
            cJSON *text = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;
            if (text && cJSON_IsString(text)) {
                o2_on_asr_response(text->valuestring);
            }
        }
        break;
    case O2_EVT_ASR_ENDED:
        o2_on_asr_ended();
        break;
    case O2_EVT_TTS_SENTENCE_START:
        o2_on_audio_started(root);
        break;
    case O2_EVT_TTS_SENTENCE_END:
        ESP_LOGD(TAG, "O2 tts sentence end");
        break;
    case O2_EVT_TTS_RESPONSE:
        o2_on_audio(f);
        break;
    case O2_EVT_TTS_ENDED:
        o2_on_audio_done(root);
        break;
    case O2_EVT_CHAT_RESPONSE:
        if (root) {
            cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
            if (content && cJSON_IsString(content)) {
                o2_on_chat_response(content->valuestring);
            }
        }
        break;
    case O2_EVT_CHAT_QUERY_CONFIRMED:
        ESP_LOGD(TAG, "O2 text query confirmed");
        break;
    case O2_EVT_CHAT_ENDED:
        o2_on_chat_ended();
        break;
    default:
        ESP_LOGD(TAG, "o2 unhandled event %" PRIu32, f->event_id);
        break;
    }
    if (root) {
        cJSON_Delete(root);
    }
}

static void o2_feed(const uint8_t *data, size_t len)
{
    if (!s_o2_rx) {
        return;
    }
    if (len >= O2_RX_CAP) {
        s_stat_rx_overflows++;
        return;
    }
    if (s_o2_rx_len + len > O2_RX_CAP) {
        s_stat_rx_overflows++;
        size_t drop = s_o2_rx_len + len - O2_RX_CAP;
        if (drop >= s_o2_rx_len) {
            s_o2_rx_len = 0;
        } else {
            memmove(s_o2_rx, s_o2_rx + drop, s_o2_rx_len - drop);
            s_o2_rx_len -= drop;
        }
    }
    memcpy(s_o2_rx + s_o2_rx_len, data, len);
    s_o2_rx_len += len;

    while (s_o2_rx_len >= 4) {
        o2_frame_t f;
        size_t consumed = 0;
        esp_err_t err = o2_parse_frame(s_o2_rx, s_o2_rx_len, &f, &consumed);
        if (err == ESP_ERR_INVALID_SIZE) {
            break;
        }
        if (err != ESP_OK) {
            s_stat_rx_overflows++;
            ESP_LOGW(TAG, "o2 bad frame (err=%s, %u buffered), resync",
                     esp_err_to_name(err), (unsigned)s_o2_rx_len);
            s_o2_rx_len = 0;
            break;
        }
        o2_handle_frame(&f);
        memmove(s_o2_rx, s_o2_rx + consumed, s_o2_rx_len - consumed);
        s_o2_rx_len -= consumed;
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
        s_o2_rx_len = 0;
        s_o2_conn_ready = false;
        s_o2_restart_pending = false;
        s_ws_binary_msg = false;
        s_audio_started = false;
        s_play_drain = false;
        s_play_active = false;
        s_cancel_play = true;
        s_prompt_turn = false;
        s_silent_until_ms = 0;
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
        if (s_o2_mode) {
            o2_send_start_connection();
        } else {
            send_session_create();
        }
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
        s_clip_pending = false;
        s_capture = false;
        s_cancel_pending = false;
        s_o2_conn_ready = false;
        s_o2_rx_len = 0;
        s_o2_restart_pending = false;
        s_o2_prompt_pending = false;
        s_ws_binary_msg = false;
        s_audio_started = false;
        s_play_drain = false;
        s_play_active = false;
        s_cancel_play = true;
        s_no_speech_turn = false;
        s_prompt_turn = false;
        s_silent_until_ms = 0;
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
        if (!ev || ev->data_len <= 0) {
            break;
        }
        if (ev->op_code == 0x1 || ev->op_code == 0x2) {
            s_ws_binary_msg = (ev->op_code == 0x2);
        }
        if (s_ws_binary_msg) {
            if (s_o2_mode) {
                o2_feed((const uint8_t *)ev->data_ptr, (size_t)ev->data_len);
            }
        } else {
            /* 0x1 = text frame, 0x0 = continuation of a fragmented frame. The
             * old code only accepted 0x1, so large JSON events split by the
             * server were silently discarded. */
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
        if (s_prompt_turn &&
            (int32_t)(now_ms() - s_prompt_sent_ms) > NO_SPEECH_PROMPT_TIMEOUT_MS) {
            /* The audible reminder never produced a response. Drop back to
             * idle here instead of letting the state machine turn 20 s of
             * THINKING into a "服务开小差了" error screen. */
            ESP_LOGW(TAG, "no-speech prompt got no reply; back to idle");
            s_prompt_turn = false;
            s_no_speech_turn = false;
            voice_state(PANDA_VOICE_IDLE);
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
                if ((esp_timer_get_time() - s_ptt_press_us) < PTT_POP_GUARD_US) {
                    continue;
                }
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
    s_o2_mode = strcmp(cfg->api_mode, "o2") == 0;
    if (s_o2_mode) {
        if (!cfg->app_id[0] || !cfg->access_key[0]) {
            ESP_LOGW(TAG, "api_mode=o2 needs app_id and access_key; set them first");
            return ESP_ERR_INVALID_STATE;
        }
    } else if (!cfg->api_key[0]) {
        ESP_LOGW(TAG, "api_key empty; configure settings first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!panda_connectivity_is_connected()) {
        ESP_LOGW(TAG, "wifi not connected yet; websocket will retry");
    }

    /* A failed start may be retried when WiFi comes up; allocate only once so
     * retries do not leak PSRAM buffers or tasks. */
    if (s_ring == NULL) {
        s_ring = heap_caps_malloc(RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_rx = heap_caps_malloc(RX_CAP, MALLOC_CAP_SPIRAM);
        s_tx = heap_caps_malloc(TX_CAP, MALLOC_CAP_SPIRAM);
        s_pcm24 = heap_caps_malloc(PCM24_CAP, MALLOC_CAP_SPIRAM);
        s_pcm16 = heap_caps_malloc(PCM16_CAP, MALLOC_CAP_SPIRAM);
        if (s_o2_mode) {
            s_o2_rx = heap_caps_malloc(O2_RX_CAP, MALLOC_CAP_SPIRAM);
            s_o2_payload = heap_caps_malloc(O2_INFLATE_CAP, MALLOC_CAP_SPIRAM);
        }
        s_ring_lock = xSemaphoreCreateMutex();
        s_tx_lock = xSemaphoreCreateMutex();
        /* Static queue in PSRAM: 8 pending frames would otherwise cost ~8 KB
         * of internal RAM, which the TLS handshake and LVGL also need. */
        s_tx_q_storage = heap_caps_aligned_alloc(8, TX_QUEUE_DEPTH * sizeof(tx_frame_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_tx_q_storage) {
            s_tx_q = xQueueCreateStatic(TX_QUEUE_DEPTH, sizeof(tx_frame_t),
                                        s_tx_q_storage, &s_tx_q_struct);
        }
    }
    if (!s_ring || !s_rx || !s_tx || !s_pcm24 || !s_pcm16 || !s_ring_lock ||
        !s_tx_lock || !s_tx_q || (s_o2_mode && (!s_o2_rx || !s_o2_payload))) {
        ESP_LOGE(TAG, "PSRAM buffers unavailable");
        return ESP_ERR_NO_MEM;
    }
    ring_reset();
    resample_reset();
    s_rx_len = 0;
    s_rx_skip = false;
    s_o2_rx_len = 0;
    s_cancel_play = false;
    s_play_running = true;

    if (s_o2_mode) {
        char connect_id[48];
        snprintf(connect_id, sizeof(connect_id), "%08" PRIx32 "-%04x-%04x",
                 esp_random(), (unsigned)(esp_random() & 0xFFFF),
                 (unsigned)(esp_random() & 0xFFFF));
        snprintf(s_headers, sizeof(s_headers),
                 "X-Api-App-ID: %s\r\n"
                 "X-Api-Access-Key: %s\r\n"
                 "X-Api-Resource-Id: %s\r\n"
                 "X-Api-App-Key: %s\r\n"
                 "X-Api-Connect-Id: %s\r\n",
                 cfg->app_id, cfg->access_key,
                 cfg->resource_id[0] ? cfg->resource_id : O2_RESOURCE_ID_DEFAULT,
                 cfg->app_key[0] ? cfg->app_key : O2_APP_KEY_DEFAULT,
                 connect_id);
        ESP_LOGI(TAG, "O2 mode: app_id=%s resource=%s connect_id=%s",
                 panda_settings_masked(cfg->app_id),
                 cfg->resource_id[0] ? cfg->resource_id : O2_RESOURCE_ID_DEFAULT,
                 connect_id);
    } else {
        snprintf(s_headers, sizeof(s_headers), "X-Api-Key: %s\r\n", cfg->api_key);
    }
    esp_websocket_client_config_t ws_cfg = {
        .uri = s_o2_mode ? O2_WSS_URI : VOICE_WSS_URI,
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
    /* Start the audio tasks only once the 8 KB websocket stack has been
     * granted: creating them first left too little contiguous internal RAM
     * for the client on a fragmented boot heap. */
    if (s_play_task == NULL) {
        xTaskCreate(play_task, "voice_play", 4096, NULL, 10, &s_play_task);
    }
    if (s_cap_task == NULL) {
        xTaskCreate(capture_task, "voice_cap", 4096, NULL, 9, &s_cap_task);
    }
    if (s_tx_task == NULL) {
        xTaskCreate(tx_task, "voice_tx", 4096, NULL, 8, &s_tx_task);
    }
    ESP_LOGI(TAG, "voice client started (%s mode, model=%s)",
             s_o2_mode ? "O2" : "JSON", s_o2_mode ? O2_MODEL : cfg->model);
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

static esp_err_t start_test_clip(void)
{
    if (!s_clip_pcm && !load_test_clip()) {
        return ESP_ERR_NOT_FOUND;
    }
    s_clip_pos = 0;
    s_pending_capture = false;
    s_no_speech_turn = false;
    s_prompt_turn = false;
    s_silent_until_ms = 0;
    s_clip_active = true;
    s_capture = true;
    voice_state(PANDA_VOICE_LISTENING);
    ESP_LOGI(TAG, "test clip: %u samples", (unsigned)s_clip_samples);
    return ESP_OK;
}

esp_err_t app_voice_send_test_clip(void)
{
    if (!s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_session_active) {
        if (!s_session_pending) {
            esp_err_t err = send_session_create();
            if (err != ESP_OK) {
                return err;
            }
        }
        s_clip_pending = true;
        ESP_LOGI(TAG, "test clip queued until the session is ready");
        return ESP_OK;
    }
    return start_test_clip();
}

void app_voice_ptt_press(void)
{
    s_ptt_press_us = esp_timer_get_time();
    s_ptt_peak = 0;
    s_ptt_rms_max = 0;
    s_ptt_speech_frames = 0;
    s_no_speech_turn = false;
    s_prompt_turn = false;
    s_silent_until_ms = 0;
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
    s_clip_pending = false;
    if (s_audio_started || ring_count() > 0 || s_play_active) {
        /* Barge-in: let the play task ramp down and flush instead of cutting
         * the waveform at an arbitrary sample (an audible click). */
        s_cancel_pending = true;
        s_cancel_play = true;
        resample_reset();
        send_cancel();
        if (s_o2_mode) {
            /* O2 interrupt == FinishSession; the new session is opened when
             * the server acks 152, then capture resumes in LISTENING. */
            s_o2_restart_pending = true;
            s_pending_capture = true;
            s_capture = false;
            s_asr_text[0] = '\0';
            s_reply_text[0] = '\0';
            s_reply_started = false;
            s_reply_done = false;
            s_asr_done = false;
            s_asr_deltas = 0;
            voice_state(PANDA_VOICE_CONNECTING);
            return;
        }
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
        /* Accidental touch or silent hold. Do NOT commit the silent audio:
         * the server turns that into an ASR failure / “服务器开小差” reply,
         * which is the error the user saw after an empty voice turn. Keep
         * the turn local and ask the server for an audible reminder. */
        s_no_speech_turn = true;
        answer_no_speech("no sustained speech", false);
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
    s_clip_pending = false;
    s_cancel_pending = true;
    s_cancel_play = true;
    resample_reset();
    s_play_drain = false;
    s_play_active = false;
    s_audio_started = false;
    s_asr_done = true;
    s_reply_done = true;
    s_no_speech_turn = false;
    s_prompt_turn = false;
    s_silent_until_ms = 0;
    s_o2_restart_pending = false;
    s_o2_prompt_pending = false;
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
        /* The boot-time start can fail while the internal heap is still
         * fragmented; retry once the network is actually up, otherwise the
         * UI stays on the urgent offline screen forever. */
        if (online) {
            esp_err_t err = app_voice_start();
            ESP_LOGI(TAG, "voice client start on network up: %s", esp_err_to_name(err));
        }
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
    s_prompt_turn = false;
    s_silent_until_ms = 0;
    s_o2_conn_ready = false;
    s_o2_rx_len = 0;
    s_o2_restart_pending = false;
    s_o2_prompt_pending = false;
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
