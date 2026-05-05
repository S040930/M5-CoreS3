#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "audio_fidelity.h"

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
 * Tap PCM immediately before it is written to the speaker codec (after volume
 * ramp step). Used by realtime_voice to feed AirPlay output into the AEC
 * reference ring while music is playing.
 *
 * Called from the audio output worker context; keep the callback short.
 *
 * @param interleaved_stereo 16-bit stereo samples (L,R,L,R,...)
 * @param frames Number of stereo frames (samples / 2 channels)
 * @param sample_rate_hz Current speaker DSP sample rate (e.g. 44100)
 * @param ctx User pointer from audio_output_set_ref_tap
 */
typedef void (*audio_output_ref_tap_fn)(const int16_t *interleaved_stereo,
                                        size_t frames, uint32_t sample_rate_hz,
                                        void *ctx);

/**
 * Install or remove the pre-codec reference tap (pass NULL fn to disable).
 */
void audio_output_set_ref_tap(audio_output_ref_tap_fn fn, void *ctx);

/**
 * Write raw PCM data to the I2S output.
 * Used by the retained AirPlay playback path when the output worker
 * is stopped or bypassed.
 *
 * @param data   PCM data buffer (interleaved stereo, 16-bit)
 * @param bytes  Number of bytes to write
 * @param wait   Maximum ticks to wait for I2S DMA space
 * @return ESP_OK on success
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/**
 * Change the I2S sample rate for the current AirPlay session.
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

/**
 * Select the active audio fidelity path.
 * PURE keeps a limiter-only DSP stage and event-driven EQ updates.
 * ENHANCED keeps the full legacy DSP behavior.
 */
void audio_output_set_fidelity_mode(audio_fidelity_mode_t mode);

/**
 * Read current audio fidelity mode.
 */
audio_fidelity_mode_t audio_output_get_fidelity_mode(void);

/**
 * Temporarily transfer speaker ownership to an external module (e.g. realtime voice).
 * Stops the AirPlay playback worker while held.
 */
esp_err_t audio_output_acquire_external(const char *owner_tag, bool stop_worker);

/**
 * Release speaker ownership previously acquired by external module.
 * Optionally resumes AirPlay playback worker if it was running before acquire.
 */
esp_err_t audio_output_release_external(const char *owner_tag, bool resume_worker);

/**
 * Get current external speaker owner status for diagnostics.
 */
esp_err_t audio_output_get_external_owner(char *owner_tag, size_t owner_tag_len,
                                          bool *owned);

/**
 * Get CoreS3 microphone codec handle (for external voice capture).
 * Returns ESP_ERR_INVALID_STATE if board is not initialized.
 */
esp_err_t audio_output_get_mic_handle(void **out_handle);

/**
 * Get CoreS3 speaker codec handle (for external voice playback).
 * Returns ESP_ERR_INVALID_STATE if board is not initialized.
 */
esp_err_t audio_output_get_spk_handle(void **out_handle);

/** I2S bus state tracking for shared RX/TX on CoreS3. */
typedef enum {
    I2S_BUS_IDLE = 0,
    I2S_BUS_RX_ONLY,
    I2S_BUS_TX_ONLY,
    I2S_BUS_FULL_DUPLEX,
} i2s_bus_state_t;

/**
 * Notify that the I2S RX channel (mic) has been opened or closed.
 * Tracks shared-bus state to catch conflicts early.
 */
void audio_output_notify_i2s_rx(bool open);

/**
 * Notify that the I2S TX channel (speaker) has been opened or closed.
 */
void audio_output_notify_i2s_tx(bool open);

/**
 * Return current I2S bus state for diagnostics.
 */
i2s_bus_state_t audio_output_get_i2s_state(void);
