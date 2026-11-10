idf_component_register(
    SRCS "panda_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd_touch esp_lcd_touch_cst816s lvgl panda_board panda_display
    PRIV_REQUIRES esp_driver_i2c esp_timer
)
