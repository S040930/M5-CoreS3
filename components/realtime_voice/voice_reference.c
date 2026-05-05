#include "voice_reference.h"

#include "afe_bridge.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "voice_dsp.h"

static const char *TAG = "voice_reference";

#define VOICE_REF_RING_MS 1500

static Resample *s_ref_rs;
static double s_ref_ratio = 1.0;
static int16_t *s_ref_ring;
static size_t s_ref_cap;
static volatile size_t s_ref_w;
static volatile size_t s_ref_r;
static portMUX_TYPE s_ref_ring_spin = portMUX_INITIALIZER_UNLOCKED;

static Resample *s_airplay_ref_rs;
static double s_airplay_ref_ratio = 1.0;
static uint32_t s_airplay_ref_src_hz = 0;
static int16_t *s_airplay_ref_mono_scratch;
static size_t s_airplay_ref_mono_scratch_frames;
static int16_t *s_airplay_ref_rs_out_buf;
static size_t s_airplay_ref_rs_out_cap;

void voice_reference_playout_rs_destroy(void) {
  if (s_ref_rs != NULL) {
    resampleFree(s_ref_rs);
    s_ref_rs = NULL;
  }
  s_ref_ratio = 1.0;
}

bool voice_reference_playout_rs_ensure(void) {
  uint32_t model_rate = (uint32_t)CONFIG_VOICE_OUTPUT_SAMPLE_RATE;
  uint32_t afe_rate = 16000;
  if (model_rate == afe_rate) {
    voice_reference_playout_rs_destroy();
    return true;
  }
  if (s_ref_rs != NULL) {
    return true;
  }
  return voice_dsp_resampler_create_fixed(&s_ref_rs, (double)model_rate, (double)afe_rate,
                                          &s_ref_ratio);
}

Resample *voice_reference_playout_rs(void) { return s_ref_rs; }

double voice_reference_playout_ratio(void) { return s_ref_ratio; }

void voice_reference_airplay_rs_destroy(void) {
  if (s_airplay_ref_rs != NULL) {
    resampleFree(s_airplay_ref_rs);
    s_airplay_ref_rs = NULL;
  }
  s_airplay_ref_ratio = 1.0;
  s_airplay_ref_src_hz = 0;
}

bool voice_reference_airplay_rs_ensure(uint32_t src_hz) {
  const uint32_t afe_hz = 16000;
  if (src_hz == afe_hz) {
    voice_reference_airplay_rs_destroy();
    return true;
  }
  if (src_hz == s_airplay_ref_src_hz && s_airplay_ref_rs != NULL) {
    return true;
  }
  voice_reference_airplay_rs_destroy();
  if (!voice_dsp_resampler_create_fixed(&s_airplay_ref_rs, (double)src_hz, (double)afe_hz,
                                        &s_airplay_ref_ratio)) {
    return false;
  }
  s_airplay_ref_src_hz = src_hz;
  return true;
}

void voice_reference_airplay_scratch_free(void) {
  voice_buf_free(s_airplay_ref_mono_scratch);
  s_airplay_ref_mono_scratch = NULL;
  s_airplay_ref_mono_scratch_frames = 0;
  voice_buf_free(s_airplay_ref_rs_out_buf);
  s_airplay_ref_rs_out_buf = NULL;
  s_airplay_ref_rs_out_cap = 0;
}

bool voice_reference_ring_init(void) {
  if (s_ref_ring != NULL) {
    return true;
  }
  size_t samples = (size_t)16000 * VOICE_REF_RING_MS / 1000;
  if (samples < 512) {
    samples = 512;
  }
  s_ref_ring = (int16_t *)voice_buf_alloc(samples * sizeof(int16_t));
  if (s_ref_ring == NULL) {
    ESP_LOGE(TAG, "ref_ring alloc failed");
    return false;
  }
  s_ref_cap = samples;
  s_ref_w = 0;
  s_ref_r = 0;
  return true;
}

void voice_reference_ring_deinit(void) {
  voice_buf_free(s_ref_ring);
  s_ref_ring = NULL;
  s_ref_cap = 0;
  s_ref_w = 0;
  s_ref_r = 0;
}

