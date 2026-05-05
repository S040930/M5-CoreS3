#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t audio_volume_load(float *volume_db);
esp_err_t audio_volume_get(float *volume_db);
esp_err_t audio_volume_save(float volume_db);
esp_err_t audio_volume_persist(void);
int32_t audio_volume_db_to_q15(float volume_db);
