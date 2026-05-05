#pragma once

#include "esp_err.h"

/** Short tone + on-screen line; acquires speaker like voice path. */
esp_err_t sedentary_alert_play(const char *utf8_line);
