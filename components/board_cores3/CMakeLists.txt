idf_component_register(
    SRCS
        "board.c"
        "board_common.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
        esp_driver_i2s
        esp_codec_dev
        m5stack_core_s3_noglib
)
