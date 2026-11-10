#include "panda_touch.h"
#include "panda_board.h"
#include "panda_board_pins.h"
#include "panda_display.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_panel_io.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define CST816S_REG_DATA 0x02
#define CST816S_REG_CHIP_ID 0xA7
#define CST816S_REG_DIS_AUTO_SLEEP 0xFE

static esp_lcd_touch_handle_t s_tp;
static esp_lcd_panel_io_handle_t s_io;
static panda_touch_state_t s_state;
static panda_touch_gesture_cb_t s_cb;

static int64_t s_press_us;
static int s_press_x;
static int s_press_y;
static int64_t s_last_tap_us;
static int s_last_tap_x;
static int s_last_tap_y;
static bool s_long_sent;
static bool s_disabled;
static bool s_logs_muted;
static uint32_t s_fail_total;
static uint32_t s_ok_streak;

static void emit(panda_touch_gesture_t g, int x, int y)
{
    s_state.gesture_count++;
    s_state.last_gesture = g;
    s_state.last_gesture_x = x;
    s_state.last_gesture_y = y;
    ESP_LOGI(TAG, "gesture=%s at (%d,%d)", panda_touch_gesture_name(g), x, y);
    if (s_cb) {
        s_cb(g, x, y);
    }
}

static esp_err_t touch_write_reg(uint8_t reg, uint8_t value)
{
    return esp_lcd_panel_io_tx_param(s_io, reg, &value, 1);
}

static esp_err_t touch_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return esp_lcd_panel_io_rx_param(s_io, reg, buf, len);
}

