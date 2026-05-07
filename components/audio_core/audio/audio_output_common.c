#include "audio_output_common.h"

#include "audio_pipeline.h"
#include "audio_dsp.h"
#include "audio_eq.h"
#include "audio_receiver.h"
#include "audio_resample.h"
#include "audio_volume.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdlib.h>

#define TAG "audio_output"

// Reserve headroom to reduce clipping at loud passages.
#define OUTPUT_HEADROOM_Q15 21900
#define CLIP_RISK_PEAK      30000
#define EQ_RELOAD_PEAK_GUARD 22000
#define EQ_RELOAD_MAX_DEFER  12
#define GAP_CONCEAL_REPEAT_MAX 16
#define GAP_CONCEAL_FADE_Q15   26214
#define STACK_LOG_INTERVAL_US 15000000ULL
#define REBUFFER_LOG_INTERVAL_US 1000000ULL
#define REBUFFER_STARVATION_LOG_US 3000000ULL
#define REBUFFER_COOLDOWN_US 1500000ULL
#define REBUFFER_RESUME_STREAK_REQUIRED 3U

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
static StaticTask_t s_playback_tcb;
static StackType_t *s_playback_stack = NULL;
#define PLAYBACK_STACK_SIZE 3584

static void log_stack_watermark(const char *reason) {
  UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
  ESP_LOGI(TAG, "stack watermark[%s]: free_words=%lu free_bytes=%lu",
           reason, (unsigned long)watermark,
           (unsigned long)(watermark * sizeof(StackType_t)));
}

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
    float vol_db = 0.0f;
    audio_volume_load(&vol_db);
    int32_t vol_q15 = audio_volume_db_to_q15(vol_db);
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
    float vol_db = 0.0f;
    audio_volume_load(&vol_db);
    vol_q15 = audio_volume_db_to_q15(vol_db);
    effective_q15 = (int32_t)(((int64_t)OUTPUT_HEADROOM_Q15 * vol_q15) >> 15);
  }

  float headroom = (float)headroom_q15 / 32768.0f;
  float vol = (float)vol_q15 / 32768.0f;
  float effective = (float)effective_q15 / 32768.0f;
  ESP_LOGD(TAG,
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

static uint32_t stats_delta_u32(uint32_t current, uint32_t base) {
  return current >= base ? (current - base) : current;
}

static uint32_t rebuffer_resume_frames(uint32_t target_frames) {
  uint32_t target = target_frames > 0 ? target_frames : 4U;
  uint32_t plus_margin = target + 2U;
  uint32_t scaled = (target * 11U + 4U) / 10U;
  return plus_margin > scaled ? plus_margin : scaled;
}

static uint32_t rebuffer_low_water_frames(uint32_t target_frames) {
  if (target_frames <= 4U) {
    return target_frames > 0 ? target_frames : 1U;
  }
  if (target_frames <= 8U) {
    return target_frames / 2U;
  }
  uint32_t third = target_frames / 3U;
  return third > 4U ? third : 4U;
}

/* Maximum possible resampled frames: 48000/44100 ratio with headroom.
 * Computed as a plain constant so it can be used for static arrays. */
#define PLAYBACK_RESAMPLE_BUF_FRAMES 400

static void playback_task(void *arg) {
  static int16_t *s_pcm_buf = NULL;
  static int16_t *s_silence_buf = NULL;
  static int16_t *s_resample_buf = NULL;
  static int16_t *s_last_good_buf = NULL;

  if (!s_pcm_buf) {
    s_pcm_buf = heap_caps_malloc((AO_FRAME_SAMPLES + 1) * 2 * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pcm_buf) s_pcm_buf = malloc((AO_FRAME_SAMPLES + 1) * 2 * sizeof(int16_t));
  }
  if (!s_silence_buf) {
    s_silence_buf = heap_caps_malloc(AO_FRAME_SAMPLES * 2 * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_silence_buf) s_silence_buf = malloc(AO_FRAME_SAMPLES * 2 * sizeof(int16_t));
  }
  if (!s_resample_buf) {
    s_resample_buf = heap_caps_malloc(PLAYBACK_RESAMPLE_BUF_FRAMES * 2 * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_resample_buf) s_resample_buf = malloc(PLAYBACK_RESAMPLE_BUF_FRAMES * 2 * sizeof(int16_t));
  }
  if (!s_last_good_buf) {
    s_last_good_buf = heap_caps_malloc(PLAYBACK_RESAMPLE_BUF_FRAMES * 2 * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_last_good_buf) s_last_good_buf = malloc(PLAYBACK_RESAMPLE_BUF_FRAMES * 2 * sizeof(int16_t));
  }
  if (!s_pcm_buf || !s_silence_buf || !s_resample_buf || !s_last_good_buf) {
    ESP_LOGE(TAG, "Failed to allocate playback buffers");
    playback_running = false;
    playback_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int16_t *pcm = s_pcm_buf;
  int16_t *silence = s_silence_buf;
  int16_t *resample_buf = s_resample_buf;
  int16_t *last_good_buf = s_last_good_buf;

  memset(silence, 0, AO_FRAME_SAMPLES * 2 * sizeof(int16_t));
  memset(last_good_buf, 0, PLAYBACK_RESAMPLE_BUF_FRAMES * 2 * sizeof(int16_t));

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
  audio_stats_t stable_stats_base = {0};
  bool have_stable_stats_base = false;
  audio_stats_t rebuffer_entry_stats = {0};
  bool have_rebuffer_entry_stats = false;
  uint64_t last_stack_log_us = (uint64_t)esp_timer_get_time();
  bool rebuffering = false;
  bool rebuffer_starvation_logged = false;
  uint32_t rebuffer_gap_events = 0;
  uint64_t rebuffer_start_us = 0;
  uint64_t rebuffer_last_log_us = 0;
  uint64_t rebuffer_cooldown_until_us = 0;
  uint32_t rebuffer_resume_streak = 0;
  uint32_t zero_read_count = 0;

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

    uint32_t buffered_frames = audio_receiver_get_buffered_frames();
    uint32_t target_frames = audio_receiver_get_target_buffer_frames();
    bool anchor_valid = audio_receiver_anchor_valid();
    bool playing = audio_receiver_is_playing();
    audio_stream_type_t stream_type = audio_receiver_get_stream_type();

    if (rebuffering) {
      uint32_t resume_frames = rebuffer_resume_frames(target_frames);
      if (stream_type != AUDIO_STREAM_REALTIME || !playing || !anchor_valid) {
        rebuffering = false;
        rebuffer_starvation_logged = false;
        have_rebuffer_entry_stats = false;
        rebuffer_resume_streak = 0;
      } else {
        audio_stats_t cur_stats = {0};
        audio_receiver_get_stats(&cur_stats);
        if (buffered_frames >= resume_frames) {
          rebuffer_resume_streak++;
        } else {
          rebuffer_resume_streak = 0;
        }
        if ((loop_start_us - rebuffer_last_log_us) >= REBUFFER_LOG_INTERVAL_US) {
          ESP_LOGI(TAG,
                   "[playback] low-water rebuffer wait: buffered=%u target=%u resume=%u streak=%u/%u elapsed_ms=%llu nack+%lu drop+%lu underrun+%lu",
                   buffered_frames, target_frames > 0 ? target_frames : 4U, resume_frames,
                   rebuffer_resume_streak, REBUFFER_RESUME_STREAK_REQUIRED,
                   (unsigned long long)((loop_start_us - rebuffer_start_us) / 1000ULL),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.nack_requests_sent,
                                                         rebuffer_entry_stats.nack_requests_sent)
                                       : cur_stats.nack_requests_sent),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.packets_dropped,
                                                         rebuffer_entry_stats.packets_dropped)
                                       : cur_stats.packets_dropped),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.buffer_underruns,
                                                         rebuffer_entry_stats.buffer_underruns)
                                       : cur_stats.buffer_underruns));
          rebuffer_last_log_us = loop_start_us;
        }
        if (!rebuffer_starvation_logged &&
            (loop_start_us - rebuffer_start_us) >= REBUFFER_STARVATION_LOG_US) {
          ESP_LOGW(TAG,
                   "[playback] network starvation suspected: buffered=%u target=%u nack+%lu drop+%lu underrun+%lu late+%lu",
                   buffered_frames, resume_frames,
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.nack_requests_sent,
                                                         rebuffer_entry_stats.nack_requests_sent)
                                       : cur_stats.nack_requests_sent),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.packets_dropped,
                                                         rebuffer_entry_stats.packets_dropped)
                                       : cur_stats.packets_dropped),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.buffer_underruns,
                                                         rebuffer_entry_stats.buffer_underruns)
                                       : cur_stats.buffer_underruns),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.late_frames,
                                                         rebuffer_entry_stats.late_frames)
                                       : cur_stats.late_frames));
          rebuffer_starvation_logged = true;
        }
        if (rebuffer_resume_streak >= REBUFFER_RESUME_STREAK_REQUIRED) {
          ESP_LOGI(TAG,
                   "[playback] low-water rebuffer done: elapsed_ms=%llu buffered=%u target=%u resume=%u streak=%u nack+%lu drop+%lu underrun+%lu",
                   (unsigned long long)((loop_start_us - rebuffer_start_us) / 1000ULL),
                   buffered_frames, target_frames > 0 ? target_frames : 4U, resume_frames,
                   rebuffer_resume_streak,
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.nack_requests_sent,
                                                         rebuffer_entry_stats.nack_requests_sent)
                                       : cur_stats.nack_requests_sent),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.packets_dropped,
                                                         rebuffer_entry_stats.packets_dropped)
                                       : cur_stats.packets_dropped),
                   (unsigned long)(have_rebuffer_entry_stats
                                       ? stats_delta_u32(cur_stats.buffer_underruns,
                                                         rebuffer_entry_stats.buffer_underruns)
                                       : cur_stats.buffer_underruns));
          rebuffering = false;
          rebuffer_starvation_logged = false;
          have_rebuffer_entry_stats = false;
          zero_read_count = 0;
          rebuffer_gap_events = 0;
          concealment_repeats = 0;
          rebuffer_resume_streak = 0;
          rebuffer_cooldown_until_us = loop_start_us + REBUFFER_COOLDOWN_US;
          stable_stats_base = cur_stats;
          have_stable_stats_base = true;
        }
      }

      if (rebuffering) {
        if (s_ops && s_ops->write_pcm) {
          s_ops->write_pcm(s_ops->ctx, silence,
                           (size_t)AO_FRAME_SAMPLES * 2 * sizeof(int16_t),
                           pdMS_TO_TICKS(50));
        }
        audio_pipeline_note_playback_busy(
            (uint32_t)((uint64_t)esp_timer_get_time() - loop_start_us));
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if ((now_us - last_stack_log_us) >= STACK_LOG_INTERVAL_US) {
          log_stack_watermark("audio_play");
          last_stack_log_us = now_us;
        }
        vTaskDelay(1);
        continue;
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
#if AO_OUTPUT_CHANNELS == 1
      for (size_t i = 0; i < play_samples; i++) {
        int32_t l = (int32_t)play_buf[i * 2];
        int32_t r = (int32_t)play_buf[i * 2 + 1];
        int16_t mono = (int16_t)((l + r) / 2);
        play_buf[i * 2] = mono;
        play_buf[i * 2 + 1] = mono;
      }
      if (last_good_buf && play_samples > 0) {
        memcpy(last_good_buf, play_buf, play_samples * 2 * sizeof(int16_t));
        last_good_samples = play_samples * 2;
      }
#else
      if (last_good_buf && play_samples > 0) {
        memcpy(last_good_buf, play_buf, play_samples * 2 * sizeof(int16_t));
        last_good_samples = play_samples * 2;
      }
#endif
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

      if (++nonzero_write_count % 1000 == 0) {
        ESP_LOGD(TAG, "[playback] wrote %u blocks, peak=%d, samples=%u",
                 nonzero_write_count, output_peak, (unsigned)play_samples);
        if (output_peak >= CLIP_RISK_PEAK) {
          ESP_LOGW(TAG, "[playback] clip-risk peak=%d >= %d", output_peak,
                   CLIP_RISK_PEAK);
        }
      }
      if (nonzero_write_count % 1000 == 0) {
        audio_stats_t cur_stats = {0};
        audio_dsp_stats_t cur_dsp = {0};
        audio_receiver_get_stats(&cur_stats);
        audio_dsp_get_stats(&cur_dsp);
        ESP_LOGI(TAG,
                 "[fidelity] mode=%s in_peak=%d out_peak=%d limiter+%lu late+%lu "
                 "drop+%lu underrun+%lu buffered=%u source=real",
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
      if (!have_stable_stats_base) {
        audio_receiver_get_stats(&stable_stats_base);
        have_stable_stats_base = true;
      }
      zero_read_count = 0;
      rebuffer_gap_events = 0;
      if (s_ops && s_ops->write_pcm) {
        s_ops->write_pcm(s_ops->ctx, play_buf, play_samples * 2 * sizeof(int16_t),
                         portMAX_DELAY);
      }
      audio_pipeline_note_playback_busy(
          (uint32_t)((uint64_t)esp_timer_get_time() - loop_start_us));
      uint64_t now_us = (uint64_t)esp_timer_get_time();
      if ((now_us - last_stack_log_us) >= STACK_LOG_INTERVAL_US) {
        log_stack_watermark("audio_play");
        last_stack_log_us = now_us;
      }
      taskYIELD();
    } else {
      bool suppress_concealment = false;
      zero_read_count++;
      if (stream_type == AUDIO_STREAM_REALTIME && playing && anchor_valid) {
        uint32_t low_water_frames = rebuffer_low_water_frames(target_frames);
        bool cooldown_active = loop_start_us < rebuffer_cooldown_until_us;
        if (!rebuffering && buffered_frames <= low_water_frames &&
            zero_read_count >= 2) {
          audio_stats_t cur_stats = {0};
          audio_receiver_get_stats(&cur_stats);
          rebuffer_entry_stats = have_stable_stats_base ? stable_stats_base : cur_stats;
          have_rebuffer_entry_stats = true;
          ESP_LOGW(TAG,
                   "[playback] low-water rebuffer start: buffered=%u low=%u target=%u resume=%u zero_reads=%u gap=%u cooldown=%d nack+%lu drop+%lu underrun+%lu",
                   buffered_frames, low_water_frames, target_frames > 0 ? target_frames : 4U,
                   rebuffer_resume_frames(target_frames),
                   zero_read_count, rebuffer_gap_events,
                   cooldown_active ? 1 : 0,
                   (unsigned long)stats_delta_u32(cur_stats.nack_requests_sent,
                                                  rebuffer_entry_stats.nack_requests_sent),
                   (unsigned long)stats_delta_u32(cur_stats.packets_dropped,
                                                  rebuffer_entry_stats.packets_dropped),
                   (unsigned long)stats_delta_u32(cur_stats.buffer_underruns,
                                                  rebuffer_entry_stats.buffer_underruns));
          audio_receiver_rebuffer_start();
          rebuffering = true;
          rebuffer_starvation_logged = false;
          rebuffer_start_us = loop_start_us;
          rebuffer_last_log_us = loop_start_us;
          rebuffer_resume_streak = 0;
          concealment_repeats = GAP_CONCEAL_REPEAT_MAX;
          suppress_concealment = true;
        }
      }
      if (zero_read_count % 100 == 0) {
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
        if (!suppress_concealment &&
            last_good_buf && last_good_samples > 0 &&
            concealment_repeats < GAP_CONCEAL_REPEAT_MAX) {
          apply_concealment_fade(last_good_buf, last_good_samples,
                                 concealment_repeats);
          int16_t concealment_peak = peak_abs_sample(last_good_buf, last_good_samples);
          if (concealment_repeats == 0) {
            ESP_LOGW(TAG, "[playback] gap_concealment start: repeat=%u peak=%d", 
                     concealment_repeats, concealment_peak);
          }
          s_ops->write_pcm(s_ops->ctx, last_good_buf,
                           last_good_samples * sizeof(int16_t),
                           pdMS_TO_TICKS(50));
          audio_pipeline_note_gap_concealment(concealment_repeats == 0);
          if (concealment_repeats == 0) {
            rebuffer_gap_events++;
          }
          concealment_repeats++;
        } else {
          if (concealment_repeats == 0) {
            ESP_LOGW(TAG, "[playback] silence fill: no last_good_buf, buffer=%u, anchor=%d",
                     buffered_frames, anchor_valid ? 1 : 0);
          }
          s_ops->write_pcm(s_ops->ctx, silence,
                           (size_t)AO_FRAME_SAMPLES * 2 * sizeof(int16_t), pdMS_TO_TICKS(50));
        }
      }
      audio_pipeline_note_playback_busy(
          (uint32_t)((uint64_t)esp_timer_get_time() - loop_start_us));
      uint64_t now_us = (uint64_t)esp_timer_get_time();
      if ((now_us - last_stack_log_us) >= STACK_LOG_INTERVAL_US) {
        log_stack_watermark("audio_play");
        last_stack_log_us = now_us;
      }
      vTaskDelay(1);
    }
  }

  playback_running = false;
  playback_task_handle = NULL;
  log_stack_watermark("audio_play_exit");
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
  if (!s_playback_stack) {
    s_playback_stack = heap_caps_malloc(PLAYBACK_STACK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_playback_stack) {
      s_playback_stack = heap_caps_malloc(PLAYBACK_STACK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
  }
  if (!s_playback_stack) {
    ESP_LOGE(TAG, "Failed to allocate playback stack");
    playback_running = false;
    return;
  }
  playback_task_handle = xTaskCreateStaticPinnedToCore(
      playback_task, name, PLAYBACK_STACK_SIZE / sizeof(StackType_t), NULL, 7,
      s_playback_stack, &s_playback_tcb, AO_PLAYBACK_CORE);
  if (playback_task_handle == NULL) {
    ESP_LOGE(TAG, "Failed to create playback task");
    playback_running = false;
  }
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
