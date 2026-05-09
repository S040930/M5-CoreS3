#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*airplay_active_callback_t)(bool active);

esp_err_t airplay_domain_init(void);

esp_err_t airplay_domain_start(const char *device_name);

esp_err_t airplay_domain_stop(void);

esp_err_t airplay_domain_refresh_playback(void);

bool airplay_domain_is_active(void);

esp_err_t airplay_domain_set_active_callback(airplay_active_callback_t callback);
