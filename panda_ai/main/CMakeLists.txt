idf_component_register(
    SRCS "app_main.c" "app_core.c" "app_settings.c" "app_provisioning.c" "app_voice.c"
         "app_weather.c" "serial_cli.c" "voice_resampler.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/whoareyou.wav"
    REQUIRES nvs_flash console esp_http_server esp_http_client esp_websocket_client esp-tls mbedtls json panda_board panda_audio panda_display panda_touch panda_ui panda_connectivity panda_mp3 panda_led
    PRIV_REQUIRES esp_timer esp_driver_uart
)

# The 96 KB LVGL widget pool belongs in PSRAM: keeping it in .bss leaves only
# ~10 KB of internal RAM, which starves TLS, FATFS and the SD upload buffer.
# LV_MEM_POOL_ALLOC makes lv_mem_init() take the pool from the PSRAM heap.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PRIVATE
    "LV_MEM_POOL_INCLUDE=\"${CMAKE_CURRENT_LIST_DIR}/../components/panda_ui/include/lvgl_mem_pool.h\""
    "LV_MEM_POOL_ALLOC=panda_lvgl_pool_alloc"
)
# panda_ui is linked before lvgl, so keep the allocator object alive explicitly.
target_link_options(${lvgl_lib} INTERFACE "-u" "panda_lvgl_pool_alloc")
