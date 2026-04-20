#include "audio_output_common.h"

#include "audio_pipeline.h"
#include "audio_dsp.h"
#include "audio_eq.h"
#include "audio_receiver.h"
#include "audio_resample.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rtsp_server.h"
#include <stdlib.h>

#define TAG "audio_output"

// Reserve headroom to reduce clipping at loud passages.
#define OUTPUT_HEADROOM_Q15 21900
#define CLIP_RISK_PEAK      30000
#define EQ_RELOAD_PEAK_GUARD 22000
#define EQ_RELOAD_MAX_DEFER  12
#define GAP_CONCEAL_REPEAT_MAX 6
#define GAP_CONCEAL_FADE_Q15   26214

static const audio_output_hw_ops_t *s_ops = NULL;
static volatile bool flush_requested = false;
static volatile bool playback_running = false;
static TaskHandle_t playback_task_handle = NULL;
static volatile int source_rate = 44100;
static volatile bool resample_reinit_needed = false;
static volatile bool s_eq_reload_pending = false;
static uint8_t s_eq_reload_defer_count = 0;
static uint32_t s_gain_diag_counter = 0;
static volatile audio_fidelity_mode_t s_fidelity_mode = AUDIO_FIDELITY_MODE_PURE;

static int16_t peak_abs_sample(const int16_t *buf, size_t n) {
  int16_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = (int32_t)buf[i];
    int32_t mag = v < 0 ? -v : v;
    if (mag > 32767) {
      mag = 32767;
    }
    int16_t abs_sample = (int16_t)mag;
    if (abs_sample > peak) {
      peak = abs_sample;
    }
  }
  return peak;
}

static void apply_volume(int16_t *buf, size_t n) {
  int32_t gain_q15 = OUTPUT_HEADROOM_Q15;
  if (s_ops && s_ops->software_volume) {
    int32_t vol_q15 = airplay_get_volume_q15();
    gain_q15 = (int32_t)(((int64_t)gain_q15 * vol_q15) >> 15);
  }
  for (size_t i = 0; i < n; i++) {
    buf[i] = (int16_t)(((int32_t)buf[i] * gain_q15) >> 15);
  }
}

static void maybe_log_gain_path(void) {
  if (++s_gain_diag_counter % 1000 != 0) {
    return;
  }

  int32_t headroom_q15 = OUTPUT_HEADROOM_Q15;
  int32_t vol_q15 = 32768;
  int32_t effective_q15 = OUTPUT_HEADROOM_Q15;
  if (s_ops && s_ops->software_volume) {
    vol_q15 = airplay_get_volume_q15();
    effective_q15 = (int32_t)(((int64_t)OUTPUT_HEADROOM_Q15 * vol_q15) >> 15);
  }

  float headroom = (float)headroom_q15 / 32768.0f;
  float vol = (float)vol_q15 / 32768.0f;
  float effective = (float)effective_q15 / 32768.0f;
  ESP_LOGI(TAG,
           "gain path: sw=%d headroom=%.3f vol=%.3f effective=%.3f q15=%ld",
           (s_ops && s_ops->software_volume) ? 1 : 0, headroom, vol, effective,
           (long)effective_q15);
}

static void apply_concealment_fade(int16_t *buf, size_t sample_count,
                                   uint8_t repeat_index) {
  int32_t gain_q15 = 32768;
  for (uint8_t i = 0; i < repeat_index; ++i) {
    gain_q15 = (gain_q15 * GAP_CONCEAL_FADE_Q15) >> 15;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    buf[i] = (int16_t)(((int32_t)buf[i] * gain_q15) >> 15);
  }
}

