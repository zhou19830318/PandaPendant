#include "panda_board.h"
#include "panda_board_pins.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_PANDA_BATTERY_ADC
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_ioexp;
static i2c_master_dev_handle_t s_rtc;
static sdmmc_card_t *s_card;
static led_strip_handle_t s_rgb;
static panda_board_info_t s_info;
static bool s_btn_stable[4];
static bool s_btn_raw[4];
static bool s_rtc_last_valid;

#if CONFIG_PANDA_BATTERY_ADC
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_calibrated;
#define BAT_ADC_CH ADC_CHANNEL_0 /* GPIO1 */

static int battery_pin_mv(void)
{
    if (!s_adc) {
        return -1;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CH, &raw) != ESP_OK) {
        return -1;
    }
    int mv = 0;
    if (s_adc_calibrated) {
        if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) != ESP_OK) {
            mv = raw * 3300 / 4095;
        }
    } else {
        mv = raw * 3300 / 4095;
    }
    return mv;
}
#endif

static esp_err_t board_i2c_init(void)
{
    if (s_i2c) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = PANDA_I2C_PORT,
        .sda_io_num = PANDA_I2C_SDA,
        .scl_io_num = PANDA_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c bus");
    ESP_LOGI(TAG, "I2C0 ready SCL=%d SDA=%d @%dHz", PANDA_I2C_SCL, PANDA_I2C_SDA, PANDA_I2C_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t board_ioexp_init(void)
{
    if (s_ioexp) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca95xx_16bit(s_i2c, PANDA_IOEXP_ADDR, &s_ioexp),
                        TAG, "tca9555 init");

    const uint32_t out_mask = (1u << PANDA_IOEXP_LCD_RST) | (1u << PANDA_IOEXP_TP_RST) |
                              (1u << PANDA_IOEXP_PA_EN) | (1u << PANDA_IOEXP_CAM_EN) |
                              (1u << PANDA_IOEXP_CAM_MUX);
    const uint32_t in_mask = (1u << PANDA_IOEXP_TP_INT) | (1u << PANDA_IOEXP_BTN1) |
                             (1u << PANDA_IOEXP_BTN2) | (1u << PANDA_IOEXP_BTN3);

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_ioexp, in_mask, IO_EXPANDER_INPUT), TAG, "ioexp in");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_ioexp, out_mask, IO_EXPANDER_OUTPUT), TAG, "ioexp out");

    /* TCA9555 output latches default to 0xFF; drive the final safe levels
     * only after the pins are outputs (set_level rejects input pins). */
    esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_LCD_RST, 1);
    esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_TP_RST, 1);
    esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_PA_EN, 0);
    esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_CAM_EN, 0);
    esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_CAM_MUX, 0);

    uint32_t level = 0;
    esp_err_t err = esp_io_expander_get_level(s_ioexp, 0xFFFF, &level);
    ESP_LOGI(TAG, "TCA9555 ready @0x%02X level=0x%04" PRIx32 " (%s)", PANDA_IOEXP_ADDR, level,
             err == ESP_OK ? "ok" : esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t panda_board_init(void)
{
    memset(&s_info, 0, sizeof(s_info));
    ESP_LOGI(TAG, "==== %s init ====", PANDA_BOARD_NAME);

    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");
    s_info.i2c_ok = true;

    if (board_ioexp_init() == ESP_OK) {
        s_info.ioexp_ok = true;
    } else {
        ESP_LOGE(TAG, "TCA9555 missing: LCD/touch reset and PA control unavailable");
    }

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << PANDA_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_cfg));

    if (panda_board_rtc_init() == ESP_OK) {
        s_info.rtc_ok = true;
    }
    if (panda_board_rgb_init() == ESP_OK) {
        s_info.rgb_ok = true;
    }

#if CONFIG_PANDA_BATTERY_ADC
    adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&adc_cfg, &s_adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_config_channel(s_adc, BAT_ADC_CH, &chan_cfg) == ESP_OK) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_curve_fitting_config_t cali_cfg = {
                .unit_id = ADC_UNIT_1,
                .chan = BAT_ADC_CH,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
                s_adc_calibrated = true;
            }
