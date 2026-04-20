idf_component_register(
    SRCS
        "app_core.c"
        "settings.c"
    INCLUDE_DIRS "."
    REQUIRES
        airplay_core
        audio_core
        network_core
        nvs_flash
)