static void playback_task(void *arg) {
  int16_t *pcm = malloc((size_t)(AO_FRAME_SAMPLES + 1) * 2 * sizeof(int16_t));
  int16_t *silence = calloc((size_t)AO_FRAME_SAMPLES * 2, sizeof(int16_t));
  int16_t *resample_buf = malloc(AO_MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t));
  int16_t *last_good_buf = calloc((size_t)AO_MAX_RESAMPLE_FRAMES * 2,
                                  sizeof(int16_t));
  if (!pcm || !silence || !resample_buf) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    free(pcm);
    free(silence);
    free(resample_buf);
    free(last_good_buf);
    playback_running = false;
    playback_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  audio_eq_init((uint32_t)AO_OUTPUT_RATE);
  audio_dsp_init((uint32_t)AO_OUTPUT_RATE);
  audio_dsp_set_mode(s_fidelity_mode == AUDIO_FIDELITY_MODE_ENHANCED
                         ? AUDIO_DSP_MODE_ENHANCED
                         : AUDIO_DSP_MODE_LIMITER_ONLY);
  size_t last_good_samples = 0;
  uint8_t concealment_repeats = 0;
  uint32_t nonzero_write_count = 0;
  audio_stats_t prev_stats = {0};
  audio_dsp_stats_t prev_dsp = {0};

  while (playback_running) {
    uint64_t loop_start_us = (uint64_t)esp_timer_get_time();
    if (resample_reinit_needed) {
      resample_reinit_needed = false;
      audio_resample_init((uint32_t)source_rate, AO_OUTPUT_RATE, 2);
    }
    if (flush_requested) {
      flush_requested = false;
      audio_resample_reset();
      if (s_ops && s_ops->flush_output) {
        s_ops->flush_output(s_ops->ctx);
      }
    }

    size_t samples = audio_receiver_read(pcm, AO_FRAME_SAMPLES + 1);
    if (samples > 0) {
      int16_t *play_buf = pcm;
      size_t play_samples = samples;
      if (audio_resample_is_active()) {
        play_samples = audio_resample_process(pcm, samples, resample_buf,
                                              AO_MAX_RESAMPLE_FRAMES);
        play_buf = resample_buf;
      }
      int16_t input_peak = peak_abs_sample(play_buf, play_samples * 2);
      apply_volume(play_buf, play_samples * 2);
      audio_eq_process(play_buf, play_samples * 2);
      audio_dsp_process(play_buf, play_samples * 2);
      int16_t output_peak = peak_abs_sample(play_buf, play_samples * 2);
      if (last_good_buf && play_samples > 0) {
        memcpy(last_good_buf, play_buf, play_samples * 2 * sizeof(int16_t));
        last_good_samples = play_samples * 2;
      }
      concealment_repeats = 0;
      maybe_log_gain_path();

      if (s_eq_reload_pending) {
        int16_t reload_peak = output_peak;
        if (reload_peak <= EQ_RELOAD_PEAK_GUARD ||
            s_eq_reload_defer_count >= EQ_RELOAD_MAX_DEFER) {
          audio_eq_reload_from_settings();
          ESP_LOGI(TAG,
                   "EQ reload: applied(event) peak=%d defer_count=%u buffered=%u",
                   reload_peak, (unsigned)s_eq_reload_defer_count,
                   audio_receiver_get_buffered_frames());
          s_eq_reload_pending = false;
          s_eq_reload_defer_count = 0;
        } else {
          s_eq_reload_defer_count++;
          if (s_eq_reload_defer_count == 1 ||
              s_eq_reload_defer_count == EQ_RELOAD_MAX_DEFER) {
            ESP_LOGW(TAG,
                     "EQ reload deferred(event): peak=%d defer_count=%u/%d guard=%d",
                     reload_peak, (unsigned)s_eq_reload_defer_count,
                     EQ_RELOAD_MAX_DEFER, EQ_RELOAD_PEAK_GUARD);
          }
        }
      }

      if (++nonzero_write_count % 200 == 0) {
        ESP_LOGI(TAG, "[playback] wrote %u blocks, peak=%d, samples=%u",
                 nonzero_write_count, output_peak,
                 (unsigned)play_samples);
        if (output_peak >= CLIP_RISK_PEAK) {
          ESP_LOGW(TAG, "[playback] clip-risk peak=%d >= %d", output_peak,
                   CLIP_RISK_PEAK);
        }
      }
      if (nonzero_write_count % 500 == 0) {
        audio_stats_t cur_stats = {0};
        audio_dsp_stats_t cur_dsp = {0};
        audio_receiver_get_stats(&cur_stats);
        audio_dsp_get_stats(&cur_dsp);
        ESP_LOGI(TAG,
                 "[fidelity] mode=%s in_peak=%d out_peak=%d limiter+%lu late+%lu "
                 "drop+%lu underrun+%lu buffered=%u",
                 s_fidelity_mode == AUDIO_FIDELITY_MODE_ENHANCED ? "enhanced"
                                                                  : "pure",
                 input_peak, output_peak,
                 (unsigned long)(cur_dsp.limiter_events - prev_dsp.limiter_events),
                 (unsigned long)(cur_stats.late_frames - prev_stats.late_frames),
                 (unsigned long)(cur_stats.packets_dropped -
                                 prev_stats.packets_dropped),
                 (unsigned long)(cur_stats.buffer_underruns -
                                 prev_stats.buffer_underruns),
                 audio_receiver_get_buffered_frames());
        prev_stats = cur_stats;
        prev_dsp = cur_dsp;
      }
      if (s_ops && s_ops->write_pcm) {
        s_ops->write_pcm(s_ops->ctx, play_buf, play_samples * 4,
                         portMAX_DELAY);
      }
      audio_pipeline_note_playback_busy(
          (uint32_t)((uint64_t)esp_timer_get_time() - loop_start_us));
      taskYIELD();
    } else {
      static uint32_t zero_read_count = 0;
      if (++zero_read_count % 100 == 0) {
        uint32_t buffered_frames = audio_receiver_get_buffered_frames();
        bool anchor_valid = audio_receiver_anchor_valid();
        bool playing = audio_receiver_is_playing();
        if (playing && (anchor_valid || buffered_frames > 0)) {
          ESP_LOGW(TAG,
                   "[playback] 100x zero reads detected, buffer=%u, anchor=%d, "
                   "playing=%d",
                   buffered_frames, anchor_valid ? 1 : 0, playing ? 1 : 0);
        } else {
          ESP_LOGD(TAG,
                   "[playback] idle zero reads, buffer=%u, anchor=%d, "
                   "playing=%d",
                   buffered_frames, anchor_valid ? 1 : 0, playing ? 1 : 0);
        }
      }
      if (s_ops && s_ops->write_pcm) {
        if (last_good_buf && last_good_samples > 0 &&
            concealment_repeats < GAP_CONCEAL_REPEAT_MAX) {
          apply_concealment_fade(last_good_buf, last_good_samples,
                                 concealment_repeats);
          s_ops->write_pcm(s_ops->ctx, last_good_buf,
                           last_good_samples * sizeof(int16_t),
                           pdMS_TO_TICKS(10));
          audio_pipeline_note_gap_concealment(concealment_repeats == 0);
          concealment_repeats++;
        } else {
          s_ops->write_pcm(s_ops->ctx, silence,
                           (size_t)AO_FRAME_SAMPLES * 4, pdMS_TO_TICKS(10));
        }
      }
      audio_pipeline_note_playback_busy(
          (uint32_t)((uint64_t)esp_timer_get_time() - loop_start_us));
      vTaskDelay(1);
    }
  }

  free(pcm);
  free(silence);
  free(resample_buf);
  free(last_good_buf);
  playback_running = false;
  playback_task_handle = NULL;
  vTaskDelete(NULL);
}

