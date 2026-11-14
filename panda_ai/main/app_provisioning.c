#include "app_provisioning.h"
#include "app_core.h"
#include "app_settings.h"
#include "app_weather.h"
#include "panda_audio.h"
#include "panda_board.h"
#include "panda_connectivity.h"
#include "panda_display.h"
#include "panda_mp3.h"
#include "panda_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "prov";

#define BODY_MAX 6144
#define MAX_SCAN_AP 20
#define UPLOAD_MAX_BYTES (20 * 1024 * 1024)

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
    /* A zero-length chunk terminates the response. Empty optional fields
     * (unset App ID, no STA IP) used to end the page mid-HTML, which made
     * the provisioning page look broken or unreachable. */
    if (!text || !text[0]) {
        return ESP_OK;
    }
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_get(httpd_req_t *req)
{
    panda_settings_t *cfg = panda_settings_get();
    /* The escaped persona can be 3 KB; keeping it off the 4 KB httpd task
     * stack (which must be small enough for the fragmented internal heap). */
    size_t esc_cap = PANDA_PERSONA_MAX * 6 + 1;
    char *esc = heap_caps_malloc(esc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!esc) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
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
        ".net{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;"
        "margin:6px 0;background:#fff;border:1px solid #ddd;border-radius:8px;font-size:15px}"
        ".net form{margin:0}.del{width:auto;margin:0;padding:6px 12px;background:#c0392b;font-size:13px}"
        "</style></head><body><main><h1>Panda AI 配置</h1>"
        "<p class=\"note\">设备联网后，在同一局域网的浏览器打开本页即可上传音乐、修改设置。"
                   "设备连不上已保存的 WiFi 时会自动开启热点 <b>");
    send_chunk(req, panda_connectivity_ap_ssid());
    send_chunk(req, "</b>，屏幕也会显示该热点的二维码，扫码加入后访问 "
                   "http://192.168.4.1/ ；WiFi 恢复约 5 秒后热点自动关闭。"
                   "密钥仅保存在设备 NVS，页面不回显；留空表示保持原值。</p>"
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

    send_chunk(req, "<h2>已保存的 WiFi</h2>");
    if (cfg->wifi_count == 0) {
        send_chunk(req, "<p class=\"note\">还没有保存的网络。</p>");
    }
    for (int i = 0; i < cfg->wifi_count; i++) {
        send_chunk(req, "<div class=\"net\"><span>");
        html_escape(cfg->wifi[i].ssid, esc, esc_cap);
        send_chunk(req, esc);
        send_chunk(req, "</span><form method=\"post\" action=\"/wifi/delete\">"
                       "<input type=\"hidden\" name=\"ssid\" value=\"");
        html_escape(cfg->wifi[i].ssid, esc, esc_cap);
        send_chunk(req, esc);
        send_chunk(req, "\"><button type=\"submit\" class=\"del\">删除</button></form></div>");
    }
    send_chunk(req, "<p class=\"note\">最多保存 4 个网络；连接时按信号强度自动选择。</p>"
                   "<form method=\"post\" action=\"/wifi\">"
                   "<label>SSID</label><input id=\"ssid\" name=\"ssid\" required>"
                   "<button type=\"button\" class=\"scan\" id=\"scan_btn\" onclick=\"scanWifi()\">扫描附近 WiFi</button>"
                   "<select id=\"scan_result\" style=\"display:none\"></select>"
                   "<label>密码（开放网络留空）</label>"
                   "<input type=\"password\" name=\"pass\" autocomplete=\"new-password\" placeholder=\"开放网络留空\">"
                   "<button type=\"submit\">保存并连接</button></form>");

    send_chunk(req, "<h2>豆包实时语音</h2><form method=\"post\" action=\"/save\">"
                   "<label>鉴权模式</label><select name=\"api_mode\">");
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
    html_escape(cfg->app_id, esc, esc_cap);
    send_chunk(req, esc);
    send_chunk(req, "\"><label>Access Key（O2.0）</label>"
                   "<input type=\"password\" name=\"access_key\" placeholder=\"留空保持\">");
    send_chunk(req, "<label>App Key（O2.0，可选）</label>"
                   "<input type=\"password\" name=\"app_key\" placeholder=\"留空保持\">");
    send_chunk(req, "<label>Resource ID</label><input name=\"resource_id\" value=\"");
    html_escape(cfg->resource_id, esc, esc_cap);
    send_chunk(req, esc);
    send_chunk(req, "\"><label>Model</label><input name=\"model\" value=\"");
    html_escape(cfg->model, esc, esc_cap);
    send_chunk(req, esc);
    send_chunk(req, "\"><label>音色（speaker）</label><input name=\"speaker\" value=\"");
    html_escape(cfg->speaker, esc, esc_cap);
    send_chunk(req, esc);
    send_chunk(req, "\"><label>人设</label><textarea name=\"persona\">");
    html_escape(cfg->persona, esc, esc_cap);
    send_chunk(req, esc);
    send_chunk(req, "</textarea>");

    send_chunk(req, "<h2>SD 卡音乐</h2>");
    int tracks = panda_mp3_scan();
    if (tracks <= 0) {
        send_chunk(req, "<p class=\"note\">SD 卡 Music 文件夹里还没有 MP3。</p>");
    } else {
        send_chunk(req, "<p class=\"note\">已找到以下曲目（设备菜单里的“音乐播放”页按此顺序播放）：</p>");
        for (int i = 0; i < tracks; i++) {
            char row[32];
            send_chunk(req, "<div class=\"net\"><span>");
            snprintf(row, sizeof(row), "%d. ", i + 1);
            send_chunk(req, row);
            html_escape(panda_mp3_title(i), esc, esc_cap);
            send_chunk(req, esc);
            send_chunk(req, "</span></div>");
        }
    }
    send_chunk(req, "<p class=\"note\">选择 MP3 文件上传到 SD 卡的 Music 文件夹（单文件 ≤ 20 MB）。"
                   "上传完成后进入设备菜单的“音乐播放”页即可播放。</p>"
                   "<input type=\"file\" id=\"mp3file\" accept=\".mp3,audio/mpeg\">"
                   "<button type=\"button\" onclick=\"uploadMp3()\">上传到 SD 卡</button>"
                   "<p class=\"note\" id=\"up_status\"></p>"
                   "<script>"
                   "function uploadMp3(){"
                   "var f=document.getElementById('mp3file').files[0];"
                   "var st=document.getElementById('up_status');"
                   "if(!f){st.textContent='请先选择 MP3 文件';return;}"
                   "if(!/\\.mp3$/i.test(f.name)){st.textContent='只支持 .mp3 文件';return;}"
                   "st.textContent='上传中：0%';"
                   "var x=new XMLHttpRequest();"
                   "x.open('POST','/upload/music?name='+encodeURIComponent(f.name));"
                   "x.upload.onprogress=function(e){if(e.lengthComputable){"
                   "st.textContent='上传中：'+Math.round(e.loaded*100/e.total)+'%';}};"
                   "x.onload=function(){if(x.status==200){st.textContent='上传完成，正在刷新列表…';"
                   "setTimeout(function(){location.reload();},600);}"
                   "else{st.textContent='上传失败（'+x.status+'）';}};"
                   "x.onerror=function(){st.textContent='上传失败：网络错误';};"
                   "x.send(f);}"
                   "</script>");

    send_chunk(req, "<h2>设备</h2><div class=\"row\"><div><label>音量 0..100</label>"
                   "<input type=\"number\" name=\"volume\" min=\"0\" max=\"100\" value=\"");
    snprintf(esc, esc_cap, "%d", cfg->volume);
    send_chunk(req, esc);
    send_chunk(req, "\"></div><div><label>亮度 0..100</label>"
                   "<input type=\"number\" name=\"brightness\" min=\"0\" max=\"100\" value=\"");
    snprintf(esc, esc_cap, "%d", cfg->brightness);
    send_chunk(req, esc);
    send_chunk(req, "\"></div></div><label>天气城市</label>"
                   "<input name=\"weather_city\" maxlength=\"31\" placeholder=\"例如 北京\" value=\"");
    html_escape(cfg->weather_city, esc, esc_cap);
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
    heap_caps_free(esc);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static char *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= BODY_MAX) {
        return NULL;
    }
    char *body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return NULL;
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
            return NULL;
        }
        total += (size_t)n;
    }
    body[total] = '\0';
    return body;
}

