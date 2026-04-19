#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  LED_OFF,
  LED_STEADY,
  LED_BLINK_SLOW,   // 100ms on, 2500ms off (standby)
  LED_BLINK_MEDIUM, // 500ms on/off (paused)
  LED_BLINK_FAST,   // 250ms on/off
  LED_VU,           // Audio visualization
} led_mode_t;

/**
 * Initialize LED subsystem and register for RTSP events.
 */
void led_init(void);

/**
 * Feed audio samples for VU meter mode.
 * Call this from the audio path when playing.
 */
void led_audio_feed(const int16_t *pcm, size_t stereo_samples);

/**
 * Set error state (e.g., speaker fault, decode failure).
 * Clears automatically on next playback state change.
 */
void led_set_error(bool error);

/**
 * Read current LED state snapshot for UI/debug.
 */
void led_get_snapshot(led_mode_t *status_mode, led_mode_t *rgb_mode,
                      bool *error_active, uint8_t *status_brightness);

/**
 * Manually set the status LED mode.
 * This is intended for explicit UI control and may be overwritten by RTSP
 * state changes.
 */
esp_err_t led_set_status_mode(led_mode_t mode);

/**
 * Set status LED brightness (0..255).
 */
esp_err_t led_set_status_brightness(uint8_t brightness);
