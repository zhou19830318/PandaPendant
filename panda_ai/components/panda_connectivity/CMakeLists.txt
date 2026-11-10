idf_component_register(
    SRCS "panda_connectivity.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer panda_board
)
