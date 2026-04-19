#pragma once

#include "esp_err.h"

esp_err_t airplay_service_start(void);
void airplay_service_stop(void);
esp_err_t airplay_service_recover(void);
