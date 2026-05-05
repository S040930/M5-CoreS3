#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t rtsp_server_start(const char *device_name);
void rtsp_server_stop(void);
bool rtsp_server_is_running(void);
int32_t airplay_get_volume_q15(void);
