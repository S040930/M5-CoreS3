#include "audio_eq.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define AUDIO_EQ_BANDS 15
#define EQ_STAGES 5
#define PI_F      3.14159265358979323846f
#define SOFT_LIMIT_THRESHOLD 28000.0f
#define SOFT_LIMIT_MAX       32767.0f

// Harman-like conservative default profile for small speakers (15-band UI layout).
static const float k_default_harman_like[AUDIO_EQ_BANDS] = {
    -6.0f, -5.0f, -3.5f, -2.0f, -0.5f,
     0.0f,  0.0f, -0.5f, -1.0f, -1.5f,
    -2.0f, -2.5f, -3.0f, -2.0f, -1.0f,
};

typedef struct {
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
} biquad_coeffs_t;

typedef struct {
  float x1;
  float x2;
  float y1;
  float y2;
} biquad_state_t;

static biquad_coeffs_t s_coeffs[EQ_STAGES];
static biquad_state_t s_state_l[EQ_STAGES];
static biquad_state_t s_state_r[EQ_STAGES];
static uint32_t s_sample_rate_hz = 44100;
static bool s_ready = false;

static float avg_range(const float gains[AUDIO_EQ_BANDS], int start, int end) {
  float sum = 0.0f;
  int n = 0;
  for (int i = start; i <= end; ++i) {
    sum += gains[i];
    n++;
  }
  return (n > 0) ? (sum / (float)n) : 0.0f;
}

static void coeffs_peaking(biquad_coeffs_t *c, float fs, float f0, float q,
                           float gain_db) {
  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * PI_F * f0 / fs;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);

  float b0 = 1.0f + alpha * A;
  float b1 = -2.0f * cw;
  float b2 = 1.0f - alpha * A;
  float a0 = 1.0f + alpha / A;
  float a1 = -2.0f * cw;
  float a2 = 1.0f - alpha / A;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

static void coeffs_low_shelf(biquad_coeffs_t *c, float fs, float f0, float q,
                             float gain_db) {
  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * PI_F * f0 / fs;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);
  float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

  float b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha);
  float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
  float b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha);
  float a0 = (A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha;
  float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
  float a2 = (A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

static void coeffs_high_shelf(biquad_coeffs_t *c, float fs, float f0, float q,
                              float gain_db) {
  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * PI_F * f0 / fs;
  float cw = cosf(w0);
  float sw = sinf(w0);
  float alpha = sw / (2.0f * q);
  float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

  float b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + two_sqrtA_alpha);
  float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
  float b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - two_sqrtA_alpha);
  float a0 = (A + 1.0f) - (A - 1.0f) * cw + two_sqrtA_alpha;
  float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
  float a2 = (A + 1.0f) - (A - 1.0f) * cw - two_sqrtA_alpha;

  c->b0 = b0 / a0;
  c->b1 = b1 / a0;
  c->b2 = b2 / a0;
  c->a1 = a1 / a0;
  c->a2 = a2 / a0;
}

static void update_coeffs_from_15band(const float gains[AUDIO_EQ_BANDS]) {
  float fs = (float)s_sample_rate_hz;

  float g_low = avg_range(gains, 0, 4);
  float g_lomid = avg_range(gains, 5, 7);
  float g_mid = avg_range(gains, 8, 10);
  float g_presence = avg_range(gains, 11, 13);
  float g_air = gains[14];

  coeffs_low_shelf(&s_coeffs[0], fs, 120.0f, 0.707f, g_low);
  coeffs_peaking(&s_coeffs[1], fs, 400.0f, 0.90f, g_lomid);
  coeffs_peaking(&s_coeffs[2], fs, 1500.0f, 1.00f, g_mid);
  coeffs_peaking(&s_coeffs[3], fs, 4500.0f, 1.10f, g_presence);
  coeffs_high_shelf(&s_coeffs[4], fs, 10000.0f, 0.707f, g_air);
}

static inline float biquad_run(const biquad_coeffs_t *c, biquad_state_t *s,
                               float x) {
  float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
  s->x2 = s->x1;
  s->x1 = x;
  s->y2 = s->y1;
  s->y1 = y;
  return y;
}

static inline float soft_limit_sample(float x) {
  float sign = (x < 0.0f) ? -1.0f : 1.0f;
  float ax = fabsf(x);
  if (ax <= SOFT_LIMIT_THRESHOLD) {
    return x;
  }

  float over = ax - SOFT_LIMIT_THRESHOLD;
  float span = SOFT_LIMIT_MAX - SOFT_LIMIT_THRESHOLD;
  float normalized = over / span;
  float compressed = SOFT_LIMIT_THRESHOLD + (span * (normalized / (1.0f + normalized)));
  return sign * compressed;
}

esp_err_t audio_eq_init(uint32_t sample_rate_hz) {
  if (sample_rate_hz > 0) {
    s_sample_rate_hz = sample_rate_hz;
  }

  memset(s_state_l, 0, sizeof(s_state_l));
  memset(s_state_r, 0, sizeof(s_state_r));

  update_coeffs_from_15band(k_default_harman_like);

  s_ready = true;
  return ESP_OK;
}

void audio_eq_reload_from_settings(void) {
  update_coeffs_from_15band(k_default_harman_like);
}

void audio_eq_process(int16_t *samples, size_t sample_count) {
  if (!s_ready || samples == NULL) {
    return;
  }

  for (size_t i = 0; i + 1 < sample_count; i += 2) {
    float l = (float)samples[i];
    float r = (float)samples[i + 1];

    for (int st = 0; st < EQ_STAGES; ++st) {
      l = biquad_run(&s_coeffs[st], &s_state_l[st], l);
      r = biquad_run(&s_coeffs[st], &s_state_r[st], r);
    }

    l = soft_limit_sample(l);
    r = soft_limit_sample(r);

    if (l > 32767.0f) {
      l = 32767.0f;
    } else if (l < -32768.0f) {
      l = -32768.0f;
    }
    if (r > 32767.0f) {
      r = 32767.0f;
    } else if (r < -32768.0f) {
      r = -32768.0f;
    }

    samples[i] = (int16_t)l;
    samples[i + 1] = (int16_t)r;
  }
}
