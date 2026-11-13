#include "panda_connectivity.h"
#include "panda_board.h"

#include "esp_check.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "wifi";

#define WIFI_RETRY_MIN_MS 1000
#define WIFI_RETRY_MAX_MS 30000
#define AP_FALLBACK_MS (20 * 1000)
/* Grace period before the auto-started SoftAP is torn down after the STA
 * link recovers, so a provisioning page in flight can finish its response. */
#define AP_OFF_DELAY_MS 5000
/* Applying a new AP config restarts the interface (AP_STOP + AP_START a few
 * ms apart); debounce the stop edge so the provisioning page does not flap. */
#define AP_STOP_DEBOUNCE_MS 300
#define MAX_SCAN_AP 20
/* Data-path watchdog. The driver can keep the association and IP while ARP /
 * TCP forwarding silently dies (ESP-IDF #8123): every reconnect then times
 * out with select()/EINPROGRESS until the interface is bounced. Four failed
 * outbound connects inside the window is the trigger. */
#define NET_ERR_WINDOW_US (90LL * 1000 * 1000)
#define NET_ERR_THRESHOLD 3
/* Shorter than one error-accumulation cycle (3 attempts ~30 s) plus a
 * reconnect, so a real second wedge is not locked out by a recent manual
 * recovery, but a server outage cannot bounce the link more than ~1/45 s. */
#define RECOVER_COOLDOWN_US (45LL * 1000 * 1000)
#define RECOVER_WAIT_MS 20000

typedef enum {
    WIFI_CMD_RECONNECT = 1,
    WIFI_CMD_SELECT,
    WIFI_CMD_AP_ON,
    WIFI_CMD_AP_OFF,
    WIFI_CMD_RECOVER,
} wifi_cmd_t;

static panda_connectivity_status_cb_t s_status_cb;
static panda_connectivity_time_cb_t s_time_cb;
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_ap_timer;
static esp_timer_handle_t s_ap_off_timer;
static esp_timer_handle_t s_ap_stop_timer;
/* Timers only enqueue work: calling esp_wifi_* from an esp_timer callback
 * while the driver task is busy is a known way to wedge the driver
 * (ESP-IDF #15704). One task owns all recovery/AP transitions. */
static QueueHandle_t s_worker_q;
static TaskHandle_t s_worker_task;
static int64_t s_net_err_first_us;
static uint32_t s_net_err_count;
static int64_t s_last_recover_us;
static volatile bool s_recover_force;
static bool s_inited;
static panda_wifi_cred_t s_creds[PANDA_WIFI_CRED_MAX];
static int s_cred_count;
static int s_active_cred = -1;
static bool s_manual_disconnect;
static bool s_connected;
static bool s_ap_enabled;
static bool s_ap_auto;
static bool s_ap_user_on;
static bool s_ap_user_off;
static bool s_connecting;
static bool s_sntp_started;
static uint32_t s_retry_delay_ms = WIFI_RETRY_MIN_MS;
static uint8_t s_last_reason;
static uint8_t s_sta_channel = 1;
static char s_ip[16];
static char s_ap_ip[16];
static char s_ssid[33];
static char s_ap_ssid[33] = CONFIG_PANDA_WIFI_AP_PREFIX;

static bool has_creds(void)
{
    return s_cred_count > 0;
}


static void notify_status(void)
{
    if (s_status_cb) {
        s_status_cb(s_connected, s_ap_enabled, s_connecting);
    }
}

static const char *reason_hint(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:          return "AP not found (out of range or 5GHz-only)";
    case WIFI_REASON_AUTH_FAIL:            return "authentication failed (wrong password)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "handshake timeout (wrong password or WPA mode)";
    case WIFI_REASON_MIC_FAILURE:          return "MIC failure (WPA key mismatch)";
    case WIFI_REASON_BEACON_TIMEOUT:       return "beacon timeout (weak signal or AP sleep)";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:      return "association failed (AP busy or incompatible)";
    case WIFI_REASON_AUTH_EXPIRE:          return "authentication expired";
    case WIFI_REASON_ASSOC_LEAVE:          return "left AP";
    case WIFI_REASON_ROAMING:              return "roaming";
    default:                               return "unknown";
    }
}

