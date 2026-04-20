#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize software EQ state for PCM output.
 */
esp_err_t audio_eq_init(uint32_t sample_rate_hz);

/**
 * Reload EQ gains from settings if present, otherwise keep defaults.
 */
void audio_eq_reload_from_settings(void);

/**
 * Process interleaved stereo PCM samples in-place.
 */
void audio_eq_process(int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif
