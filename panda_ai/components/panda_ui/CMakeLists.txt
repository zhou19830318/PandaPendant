file(GLOB UI_GENERATED_SRCS
    "generated/ui.c"
    "generated/ui_helpers.c"
    "generated/components/*.c"
    "generated/screens/*.c"
    "generated/images/*.c"
    "generated/fonts/*.c"
)

idf_component_register(
    SRCS "panda_ui.c" "lvgl_mem_pool.c" ${UI_GENERATED_SRCS}
    INCLUDE_DIRS "include" "generated"
    REQUIRES lvgl panda_display panda_state espressif__qrcode
    PRIV_REQUIRES esp_timer esp_app_format
)