void audio_output_common_init(const audio_output_hw_ops_t *ops) {
  s_ops = ops;
#if defined(CONFIG_AUDIO_FIDELITY_MODE_ENHANCED)
  s_fidelity_mode = AUDIO_FIDELITY_MODE_ENHANCED;
#else
  s_fidelity_mode = AUDIO_FIDELITY_MODE_PURE;
#endif
}

void audio_output_common_start(void) {
  if (playback_task_handle != NULL) {
    return;
  }
  flush_requested = false;
  playback_running = true;
  const char *name = (s_ops && s_ops->task_name) ? s_ops->task_name
                                                  : "audio_play";
  xTaskCreatePinnedToCore(playback_task, name, 4096, NULL, 7,
                          &playback_task_handle, AO_PLAYBACK_CORE);
}

void audio_output_common_stop(void) {
  if (playback_task_handle == NULL) {
    return;
  }
  flush_requested = false;
  playback_running = false;
  int timeout = 40;
  while (playback_task_handle != NULL && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (playback_task_handle != NULL) {
    ESP_LOGW(TAG, "Playback task did not exit within timeout");
  } else {
    ESP_LOGI(TAG, "Playback task stopped");
  }
}

void audio_output_common_flush(void) {
  if (flush_requested) {
    return;
  }
  flush_requested = true;
}

void audio_output_common_set_source_rate(int rate) {
  if (rate > 0 && rate != source_rate) {
    source_rate = rate;
    resample_reinit_needed = true;
  }
}

bool audio_output_common_is_active(void) {
  return playback_task_handle != NULL && playback_running;
}

void audio_output_common_set_fidelity_mode(audio_fidelity_mode_t mode) {
  s_fidelity_mode = mode;
  audio_dsp_set_mode(mode == AUDIO_FIDELITY_MODE_ENHANCED
                         ? AUDIO_DSP_MODE_ENHANCED
                         : AUDIO_DSP_MODE_LIMITER_ONLY);
  if (mode == AUDIO_FIDELITY_MODE_PURE) {
    s_eq_reload_pending = false;
    s_eq_reload_defer_count = 0;
  }
  ESP_LOGI(TAG, "fidelity mode: %s",
           mode == AUDIO_FIDELITY_MODE_ENHANCED ? "enhanced" : "pure");
}

audio_fidelity_mode_t audio_output_common_get_fidelity_mode(void) {
  return s_fidelity_mode;
}
