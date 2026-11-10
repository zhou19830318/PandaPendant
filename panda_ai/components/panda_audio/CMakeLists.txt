idf_component_register(
    SRCS "panda_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES panda_board esp_codec_dev esp_driver_i2s esp_driver_i2c
)