/* Exponential backoff with +0..25% jitter so several pendants do not
 * hammer the AP in lockstep after a router reboot. */
static uint32_t next_retry_delay_ms(void)
{
    uint32_t base = s_retry_delay_ms;
    uint32_t delay = base + (esp_random() % (base / 4 + 1));
    if (s_retry_delay_ms < WIFI_RETRY_MAX_MS) {
        s_retry_delay_ms *= 2;
        if (s_retry_delay_ms > WIFI_RETRY_MAX_MS) {
            s_retry_delay_ms = WIFI_RETRY_MAX_MS;
        }
    }
    return delay;
}

static bool post_cmd(wifi_cmd_t cmd)
{
    if (!s_worker_q) {
        return false;
    }
    return xQueueSend(s_worker_q, &cmd, 0) == pdPASS;
}

static bool wait_for_ip(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 500) {
        if (s_connected) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return s_connected;
}

static void recover_now(void)
{
    bool force = s_recover_force;
    s_recover_force = false;
    if (s_manual_disconnect || !has_creds()) {
        return;
    }
    int64_t now = esp_timer_get_time();
    /* s_last_recover_us == 0 means "never recovered"; uptime alone must not
     * start the cooldown or the first wedge in the first two minutes is
     * ignored. */
    if (!force && s_last_recover_us != 0 &&
        now - s_last_recover_us < RECOVER_COOLDOWN_US) {
        ESP_LOGI(TAG, "network recovery suppressed (cooldown %" PRIi64 " s)",
                 (RECOVER_COOLDOWN_US - (now - s_last_recover_us)) / 1000000);
        return;
    }
    s_last_recover_us = now;
    s_net_err_count = 0;

    int rssi = 0;
    panda_connectivity_get_rssi(&rssi);
    ESP_LOGW(TAG, "network wedge suspected: recovery 1/2 disconnect+connect "
             "(ip=%s rssi=%d reason=%u %s)",
             s_ip, rssi, (unsigned)s_last_reason, reason_hint(s_last_reason));
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    esp_wifi_disconnect();
    /* Let the disconnect event land: a connect() issued before it completes
     * is rejected and recovery falls back to a 30 s backoff step. */
    for (int i = 0; i < 20 && s_connected; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_manual_disconnect && has_creds() && !s_connected) {
        s_connecting = true;
        notify_status();
        esp_wifi_connect();
    }
    if (wait_for_ip(RECOVER_WAIT_MS)) {
        ESP_LOGI(TAG, "network recovered after disconnect+connect: ip=%s", s_ip);
        return;
    }

    ESP_LOGW(TAG, "no IP after disconnect+connect: recovery 2/2 wifi stop+start");
    s_connected = false;
    s_ip[0] = '\0';
    s_connecting = true;
    notify_status();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (wait_for_ip(RECOVER_WAIT_MS)) {
        ESP_LOGI(TAG, "network recovered after stop+start: ip=%s", s_ip);
    } else {
        ESP_LOGE(TAG, "network recovery failed; normal backoff continues");
    }
}

static void select_and_connect(void);

