#include "app_settings.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NVS_NS = "panda";
static panda_settings_t s_cfg;
static bool s_loaded;

static void set_defaults(panda_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    /* JSON is the only implemented transport; switch back to "o2" once the
     * official binary protocol lands, otherwise a factory reset breaks voice. */
    strcpy(c->api_mode, "json");
    strcpy(c->resource_id, "volc.speech.dialog");
    c->app_key[0] = '\0';
    strcpy(c->model, "1.2.1.1");
    c->volume = 70;
    c->brightness = 80;
    c->auto_continue = 0;
    c->silence_timeout_ms = 1500;
    c->max_record_seconds = 15;
    c->personality = 1;
    c->feedback = 1;
    c->animation = 1;
    c->auto_sleep_s = 300;
    c->wake_mode = 2;
    c->sleep_anim = 1;
    c->dnd = 0;
    strcpy(c->weather_city, "北京");
    snprintf(c->persona, sizeof(c->persona), "你是一只温柔、好奇、有点俏皮的熊猫伙伴。回答简短、口语化。");
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read %s: %s", key, esp_err_to_name(err));
    }
}

static void load_i32(nvs_handle_t h, const char *key, int *dst)
{
    int32_t v;
    if (nvs_get_i32(h, key, &v) == ESP_OK) {
        *dst = (int)v;
    }
}

esp_err_t panda_settings_init(void)
{
    set_defaults(&s_cfg);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no settings yet, using defaults");
        s_loaded = true;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open");

    load_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    load_str(h, "api_mode", s_cfg.api_mode, sizeof(s_cfg.api_mode));
    load_str(h, "app_id", s_cfg.app_id, sizeof(s_cfg.app_id));
    load_str(h, "access_key", s_cfg.access_key, sizeof(s_cfg.access_key));
    load_str(h, "resource_id", s_cfg.resource_id, sizeof(s_cfg.resource_id));
    load_str(h, "app_key", s_cfg.app_key, sizeof(s_cfg.app_key));
    load_str(h, "api_key", s_cfg.api_key, sizeof(s_cfg.api_key));
    load_str(h, "model", s_cfg.model, sizeof(s_cfg.model));
    load_str(h, "speaker", s_cfg.speaker, sizeof(s_cfg.speaker));
    load_str(h, "persona", s_cfg.persona, sizeof(s_cfg.persona));
    load_str(h, "wx_city", s_cfg.weather_city, sizeof(s_cfg.weather_city));
    load_i32(h, "volume", &s_cfg.volume);
    load_i32(h, "brightness", &s_cfg.brightness);
    load_i32(h, "auto_cont", &s_cfg.auto_continue);
    load_i32(h, "vad_ms", &s_cfg.silence_timeout_ms);
    load_i32(h, "max_rec", &s_cfg.max_record_seconds);
    load_i32(h, "pers", &s_cfg.personality);
    load_i32(h, "fb_lvl", &s_cfg.feedback);
    load_i32(h, "anim_lvl", &s_cfg.animation);
    load_i32(h, "sleep_s", &s_cfg.auto_sleep_s);
    load_i32(h, "wake_mode", &s_cfg.wake_mode);
    load_i32(h, "sleep_anim", &s_cfg.sleep_anim);
    load_i32(h, "dnd", &s_cfg.dnd);
    nvs_close(h);

    if (s_cfg.volume < 0) s_cfg.volume = 0;
    if (s_cfg.volume > 100) s_cfg.volume = 100;
    if (s_cfg.brightness < 0) s_cfg.brightness = 0;
    if (s_cfg.brightness > 100) s_cfg.brightness = 100;
    if (s_cfg.silence_timeout_ms < 300) s_cfg.silence_timeout_ms = 300;
    if (s_cfg.silence_timeout_ms > 3000) s_cfg.silence_timeout_ms = 3000;
    if (s_cfg.max_record_seconds < 3) s_cfg.max_record_seconds = 3;
    if (s_cfg.max_record_seconds > 60) s_cfg.max_record_seconds = 60;
    if (s_cfg.personality < 0 || s_cfg.personality > 2) s_cfg.personality = 1;
    if (s_cfg.feedback < 0 || s_cfg.feedback > 2) s_cfg.feedback = 1;
    if (s_cfg.animation < 0 || s_cfg.animation > 2) s_cfg.animation = 1;
    if (s_cfg.auto_sleep_s != 0 && s_cfg.auto_sleep_s != 60 &&
        s_cfg.auto_sleep_s != 300 && s_cfg.auto_sleep_s != 600) {
        s_cfg.auto_sleep_s = 300;
    }
    if (s_cfg.wake_mode < 0 || s_cfg.wake_mode > 2) s_cfg.wake_mode = 2;
    if (s_cfg.sleep_anim != 0) s_cfg.sleep_anim = 1;
    if (s_cfg.dnd != 0) s_cfg.dnd = 1;
    if (s_cfg.weather_city[0] == '\0') {
        strcpy(s_cfg.weather_city, "北京");
    }
    s_loaded = true;
    ESP_LOGI(TAG, "loaded: wifi='%s' api_mode=%s app_id=%s model=%s vol=%d bright=%d",
             s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "(unset)", s_cfg.api_mode,
             s_cfg.app_id[0] ? panda_settings_masked(s_cfg.app_id) : "(unset)",
             s_cfg.model, s_cfg.volume, s_cfg.brightness);
    ESP_LOGI(TAG, "menu: pers=%d fb=%d anim=%d sleep=%ds wake=%d sleep_anim=%d dnd=%d city=%s",
             s_cfg.personality, s_cfg.feedback, s_cfg.animation, s_cfg.auto_sleep_s,
             s_cfg.wake_mode, s_cfg.sleep_anim, s_cfg.dnd, s_cfg.weather_city);
    return ESP_OK;
}

