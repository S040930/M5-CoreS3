#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "env_monitor.h"

esp_err_t env_domain_init(void);

esp_err_t env_domain_start(void);

esp_err_t env_domain_stop(void);

esp_err_t env_domain_get_reading(float *temperature, float *humidity, float *pressure);

bool env_domain_is_available(void);