static void wifi_worker_task(void *arg)
{
    (void)arg;
    wifi_cmd_t cmd;
    while (true) {
        if (xQueueReceive(s_worker_q, &cmd, portMAX_DELAY) != pdPASS) {
            continue;
        }
        switch (cmd) {
        case WIFI_CMD_RECONNECT:
            if (!s_manual_disconnect && has_creds() && !s_connected) {
                ESP_LOGI(TAG, "rescan (backoff was %" PRIu32 " ms)...", s_retry_delay_ms);
                select_and_connect();
            }
            break;
        case WIFI_CMD_SELECT:
            if (!s_manual_disconnect && has_creds()) {
                select_and_connect();
            }
            break;
        case WIFI_CMD_AP_ON:
            if (s_connected || s_manual_disconnect || s_ap_enabled || s_ap_user_off ||
                !has_creds()) {
                break;
            }
            ESP_LOGW(TAG, "no IP after %d s (last reason=%u %s), starting SoftAP '%s' for provisioning",
                     AP_FALLBACK_MS / 1000, s_last_reason, reason_hint(s_last_reason), s_ap_ssid);
            if (panda_connectivity_ap(true) == ESP_OK) {
                s_ap_auto = true;
                s_ap_user_on = false;
            }
            break;
        case WIFI_CMD_AP_OFF:
            if (!s_ap_enabled || !s_connected || s_ap_user_on) {
                break;
            }
            ESP_LOGI(TAG, "STA recovered, stopping fallback SoftAP");
            if (panda_connectivity_ap(false) == ESP_OK) {
                /* The automatic shutdown is not a user choice: let the
                 * fallback re-arm if the link drops again later. */
                s_ap_user_off = false;
            }
            break;
        case WIFI_CMD_RECOVER:
            recover_now();
            break;
        default:
            break;
        }
    }
}

static void disarm_ap_fallback(void)
{
    if (s_ap_timer) {
        esp_timer_stop(s_ap_timer);
    }
}

static void arm_ap_fallback(void)
{
    if (!s_ap_timer || s_ap_enabled || s_ap_user_off || !has_creds()) {
        return;
    }
    esp_timer_stop(s_ap_timer);
    esp_timer_start_once(s_ap_timer, (uint64_t)AP_FALLBACK_MS * 1000);
}

static void sntp_sync_cb(struct timeval *tv)
{
    struct tm tm_now;
    localtime_r(&tv->tv_sec, &tm_now);
    panda_board_rtc_write(&tm_now);
    if (s_time_cb) {
        s_time_cb((time_t)tv->tv_sec);
    }
    ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

static esp_err_t start_sntp(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_PANDA_SNTP_SERVER);
    cfg.wait_for_sync = false;
    cfg.start = true;
    cfg.sync_cb = sntp_sync_cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_netif_sntp_start();
    }
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started, server=%s", CONFIG_PANDA_SNTP_SERVER);
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    post_cmd(WIFI_CMD_RECONNECT);
}

static void ap_stop_timer_cb(void *arg)
{
    (void)arg;
    s_ap_enabled = false;
    s_ap_auto = false;
    ESP_LOGI(TAG, "SoftAP stopped");
    notify_status();
}

static void ap_off_timer_cb(void *arg)
{
    (void)arg;
    post_cmd(WIFI_CMD_AP_OFF);
}

static void ap_fallback_timer_cb(void *arg)
{
    (void)arg;
    post_cmd(WIFI_CMD_AP_ON);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (has_creds() && !s_manual_disconnect) {
                s_connecting = true;
                notify_status();
                ESP_LOGI(TAG, "scanning %d saved network(s) for the best match...",
                         s_cred_count);
                post_cmd(WIFI_CMD_SELECT);
                arm_ap_fallback();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *conn = (const wifi_event_sta_connected_t *)data;
            if (conn && conn->channel) {
                s_sta_channel = conn->channel;
            }
            ESP_LOGI(TAG, "associated with '%s' on channel %u, waiting for IP",
                     s_ssid, (unsigned)s_sta_channel);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
            s_last_reason = disc ? disc->reason : 0;
            ESP_LOGW(TAG, "STA disconnected, reason=%u (%s)", (unsigned)s_last_reason,
                     reason_hint(s_last_reason));
            s_connected = false;
            s_ip[0] = '\0';
            if (s_ap_off_timer) {
                esp_timer_stop(s_ap_off_timer);
            }
            if (!s_manual_disconnect && has_creds()) {
                s_connecting = true;
                uint32_t delay_ms = next_retry_delay_ms();
                if (s_retry_timer) {
                    esp_timer_stop(s_retry_timer);
                    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
                }
                ESP_LOGI(TAG, "next reconnect in %" PRIu32 " ms", delay_ms);
                arm_ap_fallback();
            } else {
                s_connecting = false;
            }
            notify_status();
            break;
        }
        case WIFI_EVENT_AP_START: {
            if (s_ap_stop_timer) {
                esp_timer_stop(s_ap_stop_timer);
            }
            s_ap_enabled = true;
            esp_netif_ip_info_t ip = {0};
            char ip_str[16] = "192.168.4.1";
            if (s_netif_ap && esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK && ip.ip.addr) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
            }
            ESP_LOGI(TAG, "SoftAP started: %s%s ip=%s ch=%u",
                     s_ap_ssid, s_ap_auto ? " (auto fallback)" : "", ip_str,
                     (unsigned)(s_sta_channel ? s_sta_channel : 1));
            notify_status();
            break;
        }
        case WIFI_EVENT_AP_STOP:
            if (s_ap_stop_timer) {
                esp_timer_stop(s_ap_stop_timer);
                esp_timer_start_once(s_ap_stop_timer, (uint64_t)AP_STOP_DEBOUNCE_MS * 1000);
            } else {
                s_ap_enabled = false;
                s_ap_auto = false;
                notify_status();
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_connecting = false;
        s_last_reason = 0;
        s_retry_delay_ms = WIFI_RETRY_MIN_MS;
        s_net_err_count = 0;
        s_net_err_first_us = 0;
        disarm_ap_fallback();
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.primary) {
            s_sta_channel = ap.primary;
        }
        ESP_LOGI(TAG, "got IP %s (channel %u)", s_ip, (unsigned)s_sta_channel);
        notify_status();
        start_sntp();
        /* APSTA shares the radio with the STA link; an auto-started fallback
         * AP must not stay up once normal connectivity is back. */
        if (s_ap_enabled && !s_ap_user_on && s_ap_off_timer) {
            esp_timer_stop(s_ap_off_timer);
            esp_timer_start_once(s_ap_off_timer, (uint64_t)AP_OFF_DELAY_MS * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        s_ip[0] = '\0';
        if (!s_manual_disconnect && has_creds()) {
            s_connecting = true;
            arm_ap_fallback();
        }
        notify_status();
    }
}

static esp_err_t configure_ap(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             CONFIG_PANDA_WIFI_AP_PREFIX, mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    size_t ap_len = strnlen(s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_len);
    ap_cfg.ap.ssid_len = (uint8_t)ap_len;
    /* In APSTA the SoftAP must share the STA channel. When the STA link is
     * down (no-credential or fallback provisioning), pin channel 1 instead of
     * a stale channel from the last association: phones in some regions hide
     * or refuse channels 12/13, which made the provisioning AP unreachable. */
    ap_cfg.ap.channel = (s_connected && s_sta_channel) ? s_sta_channel : 1;
    ap_cfg.ap.max_connection = 4;
    const char *pass = CONFIG_PANDA_WIFI_AP_PASSWORD;
    if (strlen(pass) >= 8) {
        snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%.*s",
                 (int)(sizeof(ap_cfg.ap.password) - 1), pass);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static void copy_credentials(const panda_wifi_cred_t *creds, int count)
{
    memset(s_creds, 0, sizeof(s_creds));
    s_cred_count = 0;
    if (!creds || count <= 0) {
        return;
    }
    if (count > PANDA_WIFI_CRED_MAX) {
        count = PANDA_WIFI_CRED_MAX;
    }
    for (int i = 0; i < count; i++) {
        if (!creds[i].ssid[0]) {
            continue;
        }
        s_creds[s_cred_count++] = creds[i];
    }
}

static void connect_cred(int index)
{
    if (index < 0 || index >= s_cred_count) {
        return;
    }
    const panda_wifi_cred_t *cred = &s_creds[index];
    snprintf(s_ssid, sizeof(s_ssid), "%s", cred->ssid);
    s_active_cred = index;

    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%.*s",
             (int)(sizeof(sta_cfg.sta.ssid) - 1), cred->ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%.*s",
             (int)(sizeof(sta_cfg.sta.password) - 1), cred->pass);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    /* Scan every channel and pick the strongest BSSID: multi-AP homes
     * otherwise associate to a weak repeater and drop constantly. */
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    wifi_mode_t mode = s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi mode: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sta config: %s", esp_err_to_name(err));
        return;
    }

    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    s_connecting = true;
    notify_status();
    if (s_connected) {
        esp_wifi_disconnect();
        for (int i = 0; i < 20 && s_connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
    }
}

