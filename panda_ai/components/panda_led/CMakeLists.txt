idf_component_register(
    SRCS "panda_led.c"
    INCLUDE_DIRS "include"
    REQUIRES panda_board panda_audio
    PRIV_REQUIRES esp_timer
)
