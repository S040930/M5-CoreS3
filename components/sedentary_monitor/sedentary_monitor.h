#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
  SEDENTARY_DETECT_UNKNOWN = 0,
  SEDENTARY_DETECT_PRESENT,
  SEDENTARY_DETECT_ABSENT,
} sedentary_detect_class_t;

typedef struct {
  sedentary_detect_class_t clazz;
  bool valid;
} sedentary_detect_result_t;

void sedentary_monitor_set_enabled(bool enabled);
bool sedentary_monitor_is_enabled(void);

esp_err_t sedentary_monitor_start(void);
void sedentary_monitor_stop(void);
