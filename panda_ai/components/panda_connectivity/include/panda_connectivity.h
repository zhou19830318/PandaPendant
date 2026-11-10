#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*panda_connectivity_status_cb_t)(bool connected, bool ap, bool connecting);
typedef void (*panda_connectivity_time_cb_t)(time_t epoch);

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} panda_connectivity_ap_t;

esp_err_t panda_connectivity_init(const char *ssid, const char *password);
void panda_connectivity_set_callbacks(panda_connectivity_status_cb_t status_cb,
                                      panda_connectivity_time_cb_t time_cb);

esp_err_t panda_connectivity_connect(const char *ssid, const char *password);
esp_err_t panda_connectivity_disconnect(void);
esp_err_t panda_connectivity_forget(void);
esp_err_t panda_connectivity_scan(void);
int panda_connectivity_scan_aps(panda_connectivity_ap_t *aps, int max);
esp_err_t panda_connectivity_ap(bool enable);
esp_err_t panda_connectivity_sync_time(void);

/* Data-path wedge recovery: the driver can keep the STA association (and the
 * same IP) while ARP/TCP silently die, so every reconnect times out until the
 * interface is bounced. Applications report failed outbound connects here;
 * after a few consecutive failures the connectivity task runs
 * disconnect+connect, escalating to stop+start if no IP comes back. */
esp_err_t panda_connectivity_recover(const char *reason);
void panda_connectivity_notify_net_error(const char *source);
void panda_connectivity_notify_net_ok(void);

bool panda_connectivity_is_connected(void);
bool panda_connectivity_is_ap(void);
bool panda_connectivity_is_connecting(void);
const char *panda_connectivity_ip(void);
const char *panda_connectivity_ap_ip(void);
const char *panda_connectivity_ssid(void);
const char *panda_connectivity_ap_ssid(void);
esp_err_t panda_connectivity_get_rssi(int *rssi);

uint8_t panda_connectivity_disconnect_reason(void);
const char *panda_connectivity_disconnect_hint(void);
uint32_t panda_connectivity_retry_delay_ms(void);
bool panda_connectivity_ap_auto_started(void);

#ifdef __cplusplus
}
#endif
