#include "voice_dsp.h"
#include "voice_common.h"
#include "voice_playout.h"

#include "audio/audio_output_common.h"
#include "audio/audio_resample.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "voice_dsp";

static Resample *s_cap_rs;
static Resample *s_play_rs;
static float *s_rs_f_in;
static float *s_rs_f_out;
static size_t s_rs_f_in_cap;
static size_t s_rs_f_out_cap;
static SemaphoreHandle_t s_voice_rs_float_mtx;

static void voice_rs_float_mtx_ensure(void) {
  static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
  if (s_voice_rs_float_mtx != NULL) {
    return;
  }
  portENTER_CRITICAL(&s_init_mux);
  if (s_voice_rs_float_mtx == NULL) {
    s_voice_rs_float_mtx = xSemaphoreCreateMutex();
  }
  portEXIT_CRITICAL(&s_init_mux);
}

static double s_cap_ratio = 1.0;
static double s_play_ratio = 1.0;

static void voice_rs_free_float_bufs_locked(void) {
  voice_buf_free(s_rs_f_in);
  s_rs_f_in = NULL;
  voice_buf_free(s_rs_f_out);
  s_rs_f_out = NULL;
  s_rs_f_in_cap = 0;
  s_rs_f_out_cap = 0;
}

void voice_rs_free_float_bufs(void) {
  voice_rs_float_mtx_ensure();
  if (s_voice_rs_float_mtx != NULL) {
    (void)xSemaphoreTake(s_voice_rs_float_mtx, portMAX_DELAY);
  }
  voice_rs_free_float_bufs_locked();
  if (s_voice_rs_float_mtx != NULL) {
    xSemaphoreGive(s_voice_rs_float_mtx);
  }
}

static bool voice_rs_ensure_float_bufs(size_t in_samples, size_t out_samples) {
  if (in_samples > (SIZE_MAX / sizeof(float)) || out_samples > (SIZE_MAX / sizeof(float))) {
    ESP_LOGE(TAG, "resampler float buffer size overflow: in=%lu out=%lu",
             (unsigned long)in_samples, (unsigned long)out_samples);
    return false;
  }

  if (in_samples > s_rs_f_in_cap) {
    float *new_in = (float *)voice_buf_alloc(in_samples * sizeof(float));
    if (new_in == NULL) {
      ESP_LOGW(TAG, "resampler input float alloc failed: samples=%lu old_cap=%lu",
               (unsigned long)in_samples, (unsigned long)s_rs_f_in_cap);
      return false;
    }
    voice_buf_free(s_rs_f_in);
    s_rs_f_in = new_in;
    s_rs_f_in_cap = in_samples;
  }
  if (out_samples > s_rs_f_out_cap) {
    float *new_out = (float *)voice_buf_alloc(out_samples * sizeof(float));
    if (new_out == NULL) {
      ESP_LOGW(TAG, "resampler output float alloc failed: samples=%lu old_cap=%lu",
               (unsigned long)out_samples, (unsigned long)s_rs_f_out_cap);
      return false;
    }
    voice_buf_free(s_rs_f_out);
    s_rs_f_out = new_out;
    s_rs_f_out_cap = out_samples;
  }
  return s_rs_f_in != NULL && s_rs_f_out != NULL;
}

bool voice_dsp_resampler_create_fixed(Resample **out_rs, double src_hz, double dst_hz,
                                      double *out_ratio) {
  *out_rs = NULL;
  *out_ratio = 1.0;
  if (fabs(src_hz - dst_hz) < 1.0) {
    return true;
  }
  unsigned long g = (unsigned long)src_hz;
  unsigned long b = (unsigned long)dst_hz;
  while (b != 0) {
    unsigned long t = b;
    b = g % b;
    g = t;
  }
  int exact_filters = (int)(dst_hz / (double)g);
  int max_filters = exact_filters;
  if (max_filters > 1024) {
    max_filters = 1024;
  }
  int flags = SUBSAMPLE_INTERPOLATE | INCLUDE_LOWPASS | BLACKMAN_HARRIS;
  Resample *rs = resampleFixedRatioInit(1, VOICE_DSP_RS_TAPS, max_filters, src_hz, dst_hz, 0, flags);
  if (rs == NULL) {
    return false;
  }
  *out_rs = rs;
  *out_ratio = dst_hz / src_hz;
  return true;
}