/* Scan, then join the saved network with the strongest signal. When the scan
 * succeeds but none of the saved SSIDs is present, bring the provisioning AP
 * up right away instead of waiting for the 20 s fallback. */
static void select_and_connect(void)
{
    if (s_manual_disconnect || !has_creds()) {
        return;
    }

    panda_connectivity_ap_t aps[MAX_SCAN_AP];
    int n = panda_connectivity_scan_aps(aps, MAX_SCAN_AP);
    int best = -1;
    int8_t best_rssi = -128;
    if (n > 0) {
        for (int c = 0; c < s_cred_count; c++) {
            for (int i = 0; i < n; i++) {
                if (strcmp(aps[i].ssid, s_creds[c].ssid) == 0 &&
                    aps[i].rssi > best_rssi) {
                    best_rssi = aps[i].rssi;
                    best = c;
                }
            }
        }
    } else {
        /* A failed scan must not send us to provisioning: keep using the
         * last credential and let the normal retry backoff handle it. */
        best = s_active_cred >= 0 ? s_active_cred : 0;
        ESP_LOGW(TAG, "scan unavailable, trying saved network '%s' directly",
                 s_creds[best].ssid);
        connect_cred(best);
        return;
    }

    if (best < 0) {
        ESP_LOGW(TAG, "none of %d saved network(s) in range, provisioning AP on",
                 s_cred_count);
        if (!s_ap_enabled && panda_connectivity_ap(true) == ESP_OK) {
            s_ap_auto = true;
            s_ap_user_on = false;
        }
        return;
    }

    ESP_LOGI(TAG, "selected '%s' (%d dBm, credential %d/%d)",
             s_creds[best].ssid, best_rssi, best + 1, s_cred_count);
    connect_cred(best);
}

esp_err_t panda_connectivity_init(const panda_wifi_cred_t *creds, int count)
{
    if (s_inited) {
        return ESP_OK;
    }

    copy_credentials(creds, count);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_sta || !s_netif_ap) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_set_hostname(s_netif_sta, "panda-pendant");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t country_err = esp_wifi_set_country_code("CN", true);
    if (country_err != ESP_OK) {
        ESP_LOGW(TAG, "country CN: %s", esp_err_to_name(country_err));
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    const esp_timer_create_args_t ap_timer_args = {
        .callback = ap_fallback_timer_cb,
        .name = "wifi_ap_fallback",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ap_timer_args, &s_ap_timer));

    const esp_timer_create_args_t ap_off_timer_args = {
        .callback = ap_off_timer_cb,
        .name = "wifi_ap_off",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ap_off_timer_args, &s_ap_off_timer));

    const esp_timer_create_args_t ap_stop_timer_args = {
        .callback = ap_stop_timer_cb,
        .name = "wifi_ap_stop",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ap_stop_timer_args, &s_ap_stop_timer));

    s_worker_q = xQueueCreate(8, sizeof(wifi_cmd_t));
    if (!s_worker_q) {
        return ESP_ERR_NO_MEM;
    }
    /* PSRAM stack: internal RAM is the scarce resource (weather TLS and the
     * websocket already compete for the ~44 KB left after boot). */
    if (xTaskCreatePinnedToCoreWithCaps(wifi_worker_task, "wifi_worker", 4096, NULL, 5,
                                        &s_worker_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (has_creds()) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_connecting = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(configure_ap());
        s_ap_enabled = true;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* Modem sleep is the main cause of multi-second latency spikes and
     * missed websocket frames; the pendant is mains/USB powered most of the
     * time, so prefer responsiveness over the small idle-current gain. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "disable power save: %s", esp_err_to_name(ps_err));
    }

    s_inited = true;
    if (!has_creds()) {
        ESP_LOGW(TAG, "no WiFi credentials, SoftAP '%s' password '%s'",
                 s_ap_ssid, CONFIG_PANDA_WIFI_AP_PASSWORD);
    } else {
        arm_ap_fallback();
    }
    notify_status();
    return ESP_OK;
}

