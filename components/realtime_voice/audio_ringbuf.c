#include "audio_ringbuf.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static const char *TAG = "audio_ringbuf";
static const uint64_t AUDIO_RINGBUF_OVERFLOW_LOG_INTERVAL_MS = 1000ULL;

audio_ringbuf_t *audio_ringbuf_init(size_t capacity_samples) {
  return audio_ringbuf_init_named(capacity_samples, "ring");
}

audio_ringbuf_t *audio_ringbuf_init_named(size_t capacity_samples, const char *tag) {
  audio_ringbuf_t *rb = (audio_ringbuf_t *)heap_caps_malloc(sizeof(audio_ringbuf_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (rb == NULL) {
    rb = (audio_ringbuf_t *)heap_caps_malloc(sizeof(audio_ringbuf_t), MALLOC_CAP_8BIT);
  }
  if (rb == NULL) {
    ESP_LOGE(TAG, "alloc struct failed");
    return NULL;
  }
  memset(rb, 0, sizeof(*rb));

  rb->buf = (int16_t *)heap_caps_malloc(capacity_samples * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (rb->buf == NULL) {
    rb->buf = (int16_t *)heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
  }
  if (rb->buf == NULL) {
    ESP_LOGE(TAG, "alloc buffer %u samples failed", (unsigned)capacity_samples);
    heap_caps_free(rb);
    return NULL;
  }

  rb->cap = capacity_samples;
  rb->tag = (tag != NULL && tag[0] != '\0') ? tag : "ring";
  rb->w = 0;
  rb->r = 0;
  rb->spin = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
  return rb;
}

void audio_ringbuf_deinit(audio_ringbuf_t *rb) {
  if (rb == NULL) {
    return;
  }
  if (rb->buf != NULL) {
    heap_caps_free(rb->buf);
    rb->buf = NULL;
  }
  heap_caps_free(rb);
}

void audio_ringbuf_reset(audio_ringbuf_t *rb) {
  if (rb == NULL) {
    return;
  }
  portENTER_CRITICAL(&rb->spin);
  rb->w = 0;
  rb->r = 0;
  portEXIT_CRITICAL(&rb->spin);
}

size_t audio_ringbuf_avail(const audio_ringbuf_t *rb) {
  if (rb == NULL) {
    return 0;
  }
  size_t w = rb->w;
  size_t r = rb->r;
  if (w >= r) {
    return w - r;
  }
  return rb->cap - r + w;
}

size_t audio_ringbuf_free(const audio_ringbuf_t *rb) {
  if (rb == NULL) {
    return 0;
  }
  size_t a = audio_ringbuf_avail(rb);
  return rb->cap > a + 1U ? rb->cap - a - 1U : 0;
}

static void audio_ringbuf_log_overflow(audio_ringbuf_t *rb, size_t dropped_now) {
  if (rb == NULL || dropped_now == 0) {
    return;
  }
  uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
  rb->overflow_dropped_since_log += dropped_now;
  if ((now_ms - rb->overflow_last_log_ms) < AUDIO_RINGBUF_OVERFLOW_LOG_INTERVAL_MS) {
    return;
  }
  size_t avail = audio_ringbuf_avail(rb);
  size_t free = audio_ringbuf_free(rb);
  ESP_LOGW(TAG, "%s overflow dropped %llu samples in %llums (avail=%u free=%u cap=%u)",
           rb->tag, (unsigned long long)rb->overflow_dropped_since_log,
           (unsigned long long)(rb->overflow_last_log_ms == 0 ? 0 : (now_ms - rb->overflow_last_log_ms)),
           (unsigned)avail, (unsigned)free, (unsigned)rb->cap);
  rb->overflow_last_log_ms = now_ms;
  rb->overflow_dropped_since_log = 0;
}

static size_t audio_ringbuf_push_impl(audio_ringbuf_t *rb, const int16_t *pcm, size_t samples,
                                      bool log_overflow) {
  if (rb == NULL || rb->buf == NULL || pcm == NULL || samples == 0) {
    return 0;
  }
  portENTER_CRITICAL(&rb->spin);
  size_t f = audio_ringbuf_free(rb);
  size_t n = samples < f ? samples : f;
  size_t first = rb->cap - rb->w;
  if (first > n) first = n;
  memcpy(rb->buf + rb->w, pcm, first * sizeof(int16_t));
  size_t second = n - first;
  if (second > 0) {
    memcpy(rb->buf, pcm + first, second * sizeof(int16_t));
  }
  rb->w = (rb->w + n) % rb->cap;
  portEXIT_CRITICAL(&rb->spin);
  if (log_overflow && n < samples) {
    audio_ringbuf_log_overflow(rb, samples - n);
  }
  return n;
}

size_t audio_ringbuf_push(audio_ringbuf_t *rb, const int16_t *pcm, size_t samples) {
  return audio_ringbuf_push_impl(rb, pcm, samples, true);
}

size_t audio_ringbuf_push_quiet(audio_ringbuf_t *rb, const int16_t *pcm, size_t samples) {
  return audio_ringbuf_push_impl(rb, pcm, samples, false);
}

size_t audio_ringbuf_pop(audio_ringbuf_t *rb, int16_t *dst, size_t max_samples) {
  if (rb == NULL || rb->buf == NULL || dst == NULL || max_samples == 0) {
    return 0;
  }
  portENTER_CRITICAL(&rb->spin);
  size_t a = audio_ringbuf_avail(rb);
  size_t n = max_samples < a ? max_samples : a;
  size_t first = rb->cap - rb->r;
  if (first > n) first = n;
  memcpy(dst, rb->buf + rb->r, first * sizeof(int16_t));
  size_t second = n - first;
  if (second > 0) {
    memcpy(dst + first, rb->buf, second * sizeof(int16_t));
  }
  rb->r = (rb->r + n) % rb->cap;
  portEXIT_CRITICAL(&rb->spin);
  return n;
}
