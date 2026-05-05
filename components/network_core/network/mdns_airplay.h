#pragma once

#include "esp_err.h"

/**
 * Initialize mDNS and advertise AirPlay 1 services.
 * @param device_name Human-readable device name for TXT records (e.g. "ESP32 AirPlay")
 */
esp_err_t mdns_airplay_init(const char *device_name);

/**
 * Stop AirPlay mDNS advertisement and release mDNS runtime.
 */
void mdns_airplay_deinit(void);
