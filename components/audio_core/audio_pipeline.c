#include "audio_pipeline.h"

#include "audio/audio_output.h"
#include "esp_timer.h"
#include <string.h>

static bool s_initialized = false;
static bool s_running = false;
static uint64_t s_window_start_us = 0;
static uint64_t s_busy_accum_us = 0;
static uint8_t s_avg_load_pct = 0;
static uint8_t s_peak_load_pct = 0;
static uint32_t s_gap_concealment_blocks = 0;
static uint32_t s_underrun_bursts = 0;

static void refresh_load_metrics(void) {
  uint64_t now_us = (uint64_t)esp_timer_get_time();
  if (s_window_start_us == 0) {
    s_window_start_us = now_us;
    return;
  }

  uint64_t elapsed_us = now_us - s_window_start_us;
  if (elapsed_us < 1000000ULL) {
    return;
  }

  uint32_t pct = elapsed_us > 0
                     ? (uint32_t)((s_busy_accum_us * 100ULL) / elapsed_us)
                     : 0;
  if (pct > 100U) {
    pct = 100U;
  }
  if (pct > s_peak_load_pct) {
    s_peak_load_pct = (uint8_t)pct;
  }
  s_avg_load_pct = (uint8_t)((s_avg_load_pct * 3U + pct) / 4U);
  s_busy_accum_us = 0;
  s_window_start_us = now_us;
}

esp_err_t audio_pipeline_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }

  esp_err_t err = audio_receiver_init();
  if (err != ESP_OK) {
    return err;
  }

  err = audio_output_init();
  if (err != ESP_OK) {
    return err;
  }

  s_initialized = true;
  s_window_start_us = (uint64_t)esp_timer_get_time();
  return ESP_OK;
}

esp_err_t audio_pipeline_start(const audio_pipeline_session_cfg_t *cfg) {
  if (!s_initialized) {
    esp_err_t err = audio_pipeline_init();
    if (err != ESP_OK) {
      return err;
    }
  }

  if (cfg) {
    audio_receiver_set_output_latency_us(cfg->target_latency_ms * 1000U);
  }
  audio_output_start();
  s_running = true;
  return ESP_OK;
}

void audio_pipeline_stop(void) {
  if (!s_initialized) {
    return;
  }

  audio_output_stop();
  audio_receiver_stop();
  audio_receiver_flush();
  s_running = false;
}

void audio_pipeline_flush(void) {
  if (!s_initialized) {
    return;
  }
  audio_output_flush();
  audio_receiver_flush();
}

void audio_pipeline_note_playback_busy(uint32_t busy_us) {
  s_busy_accum_us += busy_us;
}

void audio_pipeline_note_gap_concealment(bool burst_start) {
  s_gap_concealment_blocks++;
  if (burst_start) {
    s_underrun_bursts++;
  }
}

void audio_pipeline_get_snapshot(audio_pipeline_snapshot_t *snapshot) {
  if (!snapshot) {
    return;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  refresh_load_metrics();
  audio_receiver_get_stats(&snapshot->stats);
  audio_receiver_get_active_codec(snapshot->codec, sizeof(snapshot->codec));
  snapshot->output_latency_us = audio_receiver_get_output_latency_us();
  snapshot->hardware_latency_us = audio_receiver_get_hardware_latency_us();
  snapshot->target_latency_ms = CONFIG_AUDIO_TARGET_LATENCY_MS;
  snapshot->buffer_depth_frames = audio_receiver_get_buffered_frames();
  snapshot->stream_port = audio_receiver_get_stream_port();
  snapshot->buffered_port = audio_receiver_get_buffered_port();
  snapshot->avg_task_load_pct = s_avg_load_pct;
  snapshot->peak_task_load_pct = s_peak_load_pct;
  snapshot->gap_concealment_blocks = s_gap_concealment_blocks;
  snapshot->underrun_bursts = s_underrun_bursts;
  audio_dsp_get_stats(&snapshot->dsp);
  if (!s_running) {
    snapshot->avg_task_load_pct = 0;
  }
}
