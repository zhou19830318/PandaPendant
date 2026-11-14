#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t panda_display_init(void);
bool panda_display_ready(void);

lv_display_t *panda_display_lvgl(void);
esp_lcd_panel_handle_t panda_display_panel(void);
esp_lcd_panel_io_handle_t panda_display_panel_io(void);

esp_err_t panda_display_set_brightness(int percent);
int panda_display_get_brightness(void);
esp_err_t panda_display_set_invert(bool invert);
/* Panel off + backlight pad held low; call immediately before deep sleep. */
esp_err_t panda_display_prepare_deep_sleep(void);

bool panda_display_lock(uint32_t timeout_ms);
/* Same as panda_display_lock(), recording the caller's site name so a stuck
 * lock can be attributed to the task/function holding it. */
bool panda_display_lock_at(uint32_t timeout_ms, const char *site);
void panda_display_unlock(void);
/* One-line description of the current lock holder (for hang reports). */
void panda_display_lock_info(char *buf, size_t len);

/* Raw diagnostic fill; caller must hold the display lock. */
esp_err_t panda_display_fill_rgb565(uint16_t color);

#ifdef __cplusplus
}
#endif
