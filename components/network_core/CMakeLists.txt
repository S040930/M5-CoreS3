idf_component_register(
    SRCS
        "network/wifi.c"
        "network/wifi_credentials.c"
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
        nvs_flash
)

# ── Removed: target_include_directories for app_core (layering: Layer 2 → Layer 4 violation)
