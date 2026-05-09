#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_DEFAULT_DEVICE_NAME "ESP32 AirPlay"

esp_err_t settings_init(void);
esp_err_t settings_get_device_name(char *name, size_t len);
esp_err_t settings_set_device_name(const char *name);

/* Voice configuration (NVS-backed, runtime overridable) */
esp_err_t settings_get_voice_api_key(char *key, size_t len);
esp_err_t settings_set_voice_api_key(const char *key);
esp_err_t settings_get_voice_url(char *url, size_t len);
esp_err_t settings_set_voice_url(const char *url);
esp_err_t settings_get_voice_model(char *model, size_t len);
esp_err_t settings_set_voice_model(const char *model);
esp_err_t settings_get_voice_instructions(char *instructions, size_t len);
esp_err_t settings_set_voice_instructions(const char *instructions);
bool settings_has_voice_config(void);