#endif
            /* R53/R78 0R links are unpopulated by default, leaving GPIO1
             * floating. A ~45k internal pull-down collapses a floating pin
             * to nearly 0 V, while the 200K/100K divider (67k Thevenin)
             * only sags to ~0.5 V, so this tells "wired" from "not fitted". */
            gpio_set_pull_mode(PANDA_BAT_ADC_GPIO, GPIO_PULLDOWN_ONLY);
            vTaskDelay(pdMS_TO_TICKS(20));
            int pin_pd = battery_pin_mv();
            gpio_set_pull_mode(PANDA_BAT_ADC_GPIO, GPIO_FLOATING);
            vTaskDelay(pdMS_TO_TICKS(20));
            int pin_open = battery_pin_mv();
            s_info.battery_adc_ok = pin_pd >= 200;
            if (s_info.battery_adc_ok) {
                ESP_LOGI(TAG, "Battery ADC on GPIO1 (pulldown=%d mV open=%d mV)", pin_pd, pin_open);
            } else {
                ESP_LOGW(TAG, "Battery sense not wired: GPIO1 pulldown=%d mV (fit R53/R78 0R links)",
                         pin_pd);
            }
        }
    }
    if (!s_info.battery_adc_ok) {
        ESP_LOGW(TAG, "Battery ADC unavailable (hardware 0R rework required)");
    }
#else
    ESP_LOGW(TAG, "Battery ADC disabled (soldering 0R on GPIO1 required)");
#endif

    /* SD is optional; never block boot on a missing card. */
    panda_board_sdcard_mount();

    panda_board_buttons_scan();
    panda_board_print_info();
    return ESP_OK;
}

const panda_board_info_t *panda_board_get_info(void)
{
    return &s_info;
}

i2c_master_bus_handle_t panda_board_i2c(void)
{
    return s_i2c;
}

esp_io_expander_handle_t panda_board_ioexp(void)
{
    return s_ioexp;
}

esp_err_t panda_board_lcd_reset(bool active)
{
    if (!s_ioexp) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_LCD_RST, active ? 0 : 1);
}

esp_err_t panda_board_touch_reset(bool active)
{
    if (!s_ioexp) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_TP_RST, active ? 0 : 1);
}

esp_err_t panda_board_pa_enable(bool enable)
{
    if (!s_ioexp) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_PA_EN, enable ? 1 : 0);
}

esp_err_t panda_board_cam_enable(bool enable)
{
    if (!s_ioexp) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(s_ioexp, 1u << PANDA_IOEXP_CAM_EN, enable ? 1 : 0);
}

esp_err_t panda_board_buttons_scan(void)
{
    bool pressed[4] = {false, false, false, false};
    pressed[PANDA_BTN_BOOT] = gpio_get_level(PANDA_BOOT_BUTTON) == 0;

    if (s_ioexp) {
        uint32_t level = 0;
        if (esp_io_expander_get_level(s_ioexp, 0xFFFF, &level) == ESP_OK) {
            pressed[PANDA_BTN_1] = (level & (1u << PANDA_IOEXP_BTN1)) == 0;
            pressed[PANDA_BTN_2] = (level & (1u << PANDA_IOEXP_BTN2)) == 0;
            pressed[PANDA_BTN_3] = (level & (1u << PANDA_IOEXP_BTN3)) == 0;
        }
    }

    for (int i = 0; i < 4; i++) {
        if (pressed[i] == s_btn_raw[i]) {
            s_btn_stable[i] = pressed[i];
        }
        s_btn_raw[i] = pressed[i];
    }
    return ESP_OK;
}

bool panda_board_button_pressed(panda_button_t btn)
{
    if (btn > PANDA_BTN_3) {
        return false;
    }
    return s_btn_stable[btn];
}

bool panda_board_boot_pressed(void)
{
    return panda_board_button_pressed(PANDA_BTN_BOOT);
}

void panda_board_buttons_get_all(bool out[4])
{
    memcpy(out, s_btn_stable, sizeof(s_btn_stable));
}

