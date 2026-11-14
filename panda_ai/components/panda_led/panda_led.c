#include "panda_led.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "panda_audio.h"
#include "panda_board.h"

static const char *TAG = "led";

#define LED_COUNT 7
#define LED_TICK_ACTIVE_MS 40
#define LED_TICK_IDLE_MS 120
#define LED_TICK_SLOW_MS 300
#define LED_TICK_SLEEP_MS 1000

#define DEFAULT_BRIGHTNESS 45
/* Sum of all 21 channel bytes. WS2812 draws ~20 mA per channel at 255, so this
 * caps the ring near 110 mA even when everything is white. */
#define POWER_BUDGET 1100

#define EVENT_VOLUME_MS 320
#define EVENT_WAKE_MS 260
#define EVENT_NET_MS 1300

static volatile panda_led_mode_t s_mode = PANDA_LED_MODE_FULL;
static volatile int s_brightness = DEFAULT_BRIGHTNESS;
static volatile panda_led_voice_t s_voice = PANDA_LED_VOICE_IDLE;
static volatile panda_led_play_t s_play = PANDA_LED_PLAY_IDLE;
static volatile int s_track;
static volatile bool s_net_connected;
static volatile bool s_net_ap;
static volatile int s_batt_pct = -1;
static volatile bool s_charging;
static volatile bool s_batt_present;
static volatile bool s_sleeping;
static volatile bool s_frozen;
static SemaphoreHandle_t s_out_lock;
static uint8_t s_manual[3] = {255, 255, 255};

static volatile uint32_t s_vol_ms;
static volatile int s_vol_dir;
static volatile int s_vol_pct;
static volatile uint32_t s_wake_ms;
static volatile uint32_t s_net_ms;

static int s_level;
static uint8_t s_last[LED_COUNT * 3];
static uint32_t s_refreshes;
static bool s_started;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static float breathe(uint32_t t, uint32_t period_ms)
{
    return 0.5f - 0.5f * cosf((float)(t % period_ms) * 6.2831853f / (float)period_ms);
}

static void fill(float px[LED_COUNT][3], float r, float g, float b, float amp)
{
    for (int i = 0; i < LED_COUNT; i++) {
        px[i][0] = r * amp;
        px[i][1] = g * amp;
        px[i][2] = b * amp;
    }
}

static void comet(float px[LED_COUNT][3], uint32_t t, uint32_t period_ms,
                  float r, float g, float b, float scale)
{
    float head = (float)(t % period_ms) / (float)period_ms * LED_COUNT;
    for (int i = 0; i < LED_COUNT; i++) {
        float d = fabsf((float)i - head);
        if (d > LED_COUNT / 2.0f) {
            d = LED_COUNT - d;
        }
        float k = 1.0f - d / 3.0f;
        if (k < 0.0f) {
            k = 0.0f;
        }
        px[i][0] = r * k * scale;
        px[i][1] = g * k * scale;
        px[i][2] = b * k * scale;
    }
}

static void hsv2rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float hh = (float)(h % 360) / 60.0f;
    float ss = (float)s / 255.0f;
    float vv = (float)v / 255.0f;
    int i = (int)hh;
    float f = hh - (float)i;
    float p = vv * (1.0f - ss);
    float q = vv * (1.0f - ss * f);
    float tv = vv * (1.0f - ss * (1.0f - f));
    float rf, gf, bf;
    switch (i % 6) {
    case 0: rf = vv; gf = tv; bf = p; break;
    case 1: rf = q; gf = vv; bf = p; break;
    case 2: rf = p; gf = vv; bf = tv; break;
    case 3: rf = p; gf = q; bf = vv; break;
    case 4: rf = tv; gf = p; bf = vv; break;
    default: rf = vv; gf = p; bf = q; break;
    }
    *r = (uint8_t)(rf * 255.0f + 0.5f);
    *g = (uint8_t)(gf * 255.0f + 0.5f);
    *b = (uint8_t)(bf * 255.0f + 0.5f);
}

