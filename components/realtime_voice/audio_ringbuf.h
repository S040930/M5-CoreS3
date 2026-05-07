#pragma once

#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int16_t *buf;
  const char *tag;
  size_t cap;
  volatile size_t w;
  volatile size_t r;
  uint64_t overflow_dropped_since_log;
  uint64_t overflow_last_log_ms;
  portMUX_TYPE spin;
} audio_ringbuf_t;

audio_ringbuf_t *audio_ringbuf_init(size_t capacity_samples);
audio_ringbuf_t *audio_ringbuf_init_named(size_t capacity_samples, const char *tag);
void audio_ringbuf_deinit(audio_ringbuf_t *rb);
void audio_ringbuf_reset(audio_ringbuf_t *rb);
size_t audio_ringbuf_avail(const audio_ringbuf_t *rb);
size_t audio_ringbuf_free(const audio_ringbuf_t *rb);
size_t audio_ringbuf_push(audio_ringbuf_t *rb, const int16_t *pcm, size_t samples);
size_t audio_ringbuf_push_quiet(audio_ringbuf_t *rb, const int16_t *pcm, size_t samples);
size_t audio_ringbuf_pop(audio_ringbuf_t *rb, int16_t *dst, size_t max_samples);

#ifdef __cplusplus
}
#endif
