#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LTR553_I2C_ADDR 0x23

esp_err_t ltr553_init(i2c_master_bus_handle_t bus);
esp_err_t ltr553_deinit(void);
esp_err_t ltr553_read_als(uint16_t *ch0, uint16_t *ch1);
esp_err_t ltr553_als_enable(bool enable);

#ifdef __cplusplus
}
#endif
