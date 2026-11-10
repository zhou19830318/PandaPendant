#include "app_provisioning.h"
#include "app_core.h"
#include "app_settings.h"
#include "app_weather.h"
#include "panda_audio.h"
#include "panda_connectivity.h"
#include "panda_display.h"
#include "panda_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_system.h"

static const char *TAG = "prov";

#define BODY_MAX 6144
#define MAX_SCAN_AP 20

static httpd_handle_t s_server;

static void html_escape(const char *src, char *dst, size_t cap)
{
    size_t out = 0;
    if (cap == 0) {
        return;
    }
    for (const char *p = src ? src : ""; *p && out + 1 < cap; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (out + n >= cap) {
                break;
            }
            memcpy(dst + out, rep, n);
            out += n;
        } else {
            dst[out++] = *p;
        }
    }
    dst[out] = '\0';
}

static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t out = 0;
    if (cap == 0) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (out + 3 > cap) {
                break;
            }
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c < 0x20) {
            if (out + 7 > cap) {
                break;
            }
            int n = snprintf(dst + out, cap - out, "\\u%04x", c);
            if (n < 0) {
                break;
            }
            out += (size_t)n;
        } else {
            if (out + 2 > cap) {
                break;
            }
            dst[out++] = (char)c;
        }
    }
    dst[out < cap ? out : cap - 1] = '\0';
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src ? src : "", cap ? cap - 1 : 0);
    if (cap == 0) {
        return;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void url_decode(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool form_get(char *body, const char *key, char *out, size_t out_cap)
{
    size_t klen = strlen(key);
    char *p = body;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            char *value = p + klen + 1;
            size_t len = amp ? (size_t)(amp - value) : strlen(value);
            if (len >= out_cap) {
                len = out_cap - 1;
            }
            memcpy(out, value, len);
            out[len] = '\0';
            url_decode(out);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static esp_err_t send_chunk(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_get(httpd_req_t *req)
{
    panda_settings_t *cfg = panda_settings_get();
    char esc[PANDA_PERSONA_MAX * 6 + 1];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    send_chunk(req,
        "<!doctype html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Panda AI 配网</title><style>"
        "body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#f5f5f7;color:#222}"
        "main{max-width:520px;margin:0 auto;padding:20px}"
        "h1{font-size:22px;margin:8px 0 4px}h2{font-size:16px;margin:22px 0 8px}"
        ".note{color:#666;font-size:13px;line-height:1.5}"
        "label{display:block;font-size:14px;margin:10px 0 4px}"
        "input,textarea,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #ccc;"
        "border-radius:8px;font-size:15px;background:#fff}"
        "textarea{min-height:80px}button{width:100%;margin:22px 0;padding:12px;border:0;border-radius:8px;"
        "background:#111;color:#fff;font-size:16px}.row{display:flex;gap:12px}.row>div{flex:1}"
        ".scan{width:100%;margin:8px 0 0;padding:10px;background:#2b6cb0;font-size:14px}"
        "</style></head><body><main><h1>Panda AI 配网</h1>"
        "<p class=\"note\">连接热点 <b>");
    send_chunk(req, panda_connectivity_ap_ssid());
    send_chunk(req, "</b> 后打开本页。密钥仅保存在设备 NVS，页面不回显；留空表示保持原值。</p>"
                   "<p class=\"note\">平时本页关闭以节省射频资源；设备 20 秒内连不上已保存的 WiFi 时"
                   "会自动开启热点，连上热点后访问 http://192.168.4.1/ 即可重新配置，"
                   "WiFi 恢复约 5 秒后热点自动关闭。</p>"
                   "<p class=\"note\">当前 WiFi: <b>");
    send_chunk(req, panda_connectivity_is_connected() ? "已连接" :
                    panda_connectivity_is_connecting() ? "连接中" : "未连接");
    send_chunk(req, "</b> ");
    send_chunk(req, panda_connectivity_ip()[0] ? panda_connectivity_ip() : "");
    send_chunk(req, "</p>");

    if (panda_connectivity_disconnect_reason() != 0 && !panda_connectivity_is_connected()) {
        char note[192];
        snprintf(note, sizeof(note),
                 "<p class=\"note\">上次断开原因（%u）：%s</p>",
                 (unsigned)panda_connectivity_disconnect_reason(),
                 panda_connectivity_disconnect_hint());
        send_chunk(req, note);
    }
    if (panda_connectivity_is_ap()) {
        char note[192];
        snprintf(note, sizeof(note),
                 "<p class=\"note\">热点：<b>%s</b>，地址 <b>%s</b>%s</p>",
                 panda_connectivity_ap_ssid(),
                 panda_connectivity_ap_ip()[0] ? panda_connectivity_ap_ip() : "192.168.4.1",
                 panda_connectivity_ap_auto_started() ? "（已保存的 WiFi 连接失败，自动开启）" : "");
        send_chunk(req, note);
    }

    send_chunk(req, "<form method=\"post\" action=\"/save\">");

    send_chunk(req, "<h2>WiFi</h2><label>SSID</label>"
                   "<input id=\"ssid\" name=\"wifi_ssid\" value=\"");
    html_escape(cfg->wifi_ssid, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "\">"
                   "<button type=\"button\" class=\"scan\" id=\"scan_btn\" onclick=\"scanWifi()\">扫描附近 WiFi</button>"
                   "<select id=\"scan_result\" style=\"display:none\"></select>"
                   "<label>密码（留空保持；更换 SSID 时留空表示开放网络）</label>"
                   "<input type=\"password\" name=\"wifi_pass\" autocomplete=\"new-password\" placeholder=\"不修改请留空\">");

    send_chunk(req, "<h2>豆包实时语音</h2><label>鉴权模式</label><select name=\"api_mode\">");
    send_chunk(req, "<option value=\"json\"");
    if (strcmp(cfg->api_mode, "o2") != 0) {
        send_chunk(req, " selected");
    }
    send_chunk(req, ">JSON / X-Api-Key（推荐当前 Key）</option><option value=\"o2\"");
    if (strcmp(cfg->api_mode, "o2") == 0) {
        send_chunk(req, " selected");
    }
    send_chunk(req, ">O2.0 AppID+AccessKey</option></select>");

    send_chunk(req, "<label>API Key（X-Api-Key）</label>"
                   "<input type=\"password\" name=\"api_key\" placeholder=\"留空保持\">");
    send_chunk(req, "<label>App ID（O2.0）</label><input name=\"app_id\" value=\"");
    html_escape(cfg->app_id, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "\"><label>Access Key（O2.0）</label>"
                   "<input type=\"password\" name=\"access_key\" placeholder=\"留空保持\">");
    send_chunk(req, "<label>App Key（O2.0，可选）</label>"
                   "<input type=\"password\" name=\"app_key\" placeholder=\"留空保持\">");
    send_chunk(req, "<label>Resource ID</label><input name=\"resource_id\" value=\"");
    html_escape(cfg->resource_id, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "\"><label>Model</label><input name=\"model\" value=\"");
    html_escape(cfg->model, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "\"><label>音色（speaker）</label><input name=\"speaker\" value=\"");
    html_escape(cfg->speaker, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "\"><label>人设</label><textarea name=\"persona\">");
    html_escape(cfg->persona, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "</textarea>");

    send_chunk(req, "<h2>设备</h2><div class=\"row\"><div><label>音量 0..100</label>"
                   "<input type=\"number\" name=\"volume\" min=\"0\" max=\"100\" value=\"");
    snprintf(esc, sizeof(esc), "%d", cfg->volume);
    send_chunk(req, esc);
    send_chunk(req, "\"></div><div><label>亮度 0..100</label>"
                   "<input type=\"number\" name=\"brightness\" min=\"0\" max=\"100\" value=\"");
    snprintf(esc, sizeof(esc), "%d", cfg->brightness);
    send_chunk(req, esc);
    send_chunk(req, "\"></div></div><label>天气城市</label>"
                   "<input name=\"weather_city\" maxlength=\"31\" placeholder=\"例如 北京\" value=\"");
    html_escape(cfg->weather_city, esc, sizeof(esc));
    send_chunk(req, esc);
    send_chunk(req, "\">"
                   "<button type=\"submit\">保存配置</button></form>"
                   "<p class=\"note\">保存后自动尝试连接 WiFi；豆包语音配置在下次启动语音模块时生效。"
                   "<a href=\"/api/status\">查看状态</a></p>"
                   "<script>"
                   "function scanWifi(){var b=document.getElementById('scan_btn');"
                   "b.disabled=true;b.textContent='扫描中...';"
                   "fetch('/api/scan').then(function(r){return r.json();}).then(function(list){"
                   "var s=document.getElementById('scan_result');"
                   "while(s.options.length>0){s.remove(0);}"
                   "list.forEach(function(a){"
                   "s.add(new Option(a.ssid+'  '+a.rssi+' dBm'+(a.auth?' [加密]':''),a.ssid));});"
                   "s.style.display='block';b.disabled=false;b.textContent='重新扫描';"
                   "}).catch(function(){b.disabled=false;b.textContent='扫描失败，点击重试';});}"
                   "document.getElementById('scan_result').onchange=function(){"
                   "document.getElementById('ssid').value=this.value;};"
                   "</script></main></body></html>");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    char *body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t total = 0;
    int retries = 3;
    while (total < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + total, (size_t)req->content_len - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && retries-- > 0) {
            continue;
        }
        if (n <= 0) {
            heap_caps_free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        total += (size_t)n;
    }
    body[total] = '\0';

    panda_settings_t *cfg = panda_settings_get();
    char value[PANDA_PERSONA_MAX + 1];
    bool wifi_changed = false;

    if (form_get(body, "wifi_ssid", value, sizeof(value)) && value[0]) {
        if (strcmp(value, cfg->wifi_ssid) != 0) {
            wifi_changed = true;
        }
        copy_field(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), value);
    }
    if (form_get(body, "wifi_pass", value, sizeof(value))) {
        if (value[0] || wifi_changed) {
            /* A new password for the same SSID must also trigger a reconnect;
             * previously only an SSID change did, so "save" appeared to do
             * nothing after fixing a typo in the password. */
            if (strcmp(value, cfg->wifi_pass) != 0) {
                wifi_changed = true;
            }
            copy_field(cfg->wifi_pass, sizeof(cfg->wifi_pass), value);
        }
    }
    if (form_get(body, "api_mode", value, sizeof(value))) {
        if (!strcmp(value, "o2") || !strcmp(value, "json")) {
            copy_field(cfg->api_mode, sizeof(cfg->api_mode), value);
        }
    }
    struct {
        const char *key;
        char *dst;
        size_t cap;
        bool secret;
    } fields[] = {
        {"api_key", cfg->api_key, sizeof(cfg->api_key), true},
        {"app_id", cfg->app_id, sizeof(cfg->app_id), false},
        {"access_key", cfg->access_key, sizeof(cfg->access_key), true},
        {"app_key", cfg->app_key, sizeof(cfg->app_key), true},
        {"resource_id", cfg->resource_id, sizeof(cfg->resource_id), false},
        {"model", cfg->model, sizeof(cfg->model), false},
        {"speaker", cfg->speaker, sizeof(cfg->speaker), false},
        {"persona", cfg->persona, sizeof(cfg->persona), false},
        {"weather_city", cfg->weather_city, sizeof(cfg->weather_city), false},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!form_get(body, fields[i].key, value, sizeof(value))) {
            continue;
        }
        if (value[0] || !fields[i].secret) {
            copy_field(fields[i].dst, fields[i].cap, value);
        }
    }

    if (form_get(body, "volume", value, sizeof(value))) {
        int v = atoi(value);
        if (v >= 0 && v <= 100) {
            cfg->volume = v;
            panda_audio_set_volume(v);
            panda_ui_set_volume(v, v == 0);
        }
    }
    if (form_get(body, "brightness", value, sizeof(value))) {
        int v = atoi(value);
        if (v >= 0 && v <= 100) {
            cfg->brightness = v;
            panda_display_set_brightness(v);
        }
    }

    heap_caps_free(body);

    esp_err_t err = panda_settings_save();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    app_core_settings_changed();
    app_weather_refresh();

    if (cfg->wifi_ssid[0] && (wifi_changed || !panda_connectivity_is_connected())) {
        panda_connectivity_connect(cfg->wifi_ssid, cfg->wifi_pass);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req,
        "<!doctype html><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
        "<meta http-equiv=\"refresh\" content=\"2;url=/\">"
        "<body style=\"font-family:sans-serif;padding:24px\"><h2>保存成功</h2>"
        "<p>WiFi 正在连接，页面 2 秒后返回。</p>"
        "<p>若 20 秒内仍连不上，设备会自动开启热点，连上热点后访问 http://192.168.4.1/ 重试。</p>"
        "</body>", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    int rssi = 0;
    bool has_rssi = panda_connectivity_get_rssi(&rssi) == ESP_OK;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"wifi\":\"%s\",\"ip\":\"%s\",\"ap\":\"%s\",\"ap_ip\":\"%s\","
             "\"connected\":%s,\"connecting\":%s,\"heap\":%u,"
             "\"rssi\":%d,\"reason\":%u,\"reason_hint\":\"%s\","
             "\"ap_auto\":%s,\"retry_ms\":%u}",
             panda_connectivity_is_connected() ? "connected" :
             panda_connectivity_is_connecting() ? "connecting" :
             panda_connectivity_is_ap() ? "ap" : "idle",
             panda_connectivity_ip()[0] ? panda_connectivity_ip() : "",
             panda_connectivity_ap_ssid(),
             panda_connectivity_ap_ip()[0] ? panda_connectivity_ap_ip() : "",
             panda_connectivity_is_connected() ? "true" : "false",
             panda_connectivity_is_connecting() ? "true" : "false",
             (unsigned)esp_get_free_heap_size(),
             has_rssi ? rssi : 0,
             (unsigned)panda_connectivity_disconnect_reason(),
             panda_connectivity_disconnect_hint(),
             panda_connectivity_ap_auto_started() ? "true" : "false",
             (unsigned)panda_connectivity_retry_delay_ms());
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    static panda_connectivity_ap_t aps[MAX_SCAN_AP];
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int count = panda_connectivity_scan_aps(aps, MAX_SCAN_AP);
    if (count < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    esp_err_t err = send_chunk(req, "[");
    char buf[384];
    for (int i = 0; err == ESP_OK && i < count; i++) {
        char ssid[sizeof(aps[i].ssid) * 6 + 1];
        json_escape(aps[i].ssid, ssid, sizeof(ssid));
        snprintf(buf, sizeof(buf),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"auth\":%s}",
                 i ? "," : "", ssid, aps[i].rssi, (unsigned)aps[i].channel,
                 aps[i].authmode == 0 ? "false" : "true");
        err = send_chunk(req, buf);
    }
    if (err == ESP_OK) {
        err = send_chunk(req, "]");
    }
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t captive_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return captive_get(req);
}

esp_err_t app_provisioning_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
        }
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);

    ESP_LOGI(TAG, "provisioning ready: http://%s/",
             panda_connectivity_ap_ip()[0] ? panda_connectivity_ap_ip() : "192.168.4.1");
    return ESP_OK;
}

void app_provisioning_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "provisioning page stopped");
    }
}

bool app_provisioning_is_running(void)
{
    return s_server != NULL;
}
