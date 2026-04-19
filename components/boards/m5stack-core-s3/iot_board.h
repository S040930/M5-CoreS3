#pragma once

#include "board_common.h"
#include "sdkconfig.h"

#define BOARD_NAME        "M5Stack CoreS3"
#define BOARD_DESCRIPTION "ESP32-S3 with built-in speaker via BSP audio path"

// LED and battery helpers used elsewhere in the project
#define BOARD_LED_STATUS_GPIO CONFIG_LED_STATUS_GPIO
#define BOARD_LED_ERROR_GPIO  CONFIG_LED_ERROR_GPIO
#define BOARD_LED_RGB_GPIO    CONFIG_LED_RGB_GPIO
#define BOARD_BAT_CHANNEL     CONFIG_BAT_CHANNEL

