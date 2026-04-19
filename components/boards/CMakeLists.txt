# CONFIG_ vars may be unavailable during early CMake scans; list all deps once.
set(PRIV_REQUIRES
    driver
    nvs_flash
    esp_driver_i2c
    esp_driver_spi
    dac
    board_utils
    esp_codec_dev
    m5stack_core_s3_noglib
    esp_lcd
)

if(CONFIG_BOARD_TARGET_PATH)
    set(boards_dir ${CONFIG_BOARD_TARGET_PATH})
else()
    set(boards_dir "m5stack-core-s3")
endif()

idf_component_register(
    SRC_DIRS "${boards_dir}" "."
    INCLUDE_DIRS "${boards_dir}" "."
    REQUIRES esp_driver_i2c board_utils esp_codec_dev m5stack_core_s3_noglib esp_lcd
    PRIV_REQUIRES ${PRIV_REQUIRES}
)
