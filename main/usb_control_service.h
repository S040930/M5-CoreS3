#pragma once

#include "cJSON.h"
#include "esp_err.h"

esp_err_t usb_control_service_init(void);
esp_err_t usb_control_service_start(void);
void usb_control_service_stop(void);
esp_err_t usb_control_dispatch(const char *cmd, const cJSON *args,
                               cJSON *result);