esp_err_t panda_board_sdcard_mount(void)
{
    if (s_card) {
        s_info.sdcard_ok = true;
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PANDA_SD_CLK;
    slot.cmd = PANDA_SD_CMD;
    slot.d0 = PANDA_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(PANDA_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_info.sdcard_ok = false;
        ESP_LOGW(TAG, "SD card not mounted (%s) - animation assets unavailable", esp_err_to_name(err));
        return err;
    }
    s_info.sdcard_ok = true;
    ESP_LOGI(TAG, "SD card mounted at %s", PANDA_SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t panda_board_sdcard_unmount(void)
{
    if (!s_card) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_fat_sdcard_unmount(PANDA_SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        s_info.sdcard_ok = false;
    }
    return err;
}

bool panda_board_sdcard_mounted(void)
{
    return s_card != NULL;
}

const char *panda_board_sdcard_mount_point(void)
{
    return PANDA_SD_MOUNT_POINT;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

static uint8_t bin_to_bcd(uint8_t v)
{
    return ((v / 10) << 4) | (v % 10);
}

static bool rtc_regs_valid(const uint8_t r[7])
{
    for (int i = 0; i < 7; i++) {
        if (r[i] == 0xFF) {
            return false;
        }
    }
    uint8_t sec = bcd_to_bin(r[0] & 0x7F);
    uint8_t min = bcd_to_bin(r[1] & 0x7F);
    uint8_t hour = bcd_to_bin(r[2] & 0x3F);
    uint8_t day = bcd_to_bin(r[3] & 0x3F);
    uint8_t mon = bcd_to_bin(r[5] & 0x1F);
    uint8_t year = bcd_to_bin(r[6]);
    return sec < 60 && min < 60 && hour < 24 && day >= 1 && day <= 31 &&
           mon >= 1 && mon <= 12 && year >= 24 && year <= 99;
}

esp_err_t panda_board_rtc_init(void)
{
    if (!s_i2c) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rtc) {
        return ESP_OK;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PANDA_RTC_ADDR,
        .scl_speed_hz = PANDA_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c, &cfg, &s_rtc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 add device failed: %s", esp_err_to_name(err));
        s_rtc = NULL;
        return err;
    }
    uint8_t regs[7] = {0};
    uint8_t reg = 0x04;
    err = i2c_master_transmit_receive(s_rtc, &reg, 1, regs, sizeof(regs), 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not responding at 0x%02X: %s", PANDA_RTC_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = NULL;
        return err;
    }
    s_rtc_last_valid = rtc_regs_valid(regs);
    ESP_LOGI(TAG, "PCF85063 ready, time_valid=%d", s_rtc_last_valid);
    return ESP_OK;
}

esp_err_t panda_board_rtc_read(struct tm *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_rtc) {
        s_rtc_last_valid = false;
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t regs[7] = {0};
    uint8_t reg = 0x04;
    esp_err_t err = i2c_master_transmit_receive(s_rtc, &reg, 1, regs, sizeof(regs), 100);
    if (err != ESP_OK) {
        s_rtc_last_valid = false;
        return err;
    }
    s_rtc_last_valid = rtc_regs_valid(regs);
    if (!s_rtc_last_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    out->tm_sec = bcd_to_bin(regs[0] & 0x7F);
    out->tm_min = bcd_to_bin(regs[1] & 0x7F);
    uint8_t hour = regs[2];
    if (hour & 0x40) { /* 12-hour mode */
        int h = bcd_to_bin(hour & 0x1F);
        bool pm = hour & 0x20;
        if (h == 12) {
            h = 0;
        }
        out->tm_hour = pm ? h + 12 : h;
    } else {
        out->tm_hour = bcd_to_bin(hour & 0x3F);
    }
    out->tm_mday = bcd_to_bin(regs[3] & 0x3F);
    out->tm_wday = bcd_to_bin(regs[4] & 0x07);
    out->tm_mon = bcd_to_bin(regs[5] & 0x1F) - 1;
    out->tm_year = 100 + bcd_to_bin(regs[6]); /* 2000 + year */
    out->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t panda_board_rtc_write(const struct tm *tm)
{
    if (!tm) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rtc) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tm->tm_year < 100 || tm->tm_year > 199) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t regs[8];
    regs[0] = 0x04;
    regs[1] = bin_to_bcd(tm->tm_sec);
    regs[2] = bin_to_bcd(tm->tm_min);
    regs[3] = bin_to_bcd(tm->tm_hour);
    regs[4] = bin_to_bcd(tm->tm_mday);
    regs[5] = bin_to_bcd(tm->tm_wday);
    regs[6] = bin_to_bcd(tm->tm_mon + 1);
    regs[7] = bin_to_bcd(tm->tm_year - 100);

    uint8_t stop[2] = {0x00, 0x20};
    i2c_master_transmit(s_rtc, stop, sizeof(stop), 100);
    esp_err_t err = i2c_master_transmit(s_rtc, regs, sizeof(regs), 100);
    uint8_t run[2] = {0x00, 0x00};
    i2c_master_transmit(s_rtc, run, sizeof(run), 100);
    if (err == ESP_OK) {
        s_rtc_last_valid = true;
    }
    return err;
}

bool panda_board_rtc_valid(void)
{
    return s_rtc != NULL && s_rtc_last_valid;
}

uint32_t panda_board_battery_mv(void)
{
#if CONFIG_PANDA_BATTERY_ADC
    if (!s_info.battery_adc_ok) {
        return 0;
    }
    int mv = battery_pin_mv();
    if (mv <= 0) {
        return 0;
    }
    uint32_t ratio = CONFIG_PANDA_BATTERY_DIVIDER_MILLI;
    return (uint32_t)((mv * ratio + 500) / 1000);
#else
    return 0;
#endif
}

int panda_board_battery_percent(void)
{
    uint32_t mv = panda_board_battery_mv();
    if (mv == 0) {
        return -1;
    }
    static const struct {
        uint16_t mv;
        uint8_t pct;
    } curve[] = {
        {3300, 0}, {3450, 5}, {3550, 12}, {3650, 30}, {3750, 55},
        {3850, 72}, {3950, 85}, {4050, 94}, {4200, 100},
    };
    if (mv <= curve[0].mv) {
        return curve[0].pct;
    }
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (mv <= curve[i].mv) {
            int span_mv = curve[i].mv - curve[i - 1].mv;
            int span_pct = curve[i].pct - curve[i - 1].pct;
            return curve[i - 1].pct + (int)((mv - curve[i - 1].mv) * span_pct / span_mv);
        }
    }
    return 100;
}

bool panda_board_battery_charging(void)
{
    /* BAT_ADC samples the selected BAT_OUT rail: USB puts it near 5 V, the
     * battery stays at or below its 4.2 V charge limit. No VBUS GPIO exists. */
    return panda_board_battery_mv() >= 4400;
}

esp_err_t panda_board_rgb_init(void)
{
    if (s_rgb) {
        return ESP_OK;
    }
    /* A previous deep sleep may have held the strip data low. */
    gpio_hold_dis(PANDA_RGB_GPIO);
    gpio_deep_sleep_hold_dis();
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PANDA_RGB_GPIO,
        .max_leds = PANDA_RGB_LEDS,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_rgb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RGB ring init failed: %s", esp_err_to_name(err));
        s_rgb = NULL;
        return err;
    }
    panda_board_rgb_off();
    ESP_LOGI(TAG, "RGB ring ready (GPIO%d x%d)", PANDA_RGB_GPIO, PANDA_RGB_LEDS);
    return ESP_OK;
}

bool panda_board_rgb_ready(void)
{
    return s_rgb != NULL;
}

esp_err_t panda_board_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t frame[PANDA_RGB_LEDS * 3];
    for (int i = 0; i < PANDA_RGB_LEDS; i++) {
        frame[i * 3 + 0] = r;
        frame[i * 3 + 1] = g;
        frame[i * 3 + 2] = b;
    }
    return panda_board_rgb_write(frame, PANDA_RGB_LEDS);
}

