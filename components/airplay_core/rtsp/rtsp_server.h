#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Start the AirPlay RTSP server on port 7000
 * Handles initial connection requests from iOS devices
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server
 */
void rtsp_server_stop(void);

/**
 * Get current volume as Q15 scale factor for audio processing
 * @return Q15 fixed-point multiplier (0 = mute, 32768 = unity)
 */
int32_t airplay_get_volume_q15(void);
