#include "app_weather.h"
#include "app_settings.h"
#include "panda_connectivity.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "weather";

#define WEATHER_BUF_CAP 4096
#define WEATHER_REFRESH_US (30LL * 60 * 1000000)
#define WEATHER_RETRY_US (60LL * 1000000)
#define WEATHER_OFFLINE_US (10LL * 1000000)
#define WEATHER_MIN_INTERNAL (16 * 1024)

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} http_ctx_t;

static app_weather_info_t s_info;
static SemaphoreHandle_t s_info_lock;
static volatile bool s_refresh_req;
static TaskHandle_t s_task;

static const char *wmo_condition(int code)
{
    switch (code) {
    case 0: return "晴";
    case 1: return "大部晴朗";
    case 2: return "多云";
    case 3: return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: case 56: case 57: return "毛毛雨";
    case 61: case 63: case 65: case 66: case 67: return "雨";
    case 71: case 73: case 75: case 77: return "雪";
    case 80: case 81: case 82: return "阵雨";
    case 85: case 86: return "阵雪";
    case 95: case 96: case 99: return "雷雨";
    default: return "未知";
    }
}

static panda_ui_weather_t wmo_icon(int code)
{
    if (code == 0 || code == 1) {
        return PANDA_UI_WEATHER_SUN;
    }
    if (code == 2) {
        return PANDA_UI_WEATHER_PARTLY;
    }
    return PANDA_UI_WEATHER_CLOUDY;
}

static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && out + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[out++] = (char)*p;
        } else {
            dst[out++] = '%';
            dst[out++] = hex[*p >> 4];
            dst[out++] = hex[*p & 0x0F];
        }
    }
    dst[out] = '\0';
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || ctx == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t room = ctx->cap - 1 - ctx->len;
    size_t copy = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    if (copy > 0) {
        memcpy(ctx->buf + ctx->len, evt->data, copy);
        ctx->len += copy;
    }
    if (copy < (size_t)evt->data_len) {
        ctx->overflow = true;
    }
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_get(const char *url, char *buf, size_t cap, size_t *out_len)
{
    http_ctx_t ctx = {.buf = buf, .cap = cap};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 9000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .event_handler = http_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GET status %d", status);
        return ESP_FAIL;
    }
    if (ctx.overflow) {
        ESP_LOGW(TAG, "response larger than %u bytes", (unsigned)(cap - 1));
        return ESP_ERR_INVALID_SIZE;
    }
    buf[ctx.len] = '\0';
    if (out_len) {
        *out_len = ctx.len;
    }
    return ESP_OK;
}

static bool parse_geocode(const char *json, double *lat, double *lon)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    bool ok = false;
    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    cJSON *first = cJSON_GetArrayItem(results, 0);
    cJSON *jlat = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    cJSON *jlon = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
        *lat = jlat->valuedouble;
        *lon = jlon->valuedouble;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static bool parse_current(const char *json, int *temp_c, int *code)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    bool ok = false;
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *jtemp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *jcode = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    if (cJSON_IsNumber(jtemp) && cJSON_IsNumber(jcode)) {
        *temp_c = (int)lroundf((float)jtemp->valuedouble);
        *code = jcode->valueint;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t weather_fetch(void)
{
    const panda_settings_t *cfg = panda_settings_get();
    if (cfg->weather_city[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal < WEATHER_MIN_INTERNAL) {
        ESP_LOGD(TAG, "skip fetch, internal heap %" PRIu32, internal);
        return ESP_ERR_NO_MEM;
    }

    char *buf = heap_caps_malloc(WEATHER_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char city[PANDA_CITY_MAX * 3 + 1];
    url_encode(cfg->weather_city, city, sizeof(city));
    char url[320];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=zh&format=json",
             city);

    esp_err_t err = http_get(url, buf, WEATHER_BUF_CAP, NULL);
    double lat = 0;
    double lon = 0;
    if (err != ESP_OK || !parse_geocode(buf, &lat, &lon)) {
        ESP_LOGW(TAG, "geocode '%s' failed", cfg->weather_city);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&timezone=auto",
             lat, lon);
    err = http_get(url, buf, WEATHER_BUF_CAP, NULL);
    int temp = 0;
    int code = -1;
    if (err != ESP_OK || !parse_current(buf, &temp, &code)) {
        ESP_LOGW(TAG, "forecast fetch failed");
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    heap_caps_free(buf);

    app_weather_info_t info = {
        .valid = true,
        .temp_c = temp,
        .weather_code = code,
        .icon = wmo_icon(code),
        .updated = time(NULL),
    };
    snprintf(info.city, sizeof(info.city), "%s", cfg->weather_city);
    snprintf(info.condition, sizeof(info.condition), "%s", wmo_condition(code));

    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    s_info = info;
    xSemaphoreGive(s_info_lock);

    panda_ui_set_weather(true, temp, info.icon);
    ESP_LOGI(TAG, "%s %d°C %s (code %d)", info.city, temp, info.condition, code);
    return ESP_OK;
}

static void weather_task(void *arg)
{
    (void)arg;
    int64_t next_due = 0;
    while (true) {
        if (s_refresh_req) {
            s_refresh_req = false;
            next_due = 0;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_due) {
            if (!panda_connectivity_is_connected()) {
                next_due = now + WEATHER_OFFLINE_US;
            } else {
                esp_err_t err = weather_fetch();
                if (err == ESP_FAIL) {
                    /* Real HTTPS failure while wifi claims to be up
                     * corroborates a wedged data path. Memory skips and an
                     * unset city are not network evidence. */
                    panda_connectivity_notify_net_error("weather");
                } else if (err == ESP_ERR_NO_MEM) {
                    ESP_LOGW(TAG, "fetch skipped: internal heap %" PRIu32,
                             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                }
                next_due = now + (err == ESP_OK ? WEATHER_REFRESH_US : WEATHER_RETRY_US);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t app_weather_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_info_lock == NULL) {
        s_info_lock = xSemaphoreCreateMutex();
        if (s_info_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(weather_task, "weather", 8 * 1024, NULL, 3,
                                                    &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_weather_refresh(void)
{
    s_refresh_req = true;
}

bool app_weather_get_info(app_weather_info_t *info)
{
    if (info == NULL || s_info_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_info_lock, portMAX_DELAY);
    *info = s_info;
    xSemaphoreGive(s_info_lock);
    return info->valid;
}
