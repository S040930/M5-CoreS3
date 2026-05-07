#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*auto_brightness_set_fn_t)(int brightness_percent);

esp_err_t auto_brightness_start(i2c_master_bus_handle_t i2c_bus,
                                 auto_brightness_set_fn_t set_brightness_fn);
void auto_brightness_stop(void);
void auto_brightness_notify_manual_override(void);

#ifdef __cplusplus
}
#endif