static esp_err_t send_result_page(httpd_req_t *req, const char *title, const char *detail)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char page[512];
    snprintf(page, sizeof(page),
             "<!doctype html><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width\">"
             "<meta http-equiv=\"refresh\" content=\"2;url=/\">"
             "<body style=\"font-family:sans-serif;padding:24px\"><h2>%s</h2>"
             "<p>%s</p><p>页面 2 秒后返回。</p></body>", title, detail);
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void apply_wifi_settings(void)
{
    panda_settings_t *cfg = panda_settings_get();
    panda_connectivity_set_credentials(cfg->wifi, cfg->wifi_count);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    panda_settings_t *cfg = panda_settings_get();
    char ssid[PANDA_SSID_MAX] = {0};
    char pass[PANDA_PASS_MAX] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    if (!form_get(body, "pass", pass, sizeof(pass))) {
        pass[0] = '\0';
    }
    int index = panda_settings_wifi_index(ssid);
    if (index >= 0 && !pass[0]) {
        /* Re-saving an existing network without retyping the password keeps
         * the stored one; an entirely new SSID with no password is open. */
        snprintf(pass, sizeof(pass), "%s", cfg->wifi[index].pass);
    }
    if (index < 0 && cfg->wifi_count >= PANDA_WIFI_CRED_MAX) {
        heap_caps_free(body);
        return send_result_page(req, "保存失败", "最多只能保存 4 个 WiFi，请先删除一个。");
    }
    esp_err_t err = panda_settings_wifi_put(ssid, pass);
    heap_caps_free(body);
    if (err != ESP_OK) {
        return send_result_page(req, "保存失败", "写入设置出错。");
    }
    err = panda_settings_save();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wifi saved: '%s' (%d stored)", ssid, cfg->wifi_count);
    apply_wifi_settings();
    return send_result_page(req, "WiFi 已保存", "设备正在扫描并连接信号最强的已保存网络。");
}