esp_err_t panda_connectivity_set_credentials(const panda_wifi_cred_t *creds, int count)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    copy_credentials(creds, count);
    if (!has_creds()) {
        return panda_connectivity_forget();
    }
    s_manual_disconnect = false;
    s_ap_user_off = false;
    return panda_connectivity_reconnect_best();
}

esp_err_t panda_connectivity_reconnect_best(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!has_creds()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_manual_disconnect = false;
    s_last_reason = 0;
    s_retry_delay_ms = WIFI_RETRY_MIN_MS;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    s_connecting = true;
    notify_status();
    if (!post_cmd(WIFI_CMD_SELECT)) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}


void panda_connectivity_set_callbacks(panda_connectivity_status_cb_t status_cb,
                                      panda_connectivity_time_cb_t time_cb)
{
    s_status_cb = status_cb;
    s_time_cb = time_cb;
    notify_status();
}

esp_err_t panda_connectivity_disconnect(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_manual_disconnect = true;
    s_connecting = false;
    s_retry_delay_ms = WIFI_RETRY_MIN_MS;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    disarm_ap_fallback();
    if (s_ap_off_timer) {
        esp_timer_stop(s_ap_off_timer);
    }
    esp_wifi_disconnect();
    s_connected = false;
    s_ip[0] = '\0';
    notify_status();
    return ESP_OK;
}

esp_err_t panda_connectivity_forget(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_creds, 0, sizeof(s_creds));
    s_cred_count = 0;
    s_active_cred = -1;
    s_ssid[0] = '\0';
    s_manual_disconnect = true;
    s_retry_delay_ms = WIFI_RETRY_MIN_MS;
    s_ap_user_off = false;
    s_ap_user_on = false;
    s_ap_auto = false;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    disarm_ap_fallback();
    if (s_ap_off_timer) {
        esp_timer_stop(s_ap_off_timer);
    }
    esp_wifi_disconnect();
    s_connected = false;
    s_ip[0] = '\0';
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    configure_ap();
    s_ap_enabled = true;
    notify_status();
    return ESP_OK;
}

esp_err_t panda_connectivity_recover(const char *reason)
{
    if (!s_inited || !s_worker_q) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!has_creds() || s_manual_disconnect) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "network recovery requested (%s)",
             reason && reason[0] ? reason : "manual");
    /* User/CLI requests bypass the cooldown; automatic reports below do not. */
    s_recover_force = true;
    if (!post_cmd(WIFI_CMD_RECOVER)) {
        s_recover_force = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void panda_connectivity_notify_net_error(const char *source)
{
    if (!s_inited || !s_connected || s_manual_disconnect) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_net_err_count == 0 || now - s_net_err_first_us > NET_ERR_WINDOW_US) {
        s_net_err_first_us = now;
        s_net_err_count = 0;
    }
    s_net_err_count++;
    ESP_LOGW(TAG, "outbound path error from %s (%u/%u within %" PRIi64 " s), ip=%s",
             source && source[0] ? source : "?", (unsigned)s_net_err_count,
             (unsigned)NET_ERR_THRESHOLD, NET_ERR_WINDOW_US / 1000000, s_ip);
    if (s_net_err_count < NET_ERR_THRESHOLD) {
        return;
    }
    s_net_err_count = 0;
    if (!post_cmd(WIFI_CMD_RECOVER)) {
        ESP_LOGW(TAG, "recovery queue full");
    }
}

void panda_connectivity_notify_net_ok(void)
{
    s_net_err_count = 0;
    s_net_err_first_us = 0;
}

