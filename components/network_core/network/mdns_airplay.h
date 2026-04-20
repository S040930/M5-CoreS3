#pragma once

#include "esp_err.h"

/**
 * Initialize mDNS and advertise AirPlay 1 services.
 *
 * The narrowed CoreS3 build only publishes classic RAOP discovery.
 */
esp_err_t mdns_airplay_init(void);