/* Centre-out VU: 3,2,4,1,5,0,6 so the ring grows from the middle. */
static void vu(float px[LED_COUNT][3], uint8_t r, uint8_t g, uint8_t b, int level)
{
    static const uint8_t order[LED_COUNT] = {3, 2, 4, 1, 5, 0, 6};
    memset(px, 0, sizeof(float) * LED_COUNT * 3);
    /* Music peaks sit far below full scale on most tracks; a sqrt curve keeps
     * quiet passages visible and still leaves headroom for loud ones. */
    if (level > 0) {
        level = (int)(sqrtf((float)level / 100.0f) * 100.0f);
    }
    int lit = (level * LED_COUNT + 99) / 100;
    if (lit < 1) {
        lit = 1;
    }
    for (int k = 0; k < LED_COUNT; k++) {
        if (k >= lit) {
            continue;
        }
        float amp = 0.45f + 0.55f * (float)level / 100.0f;
        amp *= 1.0f - 0.3f * (float)k / (float)(LED_COUNT - 1);
        int i = order[k];
        px[i][0] = (float)r * amp;
        px[i][1] = (float)g * amp;
        px[i][2] = (float)b * amp;
    }
}

static void render_base(float px[LED_COUNT][3], uint32_t t)
{
    memset(px, 0, sizeof(float) * LED_COUNT * 3);

    if (s_mode == PANDA_LED_MODE_MANUAL) {
        fill(px, (float)s_manual[0], (float)s_manual[1], (float)s_manual[2], 1.0f);
        return;
    }

    switch (s_voice) {
    case PANDA_LED_VOICE_CONNECTING:
        comet(px, t, 1800, 255.0f, 170.0f, 50.0f, 0.55f);
        break;

    case PANDA_LED_VOICE_LISTENING:
        for (int i = 0; i < LED_COUNT; i++) {
            float ph = (float)(t % 1200) / 1200.0f - (float)i * 0.12f;
            float k = 0.5f + 0.5f * cosf(6.2831853f * ph);
            float amp = 0.20f + 0.50f * k * k;
            px[i][0] = 60.0f * amp;
            px[i][1] = 220.0f * amp;
            px[i][2] = 255.0f * amp;
        }
        break;

    case PANDA_LED_VOICE_THINKING:
        comet(px, t, 1200, 130.0f, 90.0f, 255.0f, 0.7f);
        break;

    case PANDA_LED_VOICE_SPEAKING:
        if (s_mode != PANDA_LED_MODE_STATE) {
            vu(px, 255, 225, 180, s_level);
        } else {
            fill(px, 255.0f, 225.0f, 180.0f, 0.25f + 0.25f * breathe(t, 900));
        }
        break;

    case PANDA_LED_VOICE_OFFLINE:
        fill(px, 255.0f, 150.0f, 40.0f, (t % 1600) < 220 ? 0.7f : 0.03f);
        break;

    case PANDA_LED_VOICE_ERROR: {
        uint32_t c = t % 1500;
        bool flash = c < 130 || (c > 240 && c < 370);
        fill(px, 255.0f, 40.0f, 30.0f, flash ? 0.9f : 0.04f);
        break;
    }

    case PANDA_LED_VOICE_IDLE:
    default:
        if (s_batt_present && s_batt_pct >= 0 && s_batt_pct <= 10 && !s_charging) {
            fill(px, 255.0f, 30.0f, 18.0f, 0.10f + 0.30f * breathe(t, 2600));
        } else if (s_charging) {
            int lit = (s_batt_pct + 24) / 25;
            if (lit < 1) {
                lit = 1;
            }
            if (lit > 4) {
                lit = 4;
            }
            float amp = 0.35f + 0.30f * breathe(t, 2200);
            for (int i = 0; i < lit; i++) {
                px[i][0] = 60.0f * amp;
                px[i][1] = 255.0f * amp;
                px[i][2] = 120.0f * amp;
            }
        } else if (s_net_ap) {
            fill(px, 70.0f, 130.0f, 255.0f, 0.22f + 0.40f * breathe(t, 1800));
        } else if (s_net_connected) {
            fill(px, 60.0f, 220.0f, 150.0f, 0.25f + 0.45f * breathe(t, 4600));
        } else {
            fill(px, 255.0f, 150.0f, 40.0f, 0.10f + 0.25f * breathe(t, 3200));
        }
        break;
    }
}

