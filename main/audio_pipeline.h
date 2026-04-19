#pragma once

#include "audio/audio_dsp.h"
#include "audio/audio_receiver.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t target_latency_ms;
} audio_pipeline_session_cfg_t;

typedef struct {
  audio_stats_t stats;
  char codec[32];
  uint32_t output_latency_us;
  uint32_t hardware_latency_us;
  uint32_t target_latency_ms;
  uint32_t buffer_depth_frames;
  uint16_t stream_port;
  uint16_t buffered_port;
  uint8_t avg_task_load_pct;
  uint8_t peak_task_load_pct;
  uint32_t gap_concealment_blocks;
  uint32_t underrun_bursts;
  audio_dsp_stats_t dsp;
} audio_pipeline_snapshot_t;

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(const audio_pipeline_session_cfg_t *cfg);
void audio_pipeline_stop(void);
void audio_pipeline_flush(void);
void audio_pipeline_note_playback_busy(uint32_t busy_us);
void audio_pipeline_note_gap_concealment(bool burst_start);
void audio_pipeline_get_snapshot(audio_pipeline_snapshot_t *snapshot);
