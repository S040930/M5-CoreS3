#pragma once

#include "esp_err.h"
#include "sedentary_monitor.h"

/** Call once after camera init; may erase NVS when recalibrate-on-boot is enabled. */
void sedentary_local_detect_on_boot(void);

bool sedentary_calibration_is_ready(void);
esp_err_t sedentary_calibration_begin_empty(void);
esp_err_t sedentary_calibration_begin_present(void);

/**
 * Compare latest small frame to stored baseline inside ROI; applies hysteresis and streak
 * confirmation. Returns ESP_ERR_INVALID_STATE when calibration is required but missing.
 */
esp_err_t sedentary_local_detect_run(sedentary_detect_result_t *out);
