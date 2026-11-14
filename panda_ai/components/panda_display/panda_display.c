#include "panda_display.h"
#include "panda_board.h"
#include "panda_board_pins.h"
#include "esp_lcd_st77916.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const st77916_lcd_init_cmd_t panda_st77916_vendor_init_cmds[];
extern const size_t panda_st77916_vendor_init_cmds_count;

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static bool s_ready;
static int s_brightness = 80;

#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER LEDC_TIMER_1
#define BL_LEDC_CHANNEL LEDC_CHANNEL_1
#define BL_LEDC_RES LEDC_TIMER_10_BIT

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    ledc_channel_config_t ch = {
        .gpio_num = PANDA_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    return ESP_OK;
}

esp_err_t panda_display_set_brightness(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    if (percent > 0) {
        percent = 5 + (percent * 95) / 100;
    }
    uint32_t duty = (1023u * percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "ledc duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL), TAG, "ledc update");
    s_brightness = percent;
    ESP_LOGD(TAG, "brightness=%d%% duty=%" PRIu32, percent, duty);
    return ESP_OK;
}

int panda_display_get_brightness(void)
{
    return s_brightness;
}

esp_err_t panda_display_prepare_deep_sleep(void)
{
    esp_err_t err = panda_display_set_brightness(0);
    if (s_panel) {
        esp_err_t panel_err = esp_lcd_panel_disp_on_off(s_panel, false);
        if (err == ESP_OK) {
            err = panel_err;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    /* LEDC is off in deep sleep; without a hold the backlight pad floats and
     * can light the panel through its driver. */
    gpio_reset_pin(PANDA_LCD_BL);
    gpio_set_direction(PANDA_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PANDA_LCD_BL, 0);
    gpio_hold_en(PANDA_LCD_BL);
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "display off (backlight held low)");
    return err;
}

esp_err_t panda_display_set_invert(bool invert)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_invert_color(s_panel, invert);
}

static esp_err_t lcd_quspi_init(void)
{
    /* Hardware reset through TCA9555 before the bus is active. */
    if (panda_board_lcd_reset(true) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        panda_board_lcd_reset(false);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_LOGW(TAG, "LCD reset via IO expander unavailable");
    }

    spi_bus_config_t bus = {
        .sclk_io_num = PANDA_LCD_SCLK,
        .data0_io_num = PANDA_LCD_D0,
        .data1_io_num = PANDA_LCD_D1,
        .data2_io_num = PANDA_LCD_D2,
        .data3_io_num = PANDA_LCD_D3,
        .max_transfer_sz = PANDA_LCD_MAX_TRANSFER,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(PANDA_LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "qspi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PANDA_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)PANDA_LCD_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    st77916_vendor_config_t vendor = {
        .init_cmds = panda_st77916_vendor_init_cmds,
        .init_cmds_size = panda_st77916_vendor_init_cmds_count,
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = PANDA_LCD_BPP,
        .flags.reset_active_high = 0,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel), TAG, "panel create");

    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
    ESP_LOGI(TAG, "ST77916 init done in %lld ms (%dx%d, QSPI 80MHz, vendor cmds=%d)",
             (long long)((esp_timer_get_time() - t0) / 1000), PANDA_LCD_H_RES, PANDA_LCD_V_RES,
             (int)panda_st77916_vendor_init_cmds_count);
    return ESP_OK;
}

static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 12 * 1024,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        .buffer_size = PANDA_LCD_H_RES * 10,
        .double_buffer = true,
        .hres = PANDA_LCD_H_RES,
        .vres = PANDA_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL display registered (buffer=%d px x2, internal)", PANDA_LCD_H_RES * 10);
    return ESP_OK;
}

esp_err_t panda_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    /* Release the deep-sleep hold applied by panda_display_prepare_deep_sleep. */
    gpio_hold_dis(PANDA_LCD_BL);
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init");
    int target_brightness = s_brightness;
    panda_display_set_brightness(0);
    ESP_RETURN_ON_ERROR(lcd_quspi_init(), TAG, "lcd init");
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl init");
    s_ready = true;
    panda_display_set_brightness(target_brightness);
    return ESP_OK;
}

bool panda_display_ready(void)
{
    return s_ready;
}

lv_display_t *panda_display_lvgl(void)
{
    return s_disp;
}

esp_lcd_panel_handle_t panda_display_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t panda_display_panel_io(void)
{
    return s_io;
}

static TaskHandle_t s_lock_task;
static const char *s_lock_site = "?";
static int64_t s_lock_since_us;
static int s_lock_depth;

bool panda_display_lock_at(uint32_t timeout_ms, const char *site)
{
    int64_t t0 = esp_timer_get_time();
    if (!lvgl_port_lock(timeout_ms)) {
        ESP_LOGE(TAG, "lock timeout %ums: waiter=%s holder=%s", (unsigned)timeout_ms,
                 pcTaskGetName(NULL),
                 s_lock_depth ? pcTaskGetName(s_lock_task) : "lvgl-task");
        return false;
    }
    if (s_lock_depth == 0) {
        s_lock_task = xTaskGetCurrentTaskHandle();
        s_lock_site = site;
        s_lock_since_us = t0;
    }
    s_lock_depth++;
    return true;
}

bool panda_display_lock(uint32_t timeout_ms)
{
    return panda_display_lock_at(timeout_ms, "?");
}

void panda_display_unlock(void)
{
    if (s_lock_depth > 0) {
        s_lock_depth--;
        if (s_lock_depth == 0) {
            int64_t held = esp_timer_get_time() - s_lock_since_us;
            if (held > 500000) {
                ESP_LOGW(TAG, "display lock held %lld ms by %s", (long long)(held / 1000), s_lock_site);
            }
            s_lock_task = NULL;
        }
    }
    lvgl_port_unlock();
}

void panda_display_lock_info(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (s_lock_depth <= 0) {
        /* Held inside the lvgl task (timer/event callback) or never taken. */
        snprintf(buf, len, "holder=lvgl-task(no panda_display_lock holder recorded)");
        return;
    }
    snprintf(buf, len, "holder=%s site=%s held=%lldms depth=%d",
             pcTaskGetName(s_lock_task), s_lock_site,
             (long long)((esp_timer_get_time() - s_lock_since_us) / 1000), s_lock_depth);
}

esp_err_t panda_display_fill_rgb565(uint16_t color)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    const int lines = 40;
    size_t bytes = PANDA_LCD_H_RES * lines * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < PANDA_LCD_H_RES * lines; i++) {
        buf[i] = color;
    }
    esp_err_t err = ESP_OK;
    for (int y = 0; y < PANDA_LCD_V_RES; y += lines) {
        int y2 = y + lines > PANDA_LCD_V_RES ? PANDA_LCD_V_RES : y + lines;
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, PANDA_LCD_H_RES, y2, buf);
        if (err != ESP_OK) {
            break;
        }
    }
    heap_caps_free(buf);
    return err;
}