bool voice_reference_ring_is_ready(void) { return s_ref_ring != NULL; }

static inline size_t ref_ring_avail(void) {
  if (s_ref_w >= s_ref_r) {
    return s_ref_w - s_ref_r;
  }
  return s_ref_cap - s_ref_r + s_ref_w;
}

static inline size_t ref_ring_free_slots(void) {
  size_t a = ref_ring_avail();
  return s_ref_cap > a + 1 ? s_ref_cap - a - 1 : 0;
}

void voice_reference_ring_push(const int16_t *pcm, size_t samples) {
  if (s_ref_ring == NULL || pcm == NULL || samples == 0) {
    return;
  }
  portENTER_CRITICAL(&s_ref_ring_spin);
  size_t f = ref_ring_free_slots();
  size_t n = samples < f ? samples : f;
  for (size_t i = 0; i < n; i++) {
    s_ref_ring[s_ref_w] = pcm[i];
    s_ref_w = (s_ref_w + 1) < s_ref_cap ? s_ref_w + 1 : 0;
  }
  portEXIT_CRITICAL(&s_ref_ring_spin);
}

size_t voice_reference_ring_pop(int16_t *dst, size_t max_samples) {
  if (s_ref_ring == NULL || dst == NULL || max_samples == 0) {
    return 0;
  }
  portENTER_CRITICAL(&s_ref_ring_spin);
  size_t a = ref_ring_avail();
  size_t n = max_samples < a ? max_samples : a;
  for (size_t i = 0; i < n; i++) {
    dst[i] = s_ref_ring[s_ref_r];
    s_ref_r = (s_ref_r + 1) < s_ref_cap ? s_ref_r + 1 : 0;
  }
  portEXIT_CRITICAL(&s_ref_ring_spin);
  return n;
}

void voice_reference_airplay_tap(const int16_t *stereo, size_t frames, uint32_t sample_rate_hz,
                                 void *ctx) {
  (void)ctx;
  if (!voice_reference_ring_is_ready() || stereo == NULL || frames == 0) {
    return;
  }
  if (!afe_bridge_is_ready()) {
    return;
  }

  if (s_airplay_ref_mono_scratch_frames < frames) {
    voice_buf_free(s_airplay_ref_mono_scratch);
    s_airplay_ref_mono_scratch = (int16_t *)voice_buf_alloc(frames * sizeof(int16_t));
    s_airplay_ref_mono_scratch_frames = s_airplay_ref_mono_scratch != NULL ? frames : 0;
  }
  if (s_airplay_ref_mono_scratch == NULL) {
    return;
  }

  for (size_t i = 0; i < frames; i++) {
    int32_t L = stereo[i * 2];
    int32_t R = stereo[i * 2 + 1];
    s_airplay_ref_mono_scratch[i] = (int16_t)((L + R) >> 1);
  }

  if (sample_rate_hz == 16000) {
    voice_reference_ring_push(s_airplay_ref_mono_scratch, frames);
    return;
  }

  if (!voice_reference_airplay_rs_ensure(sample_rate_hz)) {
    return;
  }

  size_t out_cap = voice_rs_output_cap(frames, s_airplay_ref_ratio);
  if (s_airplay_ref_rs_out_cap < out_cap) {
    voice_buf_free(s_airplay_ref_rs_out_buf);
    s_airplay_ref_rs_out_buf = (int16_t *)voice_buf_alloc(out_cap * sizeof(int16_t));
    s_airplay_ref_rs_out_cap = s_airplay_ref_rs_out_buf != NULL ? out_cap : 0;
  }
  if (s_airplay_ref_rs_out_buf == NULL) {
    return;
  }

  size_t out_frames =
      voice_rs_process_mono(s_airplay_ref_rs, s_airplay_ref_ratio, s_airplay_ref_mono_scratch,
                            frames, s_airplay_ref_rs_out_buf, s_airplay_ref_rs_out_cap);
  if (out_frames > 0) {
    voice_reference_ring_push(s_airplay_ref_rs_out_buf, out_frames);
  }
}
