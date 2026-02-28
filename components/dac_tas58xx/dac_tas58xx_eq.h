#pragma once

/**
 * 15-band parametric EQ using TAS5825M on-chip biquad filters.
 *
 * Each band is implemented as a peaking EQ biquad at a fixed center
 * frequency with ~2/3-octave bandwidth (Q ≈ 2.145).  Both channels
 * receive identical coefficients.
 *
 * Band   Center (Hz)
 *  0        25
 *  1        40
 *  2        63
 *  3       100
 *  4       160
 *  5       250
 *  6       400
 *  7       630
 *  8      1000
 *  9      1600
 * 10      2500
 * 11      4000
 * 12      6300
 * 13     10000
 * 14     16000
 *
 * Requires dac_init() to have been called first.
 */

#include "esp_err.h"
#include <stdbool.h>

/** Number of EQ bands (one per on-chip biquad) */
#define TAS58XX_EQ_BANDS 15

/** Gain limits per band (dB) */
#define TAS58XX_EQ_MAX_GAIN_DB  12.0f
#define TAS58XX_EQ_MIN_GAIN_DB -12.0f

/**
 * Set gain for a single EQ band.
 * Coefficients are written immediately; both L and R channels are set.
 *
 * @param band     Band index 0–14
 * @param gain_db  Gain in dB (clamped to ±12 dB)
 * @return ESP_OK on success
 */
esp_err_t tas58xx_eq_set_band(int band, float gain_db);

/**
 * Set all 15 EQ bands in one call.
 *
 * @param gains_db  Array of TAS58XX_EQ_BANDS gain values in dB
 * @return ESP_OK on success, or the first error encountered
 */
esp_err_t tas58xx_eq_set_all(const float gains_db[TAS58XX_EQ_BANDS]);

/**
 * Reset all bands to flat (0 dB gain — passthrough).
 */
esp_err_t tas58xx_eq_flat(void);

/**
 * Return the center frequency (Hz) of the given band.
 *
 * @param band  Band index 0–14
 * @return Center frequency in Hz, or 0.0f if index is invalid
 */
float tas58xx_eq_get_center_freq(int band);

/**
 * Read back biquad coefficients at the expected RAM addresses and log
 * whether they match the default (flat) pattern.  Useful for verifying
 * that the coefficient address table is correct for this device.
 *
 * @return ESP_OK if all addresses look valid
 */
esp_err_t tas58xx_eq_verify_addresses(void);
