#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef struct {
  bool speaker_open;
  uint32_t output_rate;
  uint8_t bits_per_sample;
  uint8_t channels;
  uint16_t channel_mask;
  int mclk_multiple;
  bool muted;
  int volume;
  float current_volume_db;
  float target_volume_db;
  bool ramping;
  int reg04;
  int reg05;
  int reg06;
  int reg0c;
  int reg12;
  int reg14;
} audio_output_diag_t;

/**
 * Initialize I2S audio output
 */
esp_err_t audio_output_init(void);

/**
 * Start the audio playback task
 */
void audio_output_start(void);

/**
 * Flush I2S DMA buffers (clears stale audio on pause/seek)
 */
void audio_output_flush(void);

/**
 * Stop the AirPlay playback task (for yielding I2S to another source)
 */
void audio_output_stop(void);

/**
 * Write raw PCM data to the I2S output.
 * Can be used by any audio source (BT A2DP, etc.) when the AirPlay
 * playback task is stopped.
 *
 * @param data   PCM data buffer (interleaved stereo, 16-bit)
 * @param bytes  Number of bytes to write
 * @param wait   Maximum ticks to wait for I2S DMA space
 * @return ESP_OK on success
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/**
 * Change the I2S sample rate (e.g. when BT negotiates 48 kHz)
 *
 * @param rate  Sample rate in Hz (e.g. 44100, 48000)
 */
void audio_output_set_sample_rate(uint32_t rate);

/**
 * Notify the output of the source sample rate (from AirPlay ANNOUNCE).
 * The resampler is re-initialized if the rate changes.
 */
void audio_output_set_source_rate(int rate);

/**
 * Check whether the AirPlay output worker is active.
 * Used by RTSP handlers to avoid flush requests when output is stopped.
 */
bool audio_output_is_active(void);

/**
 * Get CoreS3 speaker diagnostic state and key AW88298 registers.
 */
esp_err_t audio_output_get_diag(audio_output_diag_t *diag);

/**
 * Play a short local test tone through the speaker path.
 */
esp_err_t audio_output_play_test_tone(uint32_t frequency_hz, uint32_t duration_ms,
                                      uint8_t amplitude_pct);

/**
 * Update desired output volume in AirPlay dB scale (-30..0).
 * CoreS3 applies the value through a smooth ramp.
 */
void audio_output_set_target_volume_db(float volume_db);

/**
 * Mute/unmute hardware output without altering target volume.
 */
void audio_output_set_muted(bool muted);
