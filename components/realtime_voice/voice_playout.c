#include "voice_playout.h"

#include "audio_ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

#define VOICE_PLAYOUT_LAST_STEREO_FRAMES 512U
#define VOICE_PLAYOUT_HW_CHANNELS        2U

static const char *TAG = "voice_playout";

static audio_ringbuf_t *s_ring;
static int16_t       *s_last_stereo;
static size_t         s_last_stereo_frames;
static uint64_t       s_last_write_ms;
static voice_pcm_format_t s_stream_format;
static portMUX_TYPE s_stream_format_mux = portMUX_INITIALIZER_UNLOCKED;

static void *playout_alloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  return p;
}

static void playout_free(void *p) {
  if (p != NULL) {
    heap_caps_free(p);
  }
}

bool voice_playout_init(uint32_t sample_rate) {
  if (s_ring != NULL) {
    return true;
  }
  size_t samples = (size_t)sample_rate * VOICE_PLAYOUT_RING_MS / 1000U;
  if (samples < 1024U) {
    samples = 1024U;
  }
  s_ring = audio_ringbuf_init_named(samples, "playout_ring");
  if (s_ring == NULL) {
    ESP_LOGE(TAG, "playout_ring: alloc %u samples failed", (unsigned)samples);
    return false;
  }
  s_stream_format = voice_pcm_cloud_default_format();
  if (sample_rate > 0) {
    s_stream_format.sample_rate_hz = sample_rate;
  }
  return true;
}

void voice_playout_deinit(void) {
  playout_free(s_last_stereo);
  s_last_stereo = NULL;
  s_last_stereo_frames = 0;
  audio_ringbuf_deinit(s_ring);
  s_ring = NULL;
  s_last_write_ms = 0;
  portENTER_CRITICAL(&s_stream_format_mux);
  s_stream_format = voice_pcm_cloud_default_format();
  portEXIT_CRITICAL(&s_stream_format_mux);
}

void voice_playout_reset(void) {
  audio_ringbuf_reset(s_ring);
  s_last_write_ms = 0;
  portENTER_CRITICAL(&s_stream_format_mux);
  s_stream_format = voice_pcm_cloud_default_format();
  portEXIT_CRITICAL(&s_stream_format_mux);
}

void voice_playout_set_stream_format(const voice_pcm_format_t *format) {
  if (voice_pcm_format_is_valid(format)) {
    portENTER_CRITICAL(&s_stream_format_mux);
    s_stream_format = *format;
    portEXIT_CRITICAL(&s_stream_format_mux);
  }
}

voice_pcm_format_t voice_playout_stream_format(void) {
  voice_pcm_format_t fmt;
  portENTER_CRITICAL(&s_stream_format_mux);
  fmt = s_stream_format;
  portEXIT_CRITICAL(&s_stream_format_mux);
  if (!voice_pcm_format_is_valid(&fmt)) {
    return voice_pcm_cloud_default_format();
  }
  return fmt;
}

size_t voice_playout_avail(void) {
  return audio_ringbuf_avail(s_ring);
}

size_t voice_playout_free(void) {
  return audio_ringbuf_free(s_ring);
}

size_t voice_playout_push(const int16_t *pcm, size_t samples) {
  return audio_ringbuf_push(s_ring, pcm, samples);
}

size_t voice_playout_pop(int16_t *dst, size_t max_samples) {
  return audio_ringbuf_pop(s_ring, dst, max_samples);
}

uint64_t voice_playout_last_write_ms(void) { return s_last_write_ms; }

void voice_playout_set_last_write_ms(uint64_t ms) { s_last_write_ms = ms; }

void voice_playout_save_last_stereo(const int16_t *src, size_t frames) {
  if (src == NULL || frames == 0) {
    return;
  }
  if (s_last_stereo == NULL) {
    s_last_stereo = (int16_t *)playout_alloc(VOICE_PLAYOUT_LAST_STEREO_FRAMES *
                                             VOICE_PLAYOUT_HW_CHANNELS *
                                             sizeof(int16_t));
  }
  if (s_last_stereo == NULL) {
    return;
  }
  size_t keep = frames < VOICE_PLAYOUT_LAST_STEREO_FRAMES
                    ? frames
                    : VOICE_PLAYOUT_LAST_STEREO_FRAMES;
  memcpy(s_last_stereo, src,
         keep * VOICE_PLAYOUT_HW_CHANNELS * sizeof(int16_t));
  s_last_stereo_frames = keep;
}

const int16_t *voice_playout_last_stereo(size_t *frames) {
  if (frames != NULL) {
    *frames = s_last_stereo_frames;
  }
  return s_last_stereo;
}
