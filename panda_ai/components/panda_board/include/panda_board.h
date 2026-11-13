#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANDA_BTN_BOOT = 0,
    PANDA_BTN_1 = 1,
    PANDA_BTN_2 = 2,
    PANDA_BTN_3 = 3,
} panda_button_t;

typedef struct {
    bool i2c_ok;
    bool ioexp_ok;
    bool battery_adc_ok;
    bool rtc_ok;
    bool sdcard_ok;
    bool rgb_ok;
    uint32_t boot_count;
} panda_board_info_t;

esp_err_t panda_board_init(void);
const panda_board_info_t *panda_board_get_info(void);

i2c_master_bus_handle_t panda_board_i2c(void);
esp_io_expander_handle_t panda_board_ioexp(void);

/* Reset lines behind TCA9555 (active level true = asserted/low) */
esp_err_t panda_board_lcd_reset(bool active);
esp_err_t panda_board_touch_reset(bool active);
esp_err_t panda_board_pa_enable(bool enable);
esp_err_t panda_board_cam_enable(bool enable);

/* Raw button state with light debounce */
bool panda_board_button_pressed(panda_button_t btn);
bool panda_board_boot_pressed(void);
esp_err_t panda_board_buttons_scan(void);
void panda_board_buttons_get_all(bool out[4]);

/* SD card */
esp_err_t panda_board_sdcard_mount(void);
esp_err_t panda_board_sdcard_unmount(void);
bool panda_board_sdcard_mounted(void);
const char *panda_board_sdcard_mount_point(void);

/* RTC PCF85063 */
esp_err_t panda_board_rtc_init(void);
esp_err_t panda_board_rtc_read(struct tm *out);
esp_err_t panda_board_rtc_write(const struct tm *tm);
bool panda_board_rtc_valid(void);

/* Battery: returns 0 / -1 when unavailable (default hardware) */
uint32_t panda_board_battery_mv(void);
int panda_board_battery_percent(void);
bool panda_board_battery_charging(void);

/* RGB ring (7x WS2812). rgb_write takes count*3 bytes (RGB order) and does a
 * single strip refresh so a whole frame costs one RMT transfer. */
esp_err_t panda_board_rgb_init(void);
bool panda_board_rgb_ready(void);
esp_err_t panda_board_rgb_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t panda_board_rgb_write(const uint8_t *rgb, int count);
esp_err_t panda_board_rgb_off(void);

/* Diagnostics */
void panda_board_print_info(void);
esp_err_t panda_board_i2c_scan(void);
const char *panda_board_get_name(void);

#ifdef __cplusplus
}
#endif
