idf_component_register(
    SRCS
        "app_core.c"
        "settings.c"
    INCLUDE_DIRS "."
    REQUIRES
        airplay_core
        audio_core
        auto_brightness
        env_monitor
        network_core
        screen_ui
        realtime_voice
        sedentary_monitor
        nvs_flash
)