static void render_play(float px[LED_COUNT][3], uint32_t t)
{
    if (s_play == PANDA_LED_PLAY_IDLE) {
        return;
    }
    if (s_play == PANDA_LED_PLAY_PLAYING) {
        uint16_t hue = (uint16_t)((s_track * 47 + 30) % 360);
        uint8_t r, g, b;
        hsv2rgb(hue, 230, 255, &r, &g, &b);
        vu(px, r, g, b, s_level);
    } else {
        fill(px, 255.0f, 160.0f, 40.0f, 0.22f + 0.10f * breathe(t, 3000));
    }
}

static void overlay_max(float px[LED_COUNT][3], int i, float r, float g, float b)
{
    if (r > px[i][0]) px[i][0] = r;
    if (g > px[i][1]) px[i][1] = g;
    if (b > px[i][2]) px[i][2] = b;
}

static void render_events(float px[LED_COUNT][3], uint32_t t)
{
    uint32_t dt = t - s_vol_ms;
    if (dt < EVENT_VOLUME_MS) {
        float k = (1.0f - (float)dt / EVENT_VOLUME_MS) *
                  (0.25f + 0.75f * (float)s_vol_pct / 100.0f);
        int from = s_vol_dir > 0 ? 4 : 0;
        for (int i = from; i < from + 3; i++) {
            overlay_max(px, i, 120.0f * k, 255.0f * k, 170.0f * k);
        }
    }

    dt = t - s_wake_ms;
    if (dt < EVENT_WAKE_MS) {
        float k = 0.8f * (1.0f - (float)dt / EVENT_WAKE_MS);
        for (int i = 0; i < LED_COUNT; i++) {
            overlay_max(px, i, 255.0f * k, 255.0f * k, 255.0f * k);
        }
    }

    dt = t - s_net_ms;
    if (dt < EVENT_NET_MS) {
        float p = (float)dt / EVENT_NET_MS * (LED_COUNT + 1);
        float fade = 1.0f - (float)dt / EVENT_NET_MS;
        for (int i = 0; i < LED_COUNT; i++) {
            if ((float)i <= p) {
                overlay_max(px, i, 40.0f * fade, 255.0f * fade, 110.0f * fade);
            }
        }
    }
}

/* Brightness -> gamma -> global power budget, then push only if it changed. */
static void output_frame(const float px[LED_COUNT][3])
{
    uint8_t frame[LED_COUNT * 3];
    int brightness = s_brightness;
    int total = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        for (int c = 0; c < 3; c++) {
            float v = px[i][c] * (float)brightness / 100.0f;
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 255.0f) {
                v = 255.0f;
            }
            v = v * v / 255.0f;
            frame[i * 3 + c] = (uint8_t)(v + 0.5f);
            total += frame[i * 3 + c];
        }
    }
    if (total > POWER_BUDGET) {
        for (int i = 0; i < LED_COUNT * 3; i++) {
            frame[i] = (uint8_t)((int)frame[i] * POWER_BUDGET / total);
        }
    }

    if (memcmp(frame, s_last, sizeof(frame)) == 0) {
        return;
    }
    if (panda_board_rgb_write(frame, LED_COUNT) == ESP_OK) {
        memcpy(s_last, frame, sizeof(frame));
        s_refreshes++;
    }
}

static bool active_tick(uint32_t t)
{
    if (s_play == PANDA_LED_PLAY_PLAYING) {
        return true;
    }
    if (s_voice == PANDA_LED_VOICE_LISTENING || s_voice == PANDA_LED_VOICE_THINKING ||
        s_voice == PANDA_LED_VOICE_SPEAKING) {
        return true;
    }
    return (t - s_vol_ms) < EVENT_VOLUME_MS || (t - s_wake_ms) < EVENT_WAKE_MS ||
           (t - s_net_ms) < EVENT_NET_MS;
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t = now_ms();

        int level = panda_audio_get_output_level();
        if (level > s_level) {
            s_level = level;
        } else {
            s_level = (s_level * 7) / 8;
        }

        float px[LED_COUNT][3];
        if (s_sleeping || s_mode == PANDA_LED_MODE_OFF) {
            s_level = 0;
            memset(px, 0, sizeof(px));
        } else {
            render_base(px, t);
            if ((s_mode == PANDA_LED_MODE_AUDIO || s_mode == PANDA_LED_MODE_FULL) &&
                s_play != PANDA_LED_PLAY_IDLE) {
                render_play(px, t);
            }
            if (s_mode == PANDA_LED_MODE_FULL) {
                render_events(px, t);
            }
        }
        if (s_out_lock) {
            xSemaphoreTake(s_out_lock, portMAX_DELAY);
        }
        if (!s_frozen) {
            output_frame(px);
        }
        if (s_out_lock) {
            xSemaphoreGive(s_out_lock);
        }

        uint32_t period;
        if (s_sleeping || s_mode == PANDA_LED_MODE_OFF) {
            period = LED_TICK_SLEEP_MS;
        } else if (s_mode == PANDA_LED_MODE_MANUAL) {
            period = LED_TICK_SLOW_MS;
        } else if (active_tick(t)) {
            period = LED_TICK_ACTIVE_MS;
        } else {
            period = LED_TICK_IDLE_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(period));
    }
}

