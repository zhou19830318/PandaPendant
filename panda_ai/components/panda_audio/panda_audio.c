#include "panda_audio.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"

#include "panda_board.h"
#include "panda_board_pins.h"

static const char *TAG = "audio";

#define MIC_GAIN_DB 37.5f
#define DEFAULT_VOLUME 70

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_out;
static esp_codec_dev_handle_t s_in;
/* Separate locks: esp_codec_dev only serializes open/reconfig, so capture and
 * playback may run concurrently. A shared lock let a 100 ms mic read stall
 * TTS playback and produce audible gaps. */
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_rx_lock;
static int s_volume = DEFAULT_VOLUME;
static bool s_ready;
static bool s_has_mic;

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = PANDA_I2S_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 12,
        .dma_frame_num = 240,
        .auto_clear = true,
        .intr_priority = 3,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG,
                        "i2s channel create failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PANDA_AUDIO_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = PANDA_I2S_MCLK,
            .bclk = PANDA_I2S_SCLK,
            .ws = PANDA_I2S_LRCK,
            .dout = PANDA_I2S_DOUT,
            .din = PANDA_I2S_DIN,
            .invert_flags = {false, false, false},
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG,
                        "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG,
                        "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx enable failed");
    return ESP_OK;
}

static esp_err_t codec_out_init(const audio_codec_data_if_t *data_if)
{
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = PANDA_I2C_PORT,
        .addr = PANDA_ES8311_ADDR << 1,
        .bus_handle = panda_board_i2c(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "ES8311 i2c control interface failed");
        return ESP_FAIL;
    }

    esp_codec_dev_hw_gain_t hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3};
    es8311_codec_cfg_t cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
    };
    const audio_codec_if_t *codec = es8311_codec_new(&cfg);
    if (codec == NULL) {
        ESP_LOGE(TAG, "ES8311 codec create failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data_if,
    };
    s_out = esp_codec_dev_new(&dev_cfg);
    if (s_out == NULL) {
        ESP_LOGE(TAG, "ES8311 device create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t codec_in_init(const audio_codec_data_if_t *data_if)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = PANDA_I2C_PORT,
        .addr = PANDA_ES7210_ADDR << 1,
        .bus_handle = panda_board_i2c(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "ES7210 i2c control interface failed");
        return ESP_FAIL;
    }

    es7210_codec_cfg_t cfg = {
        .ctrl_if = ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *codec = es7210_codec_new(&cfg);
    if (codec == NULL) {
        ESP_LOGE(TAG, "ES7210 codec create failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec,
        .data_if = data_if,
    };
    s_in = esp_codec_dev_new(&dev_cfg);
    if (s_in == NULL) {
        ESP_LOGE(TAG, "ES7210 device create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t codecs_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = PANDA_AUDIO_BITS,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = PANDA_AUDIO_RATE,
    };

    if (esp_codec_dev_open(s_out, &fs) != 0) {
        ESP_LOGE(TAG, "speaker open failed");
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_out, s_volume);
    panda_board_pa_enable(true);

    if (s_in != NULL) {
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
        if (esp_codec_dev_open(s_in, &fs) != 0) {
            ESP_LOGW(TAG, "microphone open failed");
            s_in = NULL;
        } else {
            esp_codec_dev_set_in_gain(s_in, MIC_GAIN_DB);
            s_has_mic = true;
        }
    }
    return ESP_OK;
}

esp_err_t panda_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL || s_rx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2s_init();
    if (err != ESP_OK) {
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = PANDA_I2S_NUM,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "i2s data interface failed");
        return ESP_FAIL;
    }

    err = codec_out_init(data_if);
    if (err != ESP_OK) {
        return err;
    }
    if (codec_in_init(data_if) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without microphone");
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    err = codecs_open();
    xSemaphoreGive(s_rx_lock);
    xSemaphoreGive(s_tx_lock);
    if (err != ESP_OK) {
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "audio ready (16kHz mono, mic=%s, volume=%d%%)",
             s_has_mic ? "yes" : "no", s_volume);
    return ESP_OK;
}

bool panda_audio_ready(void)
{
    return s_ready;
}

bool panda_audio_has_mic(void)
{
    return s_has_mic;
}

esp_err_t panda_audio_record(int16_t *buffer, size_t samples, size_t *samples_read)
{
    if (samples_read) {
        *samples_read = 0;
    }
    if (!s_ready || s_in == NULL || buffer == NULL || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_rx_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int ret = esp_codec_dev_read(s_in, buffer, samples * sizeof(int16_t));
    xSemaphoreGive(s_rx_lock);
    if (ret != 0) {
        return ESP_FAIL;
    }
    if (samples_read) {
        *samples_read = samples;
    }
    return ESP_OK;
}

esp_err_t panda_audio_play(const int16_t *buffer, size_t samples, size_t *samples_written)
{
    if (samples_written) {
        *samples_written = 0;
    }
    if (!s_ready || s_out == NULL || buffer == NULL || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int ret = esp_codec_dev_write(s_out, (void *)buffer, samples * sizeof(int16_t));
    xSemaphoreGive(s_tx_lock);
    if (ret != 0) {
        return ESP_FAIL;
    }
    if (samples_written) {
        *samples_written = samples;
    }
    return ESP_OK;
}

esp_err_t panda_audio_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (!s_ready || s_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int ret = esp_codec_dev_set_out_vol(s_out, percent);
    xSemaphoreGive(s_tx_lock);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t panda_audio_get_volume(int *percent)
{
    if (percent == NULL || !s_ready || s_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int ret = esp_codec_dev_get_out_vol(s_out, percent);
    xSemaphoreGive(s_tx_lock);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t panda_audio_mute(bool mute)
{
    if (!s_ready || s_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int ret = esp_codec_dev_set_out_mute(s_out, mute);
    xSemaphoreGive(s_tx_lock);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t panda_audio_self_test(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t tone_samples = PANDA_AUDIO_RATE / 5;
    int16_t *buf = heap_caps_malloc(tone_samples * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < tone_samples; i++) {
        float t = (float)i / (float)PANDA_AUDIO_RATE;
        float env = 1.0f;
        size_t fade = PANDA_AUDIO_RATE / 50;
        if (i < fade) {
            env = (float)i / (float)fade;
        } else if (i > tone_samples - fade) {
            env = (float)(tone_samples - i) / (float)fade;
        }
        buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * 880.0f * t) * env * 12000.0f);
    }
    size_t written = 0;
    esp_err_t err = panda_audio_play(buf, tone_samples, &written);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tone play failed: %s", esp_err_to_name(err));
        heap_caps_free(buf);
        return err;
    }

    if (s_has_mic) {
        size_t read = 0;
        err = panda_audio_record(buf, tone_samples, &read);
        if (err == ESP_OK && read > 0) {
            int64_t sum = 0;
            int16_t peak = 0;
            for (size_t i = 0; i < read; i++) {
                sum += (int32_t)buf[i] * buf[i];
                int16_t a = buf[i] < 0 ? -buf[i] : buf[i];
                if (a > peak) {
                    peak = a;
                }
            }
            int rms = (int)sqrtf((float)(sum / (int64_t)read));
            ESP_LOGI(TAG, "mic selftest: rms=%d peak=%d samples=%u",
                     rms, peak, (unsigned)read);
        } else {
            ESP_LOGW(TAG, "mic selftest read failed: %s", esp_err_to_name(err));
        }
    }

    heap_caps_free(buf);
    return ESP_OK;
}