panda_settings_t *panda_settings_get(void)
{
    return &s_cfg;
}

esp_err_t panda_settings_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");

#define PUT_STR(k, v) nvs_set_str(h, k, v)
#define PUT_I32(k, v) nvs_set_i32(h, k, (int32_t)(v))
    ESP_RETURN_ON_ERROR(PUT_STR("wifi_ssid", s_cfg.wifi_ssid), TAG, "wifi_ssid");
    ESP_RETURN_ON_ERROR(PUT_STR("wifi_pass", s_cfg.wifi_pass), TAG, "wifi_pass");
    ESP_RETURN_ON_ERROR(PUT_STR("api_mode", s_cfg.api_mode), TAG, "api_mode");
    ESP_RETURN_ON_ERROR(PUT_STR("app_id", s_cfg.app_id), TAG, "app_id");
    ESP_RETURN_ON_ERROR(PUT_STR("access_key", s_cfg.access_key), TAG, "access_key");
    ESP_RETURN_ON_ERROR(PUT_STR("resource_id", s_cfg.resource_id), TAG, "resource_id");
    ESP_RETURN_ON_ERROR(PUT_STR("app_key", s_cfg.app_key), TAG, "app_key");
    ESP_RETURN_ON_ERROR(PUT_STR("api_key", s_cfg.api_key), TAG, "api_key");
    ESP_RETURN_ON_ERROR(PUT_STR("model", s_cfg.model), TAG, "model");
    ESP_RETURN_ON_ERROR(PUT_STR("speaker", s_cfg.speaker), TAG, "speaker");
    ESP_RETURN_ON_ERROR(PUT_STR("persona", s_cfg.persona), TAG, "persona");
    ESP_RETURN_ON_ERROR(PUT_STR("wx_city", s_cfg.weather_city), TAG, "wx_city");
    ESP_RETURN_ON_ERROR(PUT_I32("volume", s_cfg.volume), TAG, "volume");
    ESP_RETURN_ON_ERROR(PUT_I32("brightness", s_cfg.brightness), TAG, "brightness");
    ESP_RETURN_ON_ERROR(PUT_I32("auto_cont", s_cfg.auto_continue), TAG, "auto_cont");
    ESP_RETURN_ON_ERROR(PUT_I32("vad_ms", s_cfg.silence_timeout_ms), TAG, "vad_ms");
    ESP_RETURN_ON_ERROR(PUT_I32("max_rec", s_cfg.max_record_seconds), TAG, "max_rec");
    ESP_RETURN_ON_ERROR(PUT_I32("pers", s_cfg.personality), TAG, "pers");
    ESP_RETURN_ON_ERROR(PUT_I32("fb_lvl", s_cfg.feedback), TAG, "fb_lvl");
    ESP_RETURN_ON_ERROR(PUT_I32("anim_lvl", s_cfg.animation), TAG, "anim_lvl");
    ESP_RETURN_ON_ERROR(PUT_I32("sleep_s", s_cfg.auto_sleep_s), TAG, "sleep_s");
    ESP_RETURN_ON_ERROR(PUT_I32("wake_mode", s_cfg.wake_mode), TAG, "wake_mode");
    ESP_RETURN_ON_ERROR(PUT_I32("sleep_anim", s_cfg.sleep_anim), TAG, "sleep_anim");
    ESP_RETURN_ON_ERROR(PUT_I32("dnd", s_cfg.dnd), TAG, "dnd");
#undef PUT_STR
#undef PUT_I32
    ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "nvs commit");
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved");
    return ESP_OK;
}

esp_err_t panda_settings_reset(void)
{
    set_defaults(&s_cfg);
    ESP_LOGI(TAG, "settings reset to defaults");
    return panda_settings_save();
}

const char *panda_settings_masked(const char *s)
{
    if (!s || !s[0]) {
        return "(unset)";
    }
    return "******";
}

void panda_settings_print(bool include_secrets)
{
    const panda_settings_t *c = &s_cfg;
    ESP_LOGI(TAG, "wifi_ssid=%s wifi_pass=%s", c->wifi_ssid[0] ? c->wifi_ssid : "(unset)",
             include_secrets ? c->wifi_pass : panda_settings_masked(c->wifi_pass));
    ESP_LOGI(TAG, "api_mode=%s resource_id=%s app_key=%s", c->api_mode, c->resource_id,
             include_secrets ? c->app_key : panda_settings_masked(c->app_key));
    ESP_LOGI(TAG, "app_id=%s", include_secrets ? c->app_id : panda_settings_masked(c->app_id));
    ESP_LOGI(TAG, "access_key=%s", include_secrets ? c->access_key : panda_settings_masked(c->access_key));
    ESP_LOGI(TAG, "api_key=%s model=%s speaker=%s",
             include_secrets ? c->api_key : panda_settings_masked(c->api_key),
             c->model, c->speaker[0] ? c->speaker : "(default)");
    ESP_LOGI(TAG, "volume=%d brightness=%d auto_continue=%d vad_ms=%d max_rec=%ds",
             c->volume, c->brightness, c->auto_continue, c->silence_timeout_ms,
             c->max_record_seconds);
    ESP_LOGI(TAG, "pers=%d feedback=%d animation=%d auto_sleep=%ds wake=%d sleep_anim=%d dnd=%d",
             c->personality, c->feedback, c->animation, c->auto_sleep_s,
             c->wake_mode, c->sleep_anim, c->dnd);
    ESP_LOGI(TAG, "weather_city=%s", c->weather_city);
    ESP_LOGI(TAG, "persona=%s", c->persona);
}
