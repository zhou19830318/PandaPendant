#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "app_core.h"
#include "app_settings.h"
#include "serial_cli.h"
#include "panda_board.h"
#include "panda_audio.h"
#include "panda_display.h"
#include "panda_touch.h"
#include "panda_ui.h"
#include "panda_connectivity.h"
#include "app_provisioning.h"
#include "app_weather.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Panda AI pendant booting");

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(panda_settings_init());
    panda_settings_print(false);

    ESP_ERROR_CHECK(panda_board_init());
    panda_board_print_info();

    err = panda_audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %s", esp_err_to_name(err));
    } else {
        panda_audio_set_volume(panda_settings_get()->volume);
        panda_audio_mute(panda_settings_get()->dnd != 0);
    }

    err = panda_display_init();
    if (err == ESP_OK) {
        panda_display_set_brightness(panda_settings_get()->brightness);
        err = panda_touch_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch unavailable (screen not connected?): %s", esp_err_to_name(err));
        }
        err = panda_ui_init();
        if (err == ESP_OK) {
            panda_ui_set_volume(panda_settings_get()->volume,
                                panda_settings_get()->volume == 0);
            panda_ui_set_brightness(panda_settings_get()->brightness);
        } else {
            ESP_LOGW(TAG, "UI unavailable: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "display unavailable, UI disabled: %s", esp_err_to_name(err));
    }

    /* Start the console before app_core: the voice websocket task (8 KB) and
     * its three audio tasks fragment the internal heap enough that the 6 KB
     * REPL task can no longer be allocated afterwards. */
#if CONFIG_PANDA_SERIAL_CLI
    err = serial_cli_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "serial CLI unavailable: %s", esp_err_to_name(err));
    }
#endif

    ESP_ERROR_CHECK(app_core_start());
    panda_touch_set_callback(app_core_post_touch);

    panda_connectivity_set_callbacks(app_core_set_wifi_status, app_core_set_time_from_epoch);
    const panda_settings_t *wifi_cfg = panda_settings_get();
    err = panda_connectivity_init(wifi_cfg->wifi, wifi_cfg->wifi_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connectivity init failed: %s", esp_err_to_name(err));
    }

    err = app_weather_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "weather task unavailable: %s", esp_err_to_name(err));
    }

#if CONFIG_PANDA_PROVISIONING
    /* The page is served whenever the device has a network: over the SoftAP
     * at 192.168.4.1 or over the LAN at the STA IP (music upload, WiFi and
     * voice settings). app_core stops it only when both are down. */
    err = app_provisioning_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provisioning unavailable: %s", esp_err_to_name(err));
    }
#endif

    ESP_LOGI(TAG, "boot complete, free heap=%u", (unsigned)esp_get_free_heap_size());
}
