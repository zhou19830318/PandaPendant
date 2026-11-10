idf_component_register(
    SRCS "panda_display.c" "src/esp_lcd_st77916.c" "src/st77916_panel_init.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_lvgl_port lvgl panda_board
    PRIV_REQUIRES esp_driver_spi esp_driver_ledc esp_driver_gpio esp_timer
)
