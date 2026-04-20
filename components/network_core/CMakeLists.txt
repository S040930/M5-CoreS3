idf_component_register(
    SRCS
        "network/wifi.c"
        "network/mdns_airplay.c"
        "network/ntp_clock.c"
        "network/socket_utils.c"
    INCLUDE_DIRS
        "."
        "network"
    REQUIRES
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        mdns
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${PROJECT_DIR}/components/app_core"
)