void voice_rs_destroy_cap(void) {
  if (s_cap_rs != NULL) {
    resampleFree(s_cap_rs);
    s_cap_rs = NULL;
  }
  s_cap_ratio = 1.0;
}

void voice_rs_destroy_play(void) {
  if (s_play_rs != NULL) {
    resampleFree(s_play_rs);
    s_play_rs = NULL;
  }
  s_play_ratio = 1.0;
}

bool voice_rs_cap_ensure(void) {
  uint32_t hw = voice_hw_codec_rate_hz();
  uint32_t api = (uint32_t)CONFIG_VOICE_INPUT_SAMPLE_RATE;
  if (hw == api) {
    voice_rs_destroy_cap();
    return true;
  }
  if (s_cap_rs != NULL) {
    return true;
  }
  return voice_dsp_resampler_create_fixed(&s_cap_rs, (double)hw, (double)api, &s_cap_ratio);
}

bool voice_rs_play_ensure(void) {
  uint32_t hw = voice_hw_codec_rate_hz();
  uint32_t api = voice_playout_stream_format().sample_rate_hz;
  if (api == 0) {
    api = (uint32_t)CONFIG_VOICE_OUTPUT_SAMPLE_RATE;
  }
  if (hw == api) {
    voice_rs_destroy_play();
    return true;
  }
  if (s_play_rs != NULL) {
    return true;
  }
  return voice_dsp_resampler_create_fixed(&s_play_rs, (double)api, (double)hw, &s_play_ratio);
}

void voice_play_rs_reset(void) {
  if (s_play_rs != NULL) {
    resampleReset(s_play_rs);
  }
}

void voice_rs_cap_reset(void) {
  if (s_cap_rs != NULL) {
    resampleReset(s_cap_rs);
  }
}

Resample *voice_rs_cap_rs(void) { return s_cap_rs; }

double voice_rs_cap_ratio(void) { return s_cap_ratio; }

Resample *voice_rs_play_rs(void) { return s_play_rs; }

double voice_rs_play_ratio(void) { return s_play_ratio; }

size_t voice_rs_output_cap(size_t in_frames, double ratio) {
  double scaled = ceil((double)in_frames * ratio);
  size_t cap = (size_t)scaled + (size_t)(VOICE_DSP_RS_TAPS * 2U) + 32U;
  if (cap < in_frames) {
    cap = in_frames;
  }
  return cap;
}

size_t voice_rs_process_mono(Resample *rs, double ratio, const int16_t *in, size_t in_frames,
                             int16_t *out, size_t out_cap) {
  if (rs == NULL || in_frames == 0) {
    return 0;
  }
  voice_rs_float_mtx_ensure();
  if (s_voice_rs_float_mtx == NULL) {
    return 0;
  }
  xSemaphoreTake(s_voice_rs_float_mtx, portMAX_DELAY);
  size_t in_samp = in_frames;
  size_t out_samp = out_cap;
  if (!voice_rs_ensure_float_bufs(in_samp, out_samp)) {
    xSemaphoreGive(s_voice_rs_float_mtx);
    return 0;
  }
  for (size_t i = 0; i < in_samp; ++i) {
    s_rs_f_in[i] = (float)in[i] * (1.0f / 32768.0f);
  }
  ResampleResult rr =
      resampleProcessInterleaved(rs, s_rs_f_in, (int)in_frames, s_rs_f_out, (int)out_cap, ratio);
  size_t gen = (size_t)rr.output_generated;
  if (gen > out_cap) {
    ESP_LOGE(TAG, "resampler overflow: in=%lu out=%lu cap=%lu ratio=%.4f",
             (unsigned long)in_frames, (unsigned long)gen, (unsigned long)out_cap, ratio);
    gen = out_cap;
  }
  for (size_t i = 0; i < gen; ++i) {
    float s = s_rs_f_out[i] * 32768.0f;
    if (s > 32767.0f) {
      s = 32767.0f;
    } else if (s < -32768.0f) {
      s = -32768.0f;
    }
    out[i] = (int16_t)s;
  }
  xSemaphoreGive(s_voice_rs_float_mtx);
  return gen;
}
