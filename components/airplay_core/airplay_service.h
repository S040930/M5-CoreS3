#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef void (*airplay_service_active_cb_t)(bool active);

esp_err_t airplay_service_start(const char *device_name);
void airplay_service_stop(void);
esp_err_t airplay_service_recover(void);
void airplay_service_refresh_playback(void);
bool airplay_service_is_active(void);

void airplay_service_set_active_callback(airplay_service_active_cb_t cb);
