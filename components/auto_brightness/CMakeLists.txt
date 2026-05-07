idf_component_register(
    SRCS "ltr553.c" "auto_brightness.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_timer
)
