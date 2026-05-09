idf_component_register(
    SRCS
        "app_core.c"
        "app_events.c"
        "settings.c"
        "perf_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES
        airplay_core
        audio_core
        auto_brightness
        env_monitor
        event_bus
        network_core
        screen_ui
        realtime_voice
        nvs_flash
)
