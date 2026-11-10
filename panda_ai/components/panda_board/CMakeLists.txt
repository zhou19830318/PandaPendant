idf_component_register(
    SRCS "panda_board.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_driver_sdmmc sdmmc fatfs esp_io_expander
             esp_driver_spi esp_driver_ledc esp_adc
    PRIV_REQUIRES esp_io_expander_tca95xx_16bit led_strip esp_timer
)