esp_err_t panda_board_rgb_write(const uint8_t *rgb, int count)
{
    if (!s_rgb) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb == NULL || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > PANDA_RGB_LEDS) {
        count = PANDA_RGB_LEDS;
    }
    for (int i = 0; i < count; i++) {
        led_strip_set_pixel(s_rgb, i, rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }
    return led_strip_refresh(s_rgb);
}

esp_err_t panda_board_rgb_off(void)
{
    if (!s_rgb) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(s_rgb);
}

esp_err_t panda_board_rgb_sleep_indicator(void)
{
    if (!s_rgb) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t frame[PANDA_RGB_LEDS * 3] = {0};
    /* The dimmest green the 8-bit WS2812 driver can still show evenly. */
    frame[0] = 3;
    frame[1] = 8;
    frame[2] = 4;
    esp_err_t err = panda_board_rgb_write(frame, PANDA_RGB_LEDS);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(400); /* >280us reset: the strip latches the colour */
    /* Detach RMT and hold the data line low, otherwise the pad floats while
     * the digital domain is powered down and the strip can pick up noise. */
    gpio_reset_pin(PANDA_RGB_GPIO);
    gpio_set_direction(PANDA_RGB_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PANDA_RGB_GPIO, 0);
    gpio_hold_en(PANDA_RGB_GPIO);
    gpio_deep_sleep_hold_en();
    return ESP_OK;
}

esp_err_t panda_board_i2c_scan(void)
{
    if (!s_i2c) {
        return ESP_ERR_INVALID_STATE;
    }
    int found = 0;
    ESP_LOGI(TAG, "I2C scan 0x08..0x77:");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_i2c, addr, 30) == ESP_OK) {
            ESP_LOGI(TAG, "  found 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan done, %d device(s)", found);
    return found > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void panda_board_print_info(void)
{
    ESP_LOGI(TAG, "board=%s", PANDA_BOARD_NAME);
    ESP_LOGI(TAG, "i2c=%d ioexp=%d rtc=%d sd=%d adc=%d rgb=%d",
             s_info.i2c_ok, s_info.ioexp_ok, s_info.rtc_ok, s_info.sdcard_ok,
             s_info.battery_adc_ok, s_info.rgb_ok);
    ESP_LOGI(TAG, "free heap=%" PRIu32 " internal=%" PRIu32 " psram=%" PRIu32,
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

const char *panda_board_get_name(void)
{
    return PANDA_BOARD_NAME;
}