esp_err_t panda_led_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (!panda_board_rgb_ready() && panda_board_rgb_init() != ESP_OK) {
        ESP_LOGW(TAG, "no RGB ring, LEDs disabled");
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t t = now_ms();
    s_vol_ms = t - EVENT_VOLUME_MS;
    s_wake_ms = t - EVENT_WAKE_MS;
    s_net_ms = t - EVENT_NET_MS;
    if (s_out_lock == NULL) {
        s_out_lock = xSemaphoreCreateMutex();
        if (s_out_lock == NULL) {
            ESP_LOGW(TAG, "no output lock; deep sleep indicator may race");
        }
    }
    BaseType_t ok = xTaskCreatePinnedToCore(led_task, "panda_led", 3072, NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "LED ring ready (mode=%s brightness=%d%%)",
             panda_led_mode_name(s_mode), s_brightness);
    return ESP_OK;
}

void panda_led_set_mode(panda_led_mode_t mode)
{
    if (mode > PANDA_LED_MODE_MANUAL) {
        mode = PANDA_LED_MODE_MANUAL;
    }
    s_mode = mode;
}

panda_led_mode_t panda_led_get_mode(void)
{
    return s_mode;
}

void panda_led_set_brightness(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;
}

int panda_led_get_brightness(void)
{
    return s_brightness;
}

void panda_led_set_manual(uint8_t r, uint8_t g, uint8_t b)
{
    s_manual[0] = r;
    s_manual[1] = g;
    s_manual[2] = b;
    s_mode = PANDA_LED_MODE_MANUAL;
}

void panda_led_set_voice(panda_led_voice_t voice)
{
    s_voice = voice;
}

void panda_led_set_play(panda_led_play_t play, int track_index)
{
    s_play = play;
    if (track_index >= 0) {
        s_track = track_index;
    }
}

void panda_led_set_network(bool connected, bool ap)
{
    s_net_connected = connected;
    s_net_ap = ap;
}

void panda_led_set_battery(int percent, bool charging, bool present)
{
    s_batt_pct = percent;
    s_charging = charging;
    s_batt_present = present;
}

void panda_led_set_sleeping(bool sleeping)
{
    s_sleeping = sleeping;
}

esp_err_t panda_led_prepare_deep_sleep(void)
{
    s_frozen = true;
    /* Wait for a frame that is already on the wire, then no tick can write
     * again because every writer checks s_frozen under the same lock. */
    if (s_out_lock) {
        xSemaphoreTake(s_out_lock, portMAX_DELAY);
    }
    esp_err_t err = panda_board_rgb_sleep_indicator();
    if (s_out_lock) {
        xSemaphoreGive(s_out_lock);
    }
    return err;
}

void panda_led_notify_volume(int direction, int volume_percent)
{
    s_vol_dir = direction >= 0 ? 1 : -1;
    s_vol_pct = volume_percent;
    s_vol_ms = now_ms();
}

void panda_led_notify_wake(void)
{
    s_wake_ms = now_ms();
}

void panda_led_notify_network_connected(void)
{
    s_net_ms = now_ms();
}

const char *panda_led_mode_name(panda_led_mode_t mode)
{
    switch (mode) {
    case PANDA_LED_MODE_OFF: return "off";
    case PANDA_LED_MODE_STATE: return "state";
    case PANDA_LED_MODE_AUDIO: return "audio";
    case PANDA_LED_MODE_FULL: return "full";
    case PANDA_LED_MODE_MANUAL: return "manual";
    default: return "?";
    }
}

uint32_t panda_led_refresh_count(void)
{
    return s_refreshes;
}