static esp_err_t wifi_delete_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char ssid[PANDA_SSID_MAX] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    heap_caps_free(body);
    esp_err_t err = panda_settings_wifi_remove(ssid);
    if (err == ESP_OK) {
        panda_settings_save();
        ESP_LOGI(TAG, "wifi removed: '%s' (%d left)", ssid, panda_settings_wifi_count());
        apply_wifi_settings();
    }
    return send_result_page(req, err == ESP_OK ? "已删除" : "未找到",
                            err == ESP_OK ? "该网络已从设备移除。" : "没有这个已保存的网络。");
}

static esp_err_t upload_music_post(httpd_req_t *req)
{
    char query[768];
    char name[256];
    if (req->content_len <= 0 || req->content_len > UPLOAD_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name required");
    }
    /* httpd_query_key_value() hands back the raw value, so Chinese names
     * arrive percent-encoded (three bytes per character). */
    url_decode(name);

    /* Keep only the base name and a filesystem-safe charset. */
    char *slash = strrchr(name, '/');
    if (slash != NULL) {
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    }
    for (char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\\' || c == '?' || c == '&' || c == '%' || c == '*' ||
            c == ':' || c == '"' || c == '<' || c == '>' || c == '|') {
            *p = '_';
        }
    }
    size_t len = strlen(name);
    if (len <= 4 || strcasecmp(name + len - 4, ".mp3") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only .mp3");
    }
    if (!panda_board_sdcard_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sdcard");
    }

    char dir[128];
    snprintf(dir, sizeof(dir), "%.96s/Music", panda_board_sdcard_mount_point());
    if (mkdir(dir, 0777) != 0) {
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGE(TAG, "cannot create %s", dir);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
        }
    }

    char path[320];
    snprintf(path, sizeof(path), "%.100s/%.200s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }

    /* ESP32-S3's SDMMC DMA cannot address PSRAM, so a PSRAM buffer makes
     * FatFs fall back to one 512 B command per sector; on this slow SDSC card
     * that is ~8 KB/s. Even with an internal buffer, FatFs only issues a
     * multi-block write while the file offset is sector aligned; after the
     * first partial chunk every following 512 B sector goes out as its own
     * command (~58 ms each on this card). So fill the buffer completely and
     * only write whole chunks: the file offset stays a multiple of the chunk
     * size and FatFs always takes the bulk path. */
    enum { UPLOAD_CHUNK = 16384 };
    char *buf = heap_caps_aligned_alloc(4096, UPLOAD_CHUNK, MALLOC_CAP_DMA);
    bool dma_buf = buf != NULL;
    if (buf == NULL) {
        ESP_LOGW(TAG, "no internal DMA buffer, upload will be slow");
        buf = heap_caps_malloc(UPLOAD_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        close(fd);
        remove(path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    ESP_LOGI(TAG, "upload %s: %s buffer @%p (mod4096=%u)",
             name, dma_buf ? "dma" : "psram", buf,
             (unsigned)((uintptr_t)buf & 4095));

    int64_t start_us = esp_timer_get_time();
    size_t total = 0;
    size_t fill = 0;
    bool ok = true;
    int timeouts = 0;
    int64_t recv_us = 0;
    int64_t write_us = 0;
    int64_t max_recv_us = 0;
    int64_t max_write_us = 0;
    while (total + fill < (size_t)req->content_len) {
        int64_t t0 = esp_timer_get_time();
        int n = httpd_req_recv(req, buf + fill, UPLOAD_CHUNK - fill);
        int64_t t1 = esp_timer_get_time();
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        recv_us += t1 - t0;
        if (t1 - t0 > max_recv_us) {
            max_recv_us = t1 - t0;
        }
        fill += (size_t)n;
        if (fill < UPLOAD_CHUNK && total + fill < (size_t)req->content_len) {
            continue;
        }
        if (write(fd, buf, fill) != (ssize_t)fill) {
            ok = false;
            break;
        }
        int64_t t2 = esp_timer_get_time();
        write_us += t2 - t1;
        if (t2 - t1 > max_write_us) {
            max_write_us = t2 - t1;
        }
        size_t prev = total;
        total += fill;
        fill = 0;
        if ((total / (1024 * 1024)) != (prev / (1024 * 1024))) {
            ESP_LOGI(TAG, "upload %s: %u/%u KiB @%u KB/s (recv %lld ms, write %lld ms, max %lld/%lld ms)",
                     name, (unsigned)(total / 1024),
                     (unsigned)((size_t)req->content_len / 1024),
                     (unsigned)(total / 1024 / ((esp_timer_get_time() - start_us) / 1000000 + 1)),
                     recv_us / 1000, write_us / 1000,
                     max_recv_us / 1000, max_write_us / 1000);
        }
        if (!dma_buf) {
            /* The PSRAM fallback writes one 512 B sector per command and can
             * stall long enough to trip the task watchdog. */
            vTaskDelay(1);
        }
    }
    heap_caps_free(buf);
    close(fd);
    if (!ok) {
        remove(path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }

    int tracks = panda_mp3_scan();
    ESP_LOGI(TAG, "uploaded %s (%u bytes), %d track(s) on card",
             name, (unsigned)total, tracks);

    char json[160];
    snprintf(json, sizeof(json), "{\"ok\":true,\"file\":\"%s\",\"bytes\":%u,\"tracks\":%d}",
             name, (unsigned)total, tracks);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    panda_settings_t *cfg = panda_settings_get();
    char value[PANDA_PERSONA_MAX + 1];
    char prev_city[PANDA_CITY_MAX];
    snprintf(prev_city, sizeof(prev_city), "%s", cfg->weather_city);

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

    if (strcmp(prev_city, cfg->weather_city) != 0) {
        cfg->weather_city_label[0] = '\0';
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

    return send_result_page(req, "保存成功", "豆包语音配置已保存，下次启动语音模块时生效。");
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
             "\"ap_auto\":%s,\"retry_ms\":%u,\"wifi_saved\":%d}",
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
             (unsigned)panda_connectivity_retry_delay_ms(),
             panda_settings_wifi_count());
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
    config.max_uri_handlers = 13;
    /* 8 KB did not fit in the fragmented internal heap right after SoftAP
     * start: httpd_start bound port 80, then failed to create its task, and
     * IDF does not close the listen socket on that path. Every later retry
     * then failed with EADDRINUSE (112).
     * The music upload handler needs the 8 KB: FatFs + SDMMC write paths plus
     * the request buffers overflowed a 4 KB httpd stack at ~1 MB into a file.
     * After the LVGL/MP3 pools moved to PSRAM the internal heap is large
     * enough, and a start is refused unless a contiguous block is available,
     * so a failure never leaks the port. */
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    /* Phones keep the captive-portal probe socket open for a few seconds
     * while their "no internet" check runs; the defaults (5 s) made the
     * first page load fail on some Android builds. */
    config.max_open_sockets = 4;
    config.backlog_conn = 4;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < config.stack_size + 2048) {
        ESP_LOGW(TAG, "defer httpd: largest internal block %u < %u",
                 (unsigned)largest, (unsigned)(config.stack_size + 2048));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd start failed: %s (largest internal %u)",
                 esp_err_to_name(err), (unsigned)largest);
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/wifi/delete", .method = HTTP_POST, .handler = wifi_delete_post},
        {.uri = "/upload/music", .method = HTTP_POST, .handler = upload_music_post},
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
