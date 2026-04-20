#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  uint32_t frames_processed;
  uint32_t gate_active_frames;
  uint32_t compressor_active_frames;
  uint32_t limiter_events;
  int16_t peak_dbfs_x100;
  int16_t rms_dbfs_x100;
  int16_t noise_floor_dbfs_x100;
  int16_t low_band_dbfs_x100;
  int16_t mid_band_dbfs_x100;
  int16_t high_band_dbfs_x100;
  uint8_t gate_gain_pct;
  uint8_t compressor_gain_pct;
} audio_dsp_stats_t;

typedef enum {
  AUDIO_DSP_MODE_LIMITER_ONLY = 0,
  AUDIO_DSP_MODE_ENHANCED = 1,
} audio_dsp_mode_t;

esp_err_t audio_dsp_init(uint32_t sample_rate_hz);
void audio_dsp_set_mode(audio_dsp_mode_t mode);
audio_dsp_mode_t audio_dsp_get_mode(void);
void audio_dsp_process(int16_t *samples, size_t sample_count);
void audio_dsp_get_stats(audio_dsp_stats_t *stats);
