#include "voice_frontend.h"

#include "afe_bridge.h"
#include "audio/audio_output.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "voice_dsp.h"
#include "voice_reference.h"

#include <string.h>

#ifndef CONFIG_VOICE_MIC_IN_GAIN_DB
#define CONFIG_VOICE_MIC_IN_GAIN_DB 33
#endif

#define TAG "voice_frontend"
#define VOICE_FE_CAP_TASK_STACK 10240
#define VOICE_FE_CAP_TASK_PRIO 5
#define VOICE_FE_FETCH_TASK_STACK 6144
#define VOICE_FE_FETCH_TASK_PRIO 4
#define VOICE_FE_QUEUE_LEN 8
#define VOICE_FE_AUDIO_SLOTS 4
#define VOICE_HW_CHANNELS 2
#define VOICE_MIC_EMPTY_READ_TOLERANCE 16
#define VOICE_MIC_EMPTY_READ_LOG_INTERVAL 8
#define VOICE_FE_FETCH_WAIT_MS 100
#define VOICE_FE_FETCH_MAX_BATCH 8

typedef enum {
  FE_STATE_STOPPED = 0,
  FE_STATE_STARTING,
  FE_STATE_RUNNING,
  FE_STATE_STOPPING,
  FE_STATE_BLOCKED,
} voice_fe_state_t;

typedef struct {
  int16_t *pcm;
  size_t frames;
  uint32_t seq;
} voice_fe_audio_slot_t;

typedef struct {
  TaskHandle_t cap_task;
  TaskHandle_t fetch_task;
  QueueHandle_t queue;
  bool running;
  bool stopping;
  voice_fe_state_t state;
  esp_codec_dev_handle_t mic;
  portMUX_TYPE slot_mux;
  uint16_t next_slot;
  voice_fe_audio_slot_t slots[VOICE_FE_AUDIO_SLOTS];
  int16_t *afe_mic_accum;
  size_t afe_mic_accum_cap;
  size_t afe_mic_accum_len;
  int16_t *afe_ref_accum;
  size_t afe_ref_accum_cap;
  size_t afe_ref_accum_len;
  int16_t *afe_feed_buf;
  size_t afe_feed_buf_cap;
  uint32_t feed_ok_count;
  uint32_t fetch_ok_count;
  uint32_t mic_read_ok_count;
  uint32_t mic_read_fail_count;
  uint32_t queue_drop_count;
  uint32_t fetch_timeout_count;
  uint32_t fetch_yield_count;
  uint32_t yield_progress_count;
  uint32_t yield_no_progress_count;
  uint32_t last_loop_hz;
  uint32_t wake_detect_count;
  uint32_t wake_forward_count;
  uint64_t last_read_ok_ms;
  uint64_t last_feed_ok_ms;
  uint64_t last_fetch_ms;
} voice_fe_ctx_t;

static voice_fe_ctx_t s_fe = {0};

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

static void interleave_mic_ref(const int16_t *mic, const int16_t *ref, int16_t *dst, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    dst[i * 2] = mic[i];
    dst[i * 2 + 1] = ref[i];
  }
}

static void fe_accum_free(void) {
  voice_buf_free(s_fe.afe_mic_accum);
  s_fe.afe_mic_accum = NULL;
  s_fe.afe_mic_accum_cap = 0;
  s_fe.afe_mic_accum_len = 0;
  voice_buf_free(s_fe.afe_ref_accum);
  s_fe.afe_ref_accum = NULL;
  s_fe.afe_ref_accum_cap = 0;
  s_fe.afe_ref_accum_len = 0;
  voice_buf_free(s_fe.afe_feed_buf);
  s_fe.afe_feed_buf = NULL;
  s_fe.afe_feed_buf_cap = 0;
}

static void fe_slots_free(void) {
  for (size_t i = 0; i < VOICE_FE_AUDIO_SLOTS; ++i) {
    voice_buf_free(s_fe.slots[i].pcm);
    s_fe.slots[i].pcm = NULL;
    s_fe.slots[i].frames = 0;
    s_fe.slots[i].seq = 0;
  }
}

