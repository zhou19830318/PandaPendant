idf_component_register(
    SRCS "app_main.c" "app_core.c" "app_settings.c" "app_provisioning.c" "app_voice.c"
         "app_weather.c" "serial_cli.c" "voice_resampler.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/whoareyou.wav"
    REQUIRES nvs_flash console esp_http_server esp_http_client esp_websocket_client esp-tls mbedtls json panda_board panda_audio panda_display panda_touch panda_ui panda_connectivity
    PRIV_REQUIRES esp_timer esp_driver_uart
)
