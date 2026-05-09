#pragma once
#include "esp_err.h"

esp_err_t perf_monitor_init(void);
void perf_monitor_start(void);
void perf_monitor_stop(void);