static void fe_push_event(const voice_fe_event_t *ev) {
  if (s_fe.queue == NULL || ev == NULL) return;
  if (xQueueSend(s_fe.queue, ev, 0) != pdTRUE) {
    s_fe.queue_drop_count++;
  }
}

static inline void fe_set_last_read_ok_ms(uint64_t ts) {
  portENTER_CRITICAL(&s_fe.slot_mux);
  s_fe.last_read_ok_ms = ts;
  portEXIT_CRITICAL(&s_fe.slot_mux);
}

static inline void fe_set_last_feed_ok_ms(uint64_t ts) {
  portENTER_CRITICAL(&s_fe.slot_mux);
  s_fe.last_feed_ok_ms = ts;
  portEXIT_CRITICAL(&s_fe.slot_mux);
}

static inline void fe_set_last_fetch_ms(uint64_t ts) {
  portENTER_CRITICAL(&s_fe.slot_mux);
  s_fe.last_fetch_ms = ts;
  portEXIT_CRITICAL(&s_fe.slot_mux);
}

static void fe_capture_task(void *arg) {
  (void)arg;
  const uint32_t hw_rate = voice_hw_codec_rate_hz();
  size_t hw_frame_samples = ((size_t)hw_rate * (size_t)CONFIG_VOICE_UPLINK_FRAME_MS) / 1000;
  if (hw_frame_samples < 128) hw_frame_samples = 128;

  size_t pcm_cap = hw_frame_samples;
  if (voice_rs_cap_rs() != NULL) {
    pcm_cap = (size_t)resampleGetExpectedOutput(voice_rs_cap_rs(), (int)hw_frame_samples,
                                                voice_rs_cap_ratio()) + 16;
    if (pcm_cap < 160) pcm_cap = 160;
  }
  int16_t *pcm = (int16_t *)voice_buf_alloc(pcm_cap * sizeof(int16_t));
  int16_t *mono_hw = (int16_t *)voice_buf_alloc(hw_frame_samples * sizeof(int16_t));
  int16_t *hw_pcm =
      (int16_t *)voice_buf_alloc(hw_frame_samples * VOICE_HW_CHANNELS * sizeof(int16_t));
  if (pcm == NULL || mono_hw == NULL || hw_pcm == NULL) {
    voice_fe_event_t ev = {.type = VOICE_FE_EVENT_ERROR, .err = ESP_ERR_NO_MEM, .timestamp_ms = now_ms()};
    fe_push_event(&ev);
    goto done;
  }

  if (!voice_rs_cap_ensure()) {
    voice_fe_event_t ev = {.type = VOICE_FE_EVENT_ERROR, .err = ESP_FAIL, .timestamp_ms = now_ms()};
    fe_push_event(&ev);
    goto done;
  }

  uint64_t last_good_read_ms = now_ms();
  uint32_t empty_read_streak = 0;
  uint64_t diag_ms = 0;
  uint64_t yield_diag_ms = 0;
  uint32_t yield_progress_count = 0;
  uint32_t yield_no_progress_count = 0;
  uint32_t loops_1s = 0;
  uint64_t loop_hz_ms = 0;
  uint32_t no_progress_backoff_ms = 2;
  s_fe.state = FE_STATE_RUNNING;

  while (s_fe.running) {
    bool made_progress = false;
    loops_1s++;
    if (!afe_bridge_is_ready()) {
      s_fe.state = FE_STATE_BLOCKED;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    s_fe.state = FE_STATE_RUNNING;

    int want = (int)(hw_frame_samples * VOICE_HW_CHANNELS * sizeof(int16_t));
    int ret = esp_codec_dev_read(s_fe.mic, hw_pcm, want);
    if (ret != ESP_CODEC_DEV_OK) {
      s_fe.mic_read_fail_count++;
      empty_read_streak++;
      if (empty_read_streak <= VOICE_MIC_EMPTY_READ_TOLERANCE) {
        if ((empty_read_streak % VOICE_MIC_EMPTY_READ_LOG_INTERVAL) == 0U) {
          ESP_LOGW(TAG, "MIC_READ_EMPTY ret=%d streak=%lu/%d", ret,
                   (unsigned long)empty_read_streak, VOICE_MIC_EMPTY_READ_TOLERANCE);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if ((now_ms() - last_good_read_ms) >= (uint64_t)CONFIG_VOICE_CAPTURE_STALL_TIMEOUT_MS) {
        static uint64_t s_last_stall_log_ms;
        uint64_t n = now_ms();
        if (n - s_last_stall_log_ms >= 1000ULL) {
          ESP_LOGW(TAG, "mic read stalled, keeping persistent mic open");
          s_last_stall_log_ms = n;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    empty_read_streak = 0;
    last_good_read_ms = now_ms();
    s_fe.mic_read_ok_count++;
    fe_set_last_read_ok_ms(last_good_read_ms);
    made_progress = true;
    size_t got_samples = ((size_t)want / sizeof(int16_t)) / VOICE_HW_CHANNELS;
    if (got_samples == 0) continue;
    for (size_t i = 0; i < got_samples; ++i) {
      int32_t l = hw_pcm[i * VOICE_HW_CHANNELS];
      int32_t r = hw_pcm[i * VOICE_HW_CHANNELS + 1];
      mono_hw[i] = (int16_t)((l + r) / 2);
    }
    size_t api_frames = got_samples;
    if (voice_rs_cap_rs() != NULL) {
      api_frames = voice_rs_process_mono(voice_rs_cap_rs(), voice_rs_cap_ratio(), mono_hw,
                                         got_samples, pcm, pcm_cap);
      if (api_frames == 0) continue;
    } else if (got_samples <= pcm_cap) {
      memcpy(pcm, mono_hw, got_samples * sizeof(int16_t));
      api_frames = got_samples;
    } else {
      continue;
    }

    int feed_ch = afe_bridge_get_feed_chunksize();
    size_t mic_new_len = s_fe.afe_mic_accum_len + api_frames;
    if (mic_new_len > s_fe.afe_mic_accum_cap) {
      size_t cap = s_fe.afe_mic_accum_cap > 0 ? s_fe.afe_mic_accum_cap : 512;
      while (cap < mic_new_len) cap *= 2;
      int16_t *p = (int16_t *)voice_buf_alloc(cap * sizeof(int16_t));
      if (p == NULL) {
        static uint64_t s_last_alloc_log_ms;
        uint64_t n = now_ms();
        if (n - s_last_alloc_log_ms >= 1000ULL) {
          ESP_LOGW(TAG, "feed skipped: alloc_fail target=mic_accum cap=%lu",
                   (unsigned long)cap);
          s_last_alloc_log_ms = n;
        }
        continue;
      }
      if (s_fe.afe_mic_accum != NULL && s_fe.afe_mic_accum_len > 0) {
        memcpy(p, s_fe.afe_mic_accum, s_fe.afe_mic_accum_len * sizeof(int16_t));
      }
      voice_buf_free(s_fe.afe_mic_accum);
      s_fe.afe_mic_accum = p;
      s_fe.afe_mic_accum_cap = cap;
    }
    memcpy(s_fe.afe_mic_accum + s_fe.afe_mic_accum_len, pcm, api_frames * sizeof(int16_t));
    s_fe.afe_mic_accum_len = mic_new_len;

    size_t need_ref = s_fe.afe_mic_accum_len;
    if (need_ref > s_fe.afe_ref_accum_len) {
      size_t pop = need_ref - s_fe.afe_ref_accum_len;
      size_t new_ref_len = s_fe.afe_ref_accum_len + pop;
      if (new_ref_len > s_fe.afe_ref_accum_cap) {
        size_t cap = s_fe.afe_ref_accum_cap > 0 ? s_fe.afe_ref_accum_cap : 512;
        while (cap < new_ref_len) cap *= 2;
        int16_t *p = (int16_t *)voice_buf_alloc(cap * sizeof(int16_t));
        if (p == NULL) {
          static uint64_t s_last_alloc_log_ms;
          uint64_t n = now_ms();
          if (n - s_last_alloc_log_ms >= 1000ULL) {
            ESP_LOGW(TAG, "feed skipped: alloc_fail target=ref_accum cap=%lu",
                     (unsigned long)cap);
            s_last_alloc_log_ms = n;
          }
          continue;
        }
        if (s_fe.afe_ref_accum != NULL && s_fe.afe_ref_accum_len > 0) {
          memcpy(p, s_fe.afe_ref_accum, s_fe.afe_ref_accum_len * sizeof(int16_t));
        }
        voice_buf_free(s_fe.afe_ref_accum);
        s_fe.afe_ref_accum = p;
        s_fe.afe_ref_accum_cap = cap;
      }
      size_t popped = voice_reference_ring_pop(s_fe.afe_ref_accum + s_fe.afe_ref_accum_len, pop);
      s_fe.afe_ref_accum_len += popped;
      if (popped < pop) {
        memset(s_fe.afe_ref_accum + s_fe.afe_ref_accum_len, 0, (pop - popped) * sizeof(int16_t));
        s_fe.afe_ref_accum_len += (pop - popped);
      }
    }

    if (s_fe.afe_mic_accum_len < (size_t)feed_ch) {
      static uint64_t s_last_insufficient_log_ms;
      uint64_t n = now_ms();
      if (n - s_last_insufficient_log_ms >= 1000ULL) {
        ESP_LOGW(TAG, "feed skipped: mic_insufficient mic=%lu feed_ch=%d",
                 (unsigned long)s_fe.afe_mic_accum_len, feed_ch);
        s_last_insufficient_log_ms = n;
      }
    }

    bool alloc_fail_this_loop = false;
    while (s_fe.afe_mic_accum_len >= (size_t)feed_ch && s_fe.afe_ref_accum_len >= (size_t)feed_ch) {
      size_t feed_samples = (size_t)feed_ch * 2;
      if (s_fe.afe_feed_buf_cap < feed_samples) {
        voice_buf_free(s_fe.afe_feed_buf);
        s_fe.afe_feed_buf = (int16_t *)voice_buf_alloc(feed_samples * sizeof(int16_t));
        s_fe.afe_feed_buf_cap = s_fe.afe_feed_buf != NULL ? feed_samples : 0;
      }
      if (s_fe.afe_feed_buf == NULL) {
        alloc_fail_this_loop = true;
        break;
      }
      interleave_mic_ref(s_fe.afe_mic_accum, s_fe.afe_ref_accum, s_fe.afe_feed_buf, (size_t)feed_ch);
      if (afe_bridge_feed(s_fe.afe_feed_buf, (size_t)feed_ch) != ESP_OK) break;
      s_fe.feed_ok_count++;
      fe_set_last_feed_ok_ms(now_ms());
      made_progress = true;
      size_t rem_mic = s_fe.afe_mic_accum_len - (size_t)feed_ch;
      if (rem_mic > 0) memmove(s_fe.afe_mic_accum, s_fe.afe_mic_accum + feed_ch, rem_mic * sizeof(int16_t));
      s_fe.afe_mic_accum_len = rem_mic;
      size_t rem_ref = s_fe.afe_ref_accum_len - (size_t)feed_ch;
      if (rem_ref > 0) memmove(s_fe.afe_ref_accum, s_fe.afe_ref_accum + feed_ch, rem_ref * sizeof(int16_t));
      s_fe.afe_ref_accum_len = rem_ref;
    }
    if (alloc_fail_this_loop) {
      static uint64_t s_last_alloc_log_ms;
      uint64_t n = now_ms();
      if (n - s_last_alloc_log_ms >= 1000ULL) {
        ESP_LOGW(TAG, "feed skipped: alloc_fail target=feed_buf samples=%lu",
                 (unsigned long)((size_t)feed_ch * 2));
        s_last_alloc_log_ms = n;
      }
    }

    if ((now_ms() - diag_ms) >= 10000ULL) {
      uint64_t n = now_ms();
      uint64_t read_age = s_fe.last_read_ok_ms > 0 ? (n - s_fe.last_read_ok_ms) : 0;
      uint64_t feed_age = s_fe.last_feed_ok_ms > 0 ? (n - s_fe.last_feed_ok_ms) : 0;
      uint64_t fetch_age = s_fe.last_fetch_ms > 0 ? (n - s_fe.last_fetch_ms) : 0;
      ESP_LOGI(TAG,
               "diag: mic_read_ok=%lu read_fail=%lu feed_ok=%lu fetch_ok=%lu "
               "feed_pending_frames=%lu q_drop=%lu loop_hz=%lu wake=%lu/%lu "
               "fetch_timeout=%lu fetch_yield=%lu "
               "age(read/feed/fetch)=%llums/%llums/%llums",
               (unsigned long)s_fe.mic_read_ok_count, (unsigned long)s_fe.mic_read_fail_count,
               (unsigned long)s_fe.feed_ok_count, (unsigned long)s_fe.fetch_ok_count,
               (unsigned long)s_fe.afe_mic_accum_len, (unsigned long)s_fe.queue_drop_count,
               (unsigned long)s_fe.last_loop_hz,
               (unsigned long)s_fe.wake_detect_count,
               (unsigned long)s_fe.wake_forward_count,
               (unsigned long)s_fe.fetch_timeout_count,
               (unsigned long)s_fe.fetch_yield_count,
               (unsigned long long)read_age, (unsigned long long)feed_age,
               (unsigned long long)fetch_age);
      diag_ms = n;
    }

    if (made_progress) {
      yield_progress_count++;
      s_fe.yield_progress_count++;
      no_progress_backoff_ms = 2;
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      yield_no_progress_count++;
      s_fe.yield_no_progress_count++;
      vTaskDelay(pdMS_TO_TICKS(no_progress_backoff_ms));
      if (no_progress_backoff_ms < 8) no_progress_backoff_ms *= 2;
    }

    if ((now_ms() - loop_hz_ms) >= 1000ULL) {
      s_fe.last_loop_hz = loops_1s;
      loops_1s = 0;
      loop_hz_ms = now_ms();
    }

    if ((now_ms() - yield_diag_ms) >= 5000ULL) {
      uint64_t n = now_ms();
      ESP_LOGI(TAG, "voice_fe yield: progress=%lu no_progress=%lu",
               (unsigned long)yield_progress_count,
               (unsigned long)yield_no_progress_count);
      yield_diag_ms = n;
      yield_progress_count = 0;
      yield_no_progress_count = 0;
    }
  }

done:
  voice_rs_cap_reset();
  voice_buf_free(pcm);
  voice_buf_free(mono_hw);
  voice_buf_free(hw_pcm);
  s_fe.cap_task = NULL;
  if (s_fe.fetch_task == NULL) {
    s_fe.state = FE_STATE_STOPPED;
  }
  vTaskDelete(NULL);
}

static void fe_fetch_task(void *arg) {
  (void)arg;
  while (s_fe.running) {
    if (!afe_bridge_is_ready()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    afe_fetch_result_t *res = afe_bridge_fetch(pdMS_TO_TICKS(VOICE_FE_FETCH_WAIT_MS));
    if (!s_fe.running) break;
    if (res == NULL) {
      s_fe.fetch_timeout_count++;
      s_fe.yield_no_progress_count++;
      s_fe.fetch_yield_count++;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    bool made_progress = false;
    uint32_t fetch_batch_count = 0;
    while (res != NULL && s_fe.running) {
      voice_fe_event_t ev = {0};
      ev.timestamp_ms = now_ms();
      ev.vad_speech = (res->vad_state == VAD_SPEECH);
      if (res->wakeup_state == WAKENET_DETECTED) {
        s_fe.wake_detect_count++;
        ev.type = VOICE_FE_EVENT_WAKE;
        fe_push_event(&ev);
        s_fe.wake_forward_count++;
        fe_set_last_fetch_ms(ev.timestamp_ms);
        made_progress = true;
      }

      if (res->data != NULL && res->data_size > 0) {
        ev.type = VOICE_FE_EVENT_AUDIO;
        size_t frames = (size_t)res->data_size / sizeof(int16_t);
        if (frames > VOICE_FE_MAX_PCM_FRAMES) frames = VOICE_FE_MAX_PCM_FRAMES;
        uint16_t slot = s_fe.next_slot;
        s_fe.next_slot = (uint16_t)((s_fe.next_slot + 1U) % VOICE_FE_AUDIO_SLOTS);
        if (s_fe.slots[slot].pcm != NULL && frames > 0) {
          portENTER_CRITICAL(&s_fe.slot_mux);
          memcpy(s_fe.slots[slot].pcm, res->data, frames * sizeof(int16_t));
          s_fe.slots[slot].frames = frames;
          s_fe.slots[slot].seq++;
          ev.slot_id = slot;
          ev.slot_seq = s_fe.slots[slot].seq;
          ev.frames = frames;
          portEXIT_CRITICAL(&s_fe.slot_mux);
          fe_push_event(&ev);
          s_fe.fetch_ok_count++;
          fe_set_last_fetch_ms(ev.timestamp_ms);
          made_progress = true;
        }
      }
      fetch_batch_count++;
      if (fetch_batch_count >= VOICE_FE_FETCH_MAX_BATCH) {
        break;
      }
      res = afe_bridge_fetch(0);
    }

    if (made_progress) {
      s_fe.yield_progress_count++;
    } else {
      s_fe.yield_no_progress_count++;
    }
    s_fe.fetch_yield_count++;
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  s_fe.fetch_task = NULL;
  if (s_fe.cap_task == NULL) {
    s_fe.state = FE_STATE_STOPPED;
  }
  vTaskDelete(NULL);
}

esp_err_t voice_frontend_start(const voice_frontend_config_t *cfg) {
  if (cfg == NULL || cfg->mic == NULL) return ESP_ERR_INVALID_ARG;
  if (s_fe.running || s_fe.stopping || s_fe.state == FE_STATE_STOPPING) return ESP_OK;
  memset(&s_fe, 0, sizeof(s_fe));
  s_fe.state = FE_STATE_STARTING;
  s_fe.slot_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
  s_fe.mic = cfg->mic;
  for (size_t i = 0; i < VOICE_FE_AUDIO_SLOTS; ++i) {
    s_fe.slots[i].pcm = (int16_t *)voice_buf_alloc(VOICE_FE_MAX_PCM_FRAMES * sizeof(int16_t));
    if (s_fe.slots[i].pcm == NULL) {
      fe_slots_free();
      return ESP_ERR_NO_MEM;
    }
  }
  s_fe.queue = xQueueCreate(VOICE_FE_QUEUE_LEN, sizeof(voice_fe_event_t));
  if (s_fe.queue == NULL) {
    fe_slots_free();
    return ESP_ERR_NO_MEM;
  }
  s_fe.running = true;
  if (xTaskCreate(fe_capture_task, "voice_fe_cap", VOICE_FE_CAP_TASK_STACK, NULL,
                  VOICE_FE_CAP_TASK_PRIO, &s_fe.cap_task) != pdPASS) {
    s_fe.running = false;
    vQueueDelete(s_fe.queue);
    s_fe.queue = NULL;
    fe_slots_free();
    fe_accum_free();
    return ESP_FAIL;
  }
  if (xTaskCreate(fe_fetch_task, "voice_fe_fch", VOICE_FE_FETCH_TASK_STACK, NULL,
                  VOICE_FE_FETCH_TASK_PRIO, &s_fe.fetch_task) != pdPASS) {
    s_fe.running = false;
    afe_bridge_reset();
    for (int i = 0; i < 100 && s_fe.cap_task != NULL; ++i) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_fe.queue != NULL) {
      vQueueDelete(s_fe.queue);
      s_fe.queue = NULL;
    }
    fe_slots_free();
    fe_accum_free();
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "voice_frontend started");
  return ESP_OK;
}

void voice_frontend_stop(void) {
  if (!s_fe.running && !s_fe.stopping && s_fe.cap_task == NULL && s_fe.fetch_task == NULL) {
    return;
  }
  s_fe.stopping = true;
  s_fe.state = FE_STATE_STOPPING;
  s_fe.running = false;
  afe_bridge_reset();
  for (int i = 0; i < 200 && (s_fe.cap_task != NULL || s_fe.fetch_task != NULL); ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s_fe.queue != NULL) {
    vQueueDelete(s_fe.queue);
    s_fe.queue = NULL;
  }
  fe_slots_free();
  fe_accum_free();
  s_fe.state = FE_STATE_STOPPED;
  s_fe.stopping = false;
}

bool voice_frontend_is_running(void) { return s_fe.running; }

bool voice_frontend_read_event(voice_fe_event_t *out, TickType_t ticks) {
  if (out == NULL || s_fe.queue == NULL) return false;
  return xQueueReceive(s_fe.queue, out, ticks) == pdTRUE;
}

bool voice_frontend_read_slot_pcm(uint16_t slot_id, uint32_t slot_seq, int16_t *dst,
                                  size_t cap_frames, size_t *out_frames) {
  if (dst == NULL || out_frames == NULL) return false;
  if (slot_id >= VOICE_FE_AUDIO_SLOTS || s_fe.slots[slot_id].pcm == NULL) return false;
  bool ok = false;
  portENTER_CRITICAL(&s_fe.slot_mux);
  if (s_fe.slots[slot_id].seq == slot_seq) {
    size_t n = s_fe.slots[slot_id].frames;
    if (n > cap_frames) n = cap_frames;
    memcpy(dst, s_fe.slots[slot_id].pcm, n * sizeof(int16_t));
    *out_frames = n;
    ok = (n > 0);
  }
  portEXIT_CRITICAL(&s_fe.slot_mux);
  return ok;
}

bool voice_frontend_get_health(voice_frontend_health_t *out) {
  if (out == NULL) return false;
  portENTER_CRITICAL(&s_fe.slot_mux);
  out->mic_read_ok = s_fe.mic_read_ok_count;
  out->mic_read_fail = s_fe.mic_read_fail_count;
  out->feed_ok = s_fe.feed_ok_count;
  out->fetch_ok = s_fe.fetch_ok_count;
  out->fetch_timeout_count = s_fe.fetch_timeout_count;
  out->fetch_yield_count = s_fe.fetch_yield_count;
  out->yield_progress_count = s_fe.yield_progress_count;
  out->yield_no_progress_count = s_fe.yield_no_progress_count;
  out->last_loop_hz = s_fe.last_loop_hz;
  out->wake_detect_count = s_fe.wake_detect_count;
  out->wake_forward_count = s_fe.wake_forward_count;
  out->queue_drop = s_fe.queue_drop_count;
  out->feed_pending_frames = s_fe.afe_mic_accum_len;
  out->last_read_ok_ms = s_fe.last_read_ok_ms;
  out->last_feed_ok_ms = s_fe.last_feed_ok_ms;
  out->last_fetch_ms = s_fe.last_fetch_ms;
  portEXIT_CRITICAL(&s_fe.slot_mux);
  return true;
}
