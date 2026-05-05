#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t wifi_creds_get_ssid(char *ssid, size_t len);
esp_err_t wifi_creds_get_password(char *password, size_t len);
esp_err_t wifi_creds_save(const char *ssid, const char *password);
esp_err_t wifi_creds_clear(void);
bool wifi_creds_have(void);