static void touch_hw_reset(void)
{
    panda_board_touch_reset(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    panda_board_touch_reset(false);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* CST816S auto-sleeps after a short idle period; in that state the address
 * still ACKs but register reads NACK until the chip is reset. Disable it so
 * 30 ms polling keeps working without touch activity. */
static bool touch_wake(void)
{
    esp_err_t werr = touch_write_reg(CST816S_REG_DIS_AUTO_SLEEP, 0x01);
    uint8_t data[6] = {0};
    esp_err_t rerr = touch_read_reg(CST816S_REG_DATA, data, sizeof(data));
    ESP_LOGD(TAG, "wake: write=%s read=%s", esp_err_to_name(werr), esp_err_to_name(rerr));
    return werr == ESP_OK && rerr == ESP_OK;
}

static bool touch_recover(void)
{
    touch_hw_reset();
    return touch_wake();
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (s_disabled) {
        /* Retry every ~3 s so a panel that browns out or resets can come
         * back without a reboot. */
        if (++s_ok_streak >= 100) {
            s_ok_streak = 0;
            if (touch_recover()) {
                s_disabled = false;
                s_fail_total = 0;
                s_logs_muted = false;
                esp_log_level_set("CST816S", ESP_LOG_ERROR);
                esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_ERROR);
                esp_log_level_set("i2c.master", ESP_LOG_ERROR);
                ESP_LOGI(TAG, "touch recovered after reset");
            }
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_err_t err = esp_lcd_touch_read_data(s_tp);
    if (err != ESP_OK) {
        /* A flaky panel NACKs almost every poll; the driver logs each failure
         * at ERROR, which at 50 Hz saturates the UART and buries voice logs.
         * Mute after a short burst and give up once the panel has failed 100
         * times — a reboot retries after rewiring. A healthy run of 500 polls
         * (8 s) restores logging if it recovers. */
        s_ok_streak = 0;
        s_fail_total++;
        if (!s_logs_muted && s_fail_total >= 10) {
            esp_log_level_set("CST816S", ESP_LOG_NONE);
            esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
            esp_log_level_set("i2c.master", ESP_LOG_NONE);
            s_logs_muted = true;
            ESP_LOGW(TAG, "touch I2C failing, muting driver errors");
        }
        if (s_fail_total >= 100) {
            s_disabled = true;
            if (s_logs_muted) {
                /* No more touch polls: restore the shared I2C tags so codec
                 * and expander errors are visible again. */
                esp_log_level_set("CST816S", ESP_LOG_ERROR);
                esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_ERROR);
                esp_log_level_set("i2c.master", ESP_LOG_ERROR);
                s_logs_muted = false;
            }
            ESP_LOGW(TAG, "CST816S disabled (%u failed polls)", (unsigned)s_fail_total);
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (s_logs_muted) {
        if (++s_ok_streak >= 500) {
            esp_log_level_set("CST816S", ESP_LOG_ERROR);
            esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_ERROR);
            esp_log_level_set("i2c.master", ESP_LOG_ERROR);
            s_logs_muted = false;
            s_fail_total = 0;
            ESP_LOGI(TAG, "touch I2C healthy again, logging restored");
        }
    }

    uint16_t x = 0, y = 0, strength = 0;
    uint8_t count = 1;
    bool pressed = esp_lcd_touch_get_coordinates(s_tp, &x, &y, &strength, &count, 1);
    int64_t now = esp_timer_get_time();

    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        s_state.pressed = true;
        s_state.x = x;
        s_state.y = y;

        if (s_press_us == 0) {
            s_press_us = now;
            s_press_x = x;
            s_press_y = y;
            s_long_sent = false;
        } else if (!s_long_sent && (now - s_press_us) > 600000) {
            s_long_sent = true;
            emit(PANDA_TOUCH_LONG_PRESS, s_press_x, s_press_y);
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        if (s_state.pressed && s_press_us != 0) {
            int64_t dt = now - s_press_us;
            int dx = (int)x - s_press_x;
            int dy = (int)y - s_press_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            s_state.pressed = false;

            if (!s_long_sent) {
                if (adx > 45 || ady > 45) {
                    if (adx > ady) {
                        emit(dx > 0 ? PANDA_TOUCH_SWIPE_RIGHT : PANDA_TOUCH_SWIPE_LEFT, x, y);
                    } else {
                        emit(dy > 0 ? PANDA_TOUCH_SWIPE_DOWN : PANDA_TOUCH_SWIPE_UP, x, y);
                    }
                } else if (dt < 450000) {
                    s_state.tap_count++;
                    if (s_last_tap_us != 0 && (now - s_last_tap_us) < 350000 &&
                        abs((int)x - s_last_tap_x) < 40 && abs((int)y - s_last_tap_y) < 40) {
                        emit(PANDA_TOUCH_DOUBLE_TAP, x, y);
                        s_last_tap_us = 0;
                    } else {
                        emit(PANDA_TOUCH_TAP, x, y);
                        s_last_tap_us = now;
                        s_last_tap_x = x;
                        s_last_tap_y = y;
                    }
                }
            }
            s_press_us = 0;
        }
    }
}

static bool probe_touch(void)
{
    /* The CST816S driver logs every I2C NACK at ERROR; an unpopulated panel
     * would flood the console (~50 lines/s, more than the UART can carry)
     * and drown the voice logs. Silence it during the probe, then restore. */
    esp_log_level_set("CST816S", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    bool present = false;
    for (int attempt = 0; attempt < 3 && !present; attempt++) {
        if (attempt > 0) {
            touch_hw_reset();
        }
        present = touch_wake();
        if (!present) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    esp_log_level_set("CST816S", ESP_LOG_ERROR);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_ERROR);
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    return present;
}

void panda_touch_diag(void)
{
    if (!s_io) {
        ESP_LOGW(TAG, "touch not initialised");
        return;
    }
    esp_log_level_set("CST816S", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    uint8_t id = 0;
    uint8_t data[6] = {0};
    esp_err_t id_err = touch_read_reg(CST816S_REG_CHIP_ID, &id, 1);
    esp_err_t data_err = touch_read_reg(CST816S_REG_DATA, data, sizeof(data));
    ESP_LOGI(TAG, "diag: id=%s id=%u, data=%s num=%u",
             esp_err_to_name(id_err), id, esp_err_to_name(data_err), data[0]);

    esp_err_t sleep_err = touch_write_reg(CST816S_REG_DIS_AUTO_SLEEP, 0x01);
    vTaskDelay(pdMS_TO_TICKS(1000));
    data_err = touch_read_reg(CST816S_REG_DATA, data, sizeof(data));
    ESP_LOGI(TAG, "diag: disable-autosleep=%s, after 1s data=%s num=%u",
             esp_err_to_name(sleep_err), esp_err_to_name(data_err), data[0]);

    touch_hw_reset();
    sleep_err = touch_write_reg(CST816S_REG_DIS_AUTO_SLEEP, 0x01);
    vTaskDelay(pdMS_TO_TICKS(1000));
    data_err = touch_read_reg(CST816S_REG_DATA, data, sizeof(data));
    ESP_LOGI(TAG, "diag: after reset+sleep-off, 1s later data=%s num=%u (write=%s)",
             esp_err_to_name(data_err), data[0], esp_err_to_name(sleep_err));

    esp_log_level_set("CST816S", ESP_LOG_ERROR);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_ERROR);
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
}

esp_err_t panda_touch_init(void)
{
    if (s_tp) {
        return ESP_OK;
    }
    panda_board_touch_reset(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    panda_board_touch_reset(false);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags.disable_control_phase = 1,
        .scl_speed_hz = PANDA_I2C_FREQ_HZ,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(panda_board_i2c(), &io_cfg, &io), TAG, "touch io");
    s_io = io;

    esp_lcd_touch_config_t cfg = {
        .x_max = PANDA_LCD_H_RES,
        .y_max = PANDA_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(io, &cfg, &s_tp), TAG, "touch init");

    bool present = probe_touch();
    s_disabled = !present;
    if (!present) {
        /* Keep the indev alive: touch_read_cb retries a reset every ~3 s, so
         * the panel can recover without a reboot once it responds again. */
        ESP_LOGW(TAG, "CST816S not responding on I2C; retrying with reset");
    }

    if (!panda_display_lock(1000)) {
        ESP_LOGE(TAG, "display lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, panda_display_lvgl());
    panda_display_unlock();

    if (present) {
        ESP_LOGI(TAG, "CST816S ready @0x%02X (auto-sleep off), indev=%p",
                 PANDA_TOUCH_ADDR, (void *)indev);
    }
    return ESP_OK;
}

void panda_touch_set_callback(panda_touch_gesture_cb_t cb)
{
    s_cb = cb;
}

const panda_touch_state_t *panda_touch_get_state(void)
{
    return &s_state;
}

const char *panda_touch_gesture_name(panda_touch_gesture_t gesture)
{
    switch (gesture) {
    case PANDA_TOUCH_TAP: return "tap";
    case PANDA_TOUCH_DOUBLE_TAP: return "double_tap";
    case PANDA_TOUCH_LONG_PRESS: return "long_press";
    case PANDA_TOUCH_SWIPE_LEFT: return "swipe_left";
    case PANDA_TOUCH_SWIPE_RIGHT: return "swipe_right";
    case PANDA_TOUCH_SWIPE_UP: return "swipe_up";
    case PANDA_TOUCH_SWIPE_DOWN: return "swipe_down";
    default: return "?";
    }
}