int panda_connectivity_scan_aps(panda_connectivity_ap_t *aps, int max)
{
    if (!s_inited || !aps || max <= 0) {
        return -1;
    }
    if (max > MAX_SCAN_AP) {
        max = MAX_SCAN_AP;
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(err));
        return -1;
    }

    static wifi_ap_record_t records[MAX_SCAN_AP];
    uint16_t count = (uint16_t)max;
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan records: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return -1;
    }

    int n = 0;
    for (uint16_t i = 0; i < count && n < max; i++) {
        const char *ssid = (const char *)records[i].ssid;
        size_t len = strnlen(ssid, sizeof(records[i].ssid));
        if (len == 0) {
            continue;
        }
        if (len >= sizeof(aps[n].ssid)) {
            len = sizeof(aps[n].ssid) - 1;
        }
        memcpy(aps[n].ssid, ssid, len);
        aps[n].ssid[len] = '\0';
        aps[n].rssi = records[i].rssi;
        aps[n].channel = records[i].primary;
        aps[n].authmode = records[i].authmode;
        n++;
    }
    esp_wifi_clear_ap_list();
    return n;
}

esp_err_t panda_connectivity_scan(void)
{
    panda_connectivity_ap_t aps[MAX_SCAN_AP];
    int count = panda_connectivity_scan_aps(aps, MAX_SCAN_AP);
    if (count < 0) {
        return ESP_FAIL;
    }
    printf("found %d AP(s):\n", count);
    for (int i = 0; i < count; i++) {
        printf("  %3d dBm  ch%2u  %-4s  %s\n",
               aps[i].rssi, aps[i].channel,
               aps[i].authmode == WIFI_AUTH_OPEN ? "open" : "auth", aps[i].ssid);
    }
    return ESP_OK;
}

esp_err_t panda_connectivity_ap(bool enable)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable) {
        s_ap_user_off = false;
        s_ap_user_on = true;
        if (!s_ap_enabled) {
            /* set_config for the AP is rejected in STA-only mode, and applying
             * it to a running AP makes the driver bounce AP_STOP/AP_START.
             * The stop edge is debounced in the event handler below. */
            ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "ap mode");
            ESP_RETURN_ON_ERROR(configure_ap(), TAG, "ap config");
            s_ap_enabled = true;
        }
        disarm_ap_fallback();
    } else {
        s_ap_user_off = true;
        s_ap_user_on = false;
        s_ap_auto = false;
        s_ap_enabled = false;
        disarm_ap_fallback();
        if (s_ap_off_timer) {
            esp_timer_stop(s_ap_off_timer);
        }
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    }
    notify_status();
    return ESP_OK;
}

esp_err_t panda_connectivity_sync_time(void)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (time(NULL) > 1700000000) {
        return ESP_OK;
    }
    esp_err_t err = start_sntp();
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 50; i++) {
        if (time(NULL) > 1700000000) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ESP_ERR_TIMEOUT;
}

bool panda_connectivity_is_connected(void)
{
    return s_connected;
}

bool panda_connectivity_is_ap(void)
{
    return s_ap_enabled;
}

bool panda_connectivity_is_connecting(void)
{
    return s_connecting;
}

const char *panda_connectivity_ip(void)
{
    return s_ip;
}

const char *panda_connectivity_ap_ip(void)
{
    esp_netif_ip_info_t ip = {0};
    if (!s_ap_enabled) {
        s_ap_ip[0] = '\0';
        return s_ap_ip;
    }
    if (s_netif_ap && esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        s_ap_ip[0] = '\0';
    }
    return s_ap_ip;
}

const char *panda_connectivity_ssid(void)
{
    return s_ssid;
}

const char *panda_connectivity_ap_ssid(void)
{
    return s_ap_ssid;
}

esp_err_t panda_connectivity_get_rssi(int *rssi)
{
    if (!s_connected || !rssi) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t ap = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) {
        *rssi = ap.rssi;
    }
    return err;
}

uint8_t panda_connectivity_disconnect_reason(void)
{
    return s_last_reason;
}

const char *panda_connectivity_disconnect_hint(void)
{
    return reason_hint(s_last_reason);
}

uint32_t panda_connectivity_retry_delay_ms(void)
{
    return s_retry_delay_ms;
}

bool panda_connectivity_ap_auto_started(void)
{
    return s_ap_auto;
}
