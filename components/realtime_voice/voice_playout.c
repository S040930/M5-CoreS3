#include "voice_playout.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>

#define VOICE_PLAYOUT_LAST_STEREO_FRAMES 512U
#define VOICE_PLAYOUT_HW_CHANNELS        2U

static const char *TAG = "voice_playout";

static int16_t       *s_buf;
static size_t         s_cap;
static volatile size_t s_w;
static volatile size_t s_r;
static int16_t       *s_last_stereo;
static size_t         s_last_stereo_frames;
static uint64_t       s_last_write_ms;

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
  if (s_buf != NULL) {
    return true;
  }
  size_t samples = (size_t)sample_rate * VOICE_PLAYOUT_RING_MS / 1000U;
  if (samples < 1024U) {
    samples = 1024U;
  }
  s_buf = (int16_t *)playout_alloc(samples * sizeof(int16_t));
  if (s_buf == NULL) {
    ESP_LOGE(TAG, "playout_ring: alloc %u samples failed", (unsigned)samples);
    return false;
  }
  s_cap = samples;
  s_w = 0;
  s_r = 0;
  return true;
}

void voice_playout_deinit(void) {
  playout_free(s_last_stereo);
  s_last_stereo = NULL;
  s_last_stereo_frames = 0;
  playout_free(s_buf);
  s_buf = NULL;
  s_cap = 0;
  s_w = 0;
  s_r = 0;
  s_last_write_ms = 0;
}

void voice_playout_reset(void) {
  s_w = 0;
  s_r = 0;
  s_last_write_ms = 0;
}

size_t voice_playout_avail(void) {
  if (s_w >= s_r) {
    return s_w - s_r;
  }
  return s_cap - s_r + s_w;
}

size_t voice_playout_free(void) {
  size_t a = voice_playout_avail();
  return s_cap > a + 1U ? s_cap - a - 1U : 0;
}

size_t voice_playout_push(const int16_t *pcm, size_t samples) {
  if (s_buf == NULL || pcm == NULL || samples == 0) {
    return 0;
  }
  size_t f = voice_playout_free();
  size_t n = samples < f ? samples : f;
  for (size_t i = 0; i < n; i++) {
    s_buf[s_w] = pcm[i];
    s_w = (s_w + 1U) < s_cap ? s_w + 1U : 0;
  }
  if (n < samples) {
    ESP_LOGW(TAG, "playout_ring: overflow dropped %u samples",
             (unsigned)(samples - n));
  }
  return n;
}

size_t voice_playout_pop(int16_t *dst, size_t max_samples) {
  if (s_buf == NULL || dst == NULL || max_samples == 0) {
    return 0;
  }
  size_t a = voice_playout_avail();
  size_t n = max_samples < a ? max_samples : a;
  for (size_t i = 0; i < n; i++) {
    dst[i] = s_buf[s_r];
    s_r = (s_r + 1U) < s_cap ? s_r + 1U : 0;
  }
  return n;
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
