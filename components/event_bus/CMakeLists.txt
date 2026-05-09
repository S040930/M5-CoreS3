idf_component_register(
    SRCS
        "event_bus.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_event
        esp_timer
)
