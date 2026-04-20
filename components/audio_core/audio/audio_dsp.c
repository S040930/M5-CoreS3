#include "audio_dsp.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define DC_BLOCK_R                 0.995f
#define ENV_ATTACK                 0.18f
#define ENV_RELEASE                0.010f
#define GATE_ATTACK                0.20f
#define GATE_RELEASE               0.010f
#define GATE_MIN_GAIN              0.28f
#define NOISE_FLOOR_RISE           0.00004f
#define NOISE_FLOOR_FALL           0.010f
#define COMP_ATTACK                0.14f
#define COMP_RELEASE               0.004f
#define COMP_THRESHOLD             0.58f
#define COMP_RATIO                 2.4f
#define LIMIT_THRESHOLD            0.92f
#define LOW_CROSSOVER_ALPHA        0.080f
#define MID_ENVELOPE_ALPHA         0.045f
#define BAND_ENVELOPE_ALPHA        0.030f
#define RMS_ENVELOPE_ALPHA         0.015f
#define STATS_SMOOTH_ALPHA         0.080f
#define MIN_DBFS                   (-9600)

typedef struct {
  float dc_x1;
  float dc_y1;
  float env;
  float gate_gain;
  float comp_gain;
} dsp_channel_state_t;

static dsp_channel_state_t s_left = {0};
static dsp_channel_state_t s_right = {0};
static uint32_t s_sample_rate_hz = 44100;
static bool s_ready = false;
static audio_dsp_stats_t s_stats = {0};
static float s_noise_floor = 0.0025f;
static float s_peak_norm = 0.0f;
static float s_rms_norm = 0.0f;
static float s_low_env = 0.0f;
static float s_mid_env = 0.0f;
static float s_high_env = 0.0f;
static float s_low_lpf_l = 0.0f;
static float s_low_lpf_r = 0.0f;
static audio_dsp_mode_t s_mode = AUDIO_DSP_MODE_LIMITER_ONLY;

static int16_t dbfs_x100_from_norm(float value) {
  if (value <= 0.000001f) {
    return MIN_DBFS;
  }

  float db = 20.0f * log10f(value);
  if (db < -96.0f) {
    db = -96.0f;
  }
  if (db > 0.0f) {
    db = 0.0f;
  }
  return (int16_t)lroundf(db * 100.0f);
}

static inline float clampf_local(float value, float min_value,
                                 float max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static inline float update_envelope(float current, float input, float attack,
                                    float release) {
  float alpha = (input > current) ? attack : release;
  return current + ((input - current) * alpha);
}

static inline float process_channel(dsp_channel_state_t *state, float x,
                                    bool *gate_active, bool *comp_active,
                                    bool *limiter_active, float *post_abs) {
  float y = x - state->dc_x1 + (DC_BLOCK_R * state->dc_y1);
  state->dc_x1 = x;
  state->dc_y1 = y;

  float abs_in = fabsf(y);
  state->env = update_envelope(state->env, abs_in, ENV_ATTACK, ENV_RELEASE);

  float gate_threshold = clampf_local(s_noise_floor * 1.9f, 0.0035f, 0.035f);
  float gate_target = (state->env < gate_threshold)
                          ? clampf_local(state->env / gate_threshold,
                                         GATE_MIN_GAIN, 1.0f)
                          : 1.0f;
  state->gate_gain = update_envelope(state->gate_gain, gate_target, GATE_ATTACK,
                                     GATE_RELEASE);
  if (state->gate_gain == 0.0f) {
    state->gate_gain = 1.0f;
  }
  if (state->gate_gain < 0.995f) {
    *gate_active = true;
  }
  y *= state->gate_gain;

  float level = fabsf(y);
  float comp_target = 1.0f;
  if (level > COMP_THRESHOLD) {
    float over = level / COMP_THRESHOLD;
    float compressed = powf(over, (1.0f / COMP_RATIO) - 1.0f);
    comp_target = clampf_local(compressed, 0.40f, 1.0f);
  }
  state->comp_gain = update_envelope(state->comp_gain, comp_target, COMP_ATTACK,
                                     COMP_RELEASE);
  if (state->comp_gain == 0.0f) {
    state->comp_gain = 1.0f;
  }
  if (state->comp_gain < 0.995f) {
    *comp_active = true;
  }
  y *= state->comp_gain;

  float norm = y / 32768.0f;
  float abs_norm = fabsf(norm);
  if (abs_norm > LIMIT_THRESHOLD) {
    float excess = abs_norm - LIMIT_THRESHOLD;
    float compressed =
        LIMIT_THRESHOLD + (excess / (1.0f + (excess * 8.0f)));
    norm = (norm < 0.0f) ? -compressed : compressed;
    *limiter_active = true;
  }

  *post_abs = fabsf(norm);
  return clampf_local(norm * 32768.0f, -32768.0f, 32767.0f);
}

static inline float apply_limiter_only(float sample, bool *limiter_active) {
  float norm = sample / 32768.0f;
  float abs_norm = fabsf(norm);
  if (abs_norm > LIMIT_THRESHOLD) {
    float excess = abs_norm - LIMIT_THRESHOLD;
    float compressed =
        LIMIT_THRESHOLD + (excess / (1.0f + (excess * 8.0f)));
    norm = (norm < 0.0f) ? -compressed : compressed;
    *limiter_active = true;
  }
  return clampf_local(norm * 32768.0f, -32768.0f, 32767.0f);
}

esp_err_t audio_dsp_init(uint32_t sample_rate_hz) {
  if (sample_rate_hz > 0) {
    s_sample_rate_hz = sample_rate_hz;
  }
  (void)s_sample_rate_hz;

  memset(&s_left, 0, sizeof(s_left));
  memset(&s_right, 0, sizeof(s_right));
  memset(&s_stats, 0, sizeof(s_stats));
  s_left.gate_gain = 1.0f;
  s_right.gate_gain = 1.0f;
  s_left.comp_gain = 1.0f;
  s_right.comp_gain = 1.0f;
  s_noise_floor = 0.0025f;
  s_peak_norm = 0.0f;
  s_rms_norm = 0.0f;
  s_low_env = 0.0f;
  s_mid_env = 0.0f;
  s_high_env = 0.0f;
  s_low_lpf_l = 0.0f;
  s_low_lpf_r = 0.0f;
#if defined(CONFIG_AUDIO_FIDELITY_MODE_ENHANCED)
  s_mode = AUDIO_DSP_MODE_ENHANCED;
#else
  s_mode = AUDIO_DSP_MODE_LIMITER_ONLY;
#endif
  s_ready = true;
  return ESP_OK;
}

void audio_dsp_set_mode(audio_dsp_mode_t mode) { s_mode = mode; }

audio_dsp_mode_t audio_dsp_get_mode(void) { return s_mode; }

void audio_dsp_process(int16_t *samples, size_t sample_count) {
  if (!s_ready || samples == NULL) {
    return;
  }

  bool gate_active = false;
  bool comp_active = false;
  bool limiter_active = false;
  float block_peak = 0.0f;
  float block_energy = 0.0f;
  float block_low = 0.0f;
  float block_mid = 0.0f;
  float block_high = 0.0f;
  size_t frames = 0;

  if (s_mode == AUDIO_DSP_MODE_LIMITER_ONLY) {
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
      bool lim_l = false;
      bool lim_r = false;
      float out_l = apply_limiter_only((float)samples[i], &lim_l);
      float out_r = apply_limiter_only((float)samples[i + 1], &lim_r);
      samples[i] = (int16_t)out_l;
      samples[i + 1] = (int16_t)out_r;
      limiter_active = limiter_active || lim_l || lim_r;

      float mono = 0.5f * (((float)samples[i] / 32768.0f) +
                           ((float)samples[i + 1] / 32768.0f));
      float abs_mono = fabsf(mono);
      float sample_energy = mono * mono;
      if (abs_mono > block_peak) {
        block_peak = abs_mono;
      }
      block_energy += sample_energy;
      block_low += abs_mono;
      block_mid += abs_mono;
      block_high += abs_mono;
      frames++;
    }
  } else {
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
      float raw_l = (float)samples[i];
      float raw_r = (float)samples[i + 1];

      bool gate_l = false;
      bool gate_r = false;
      bool comp_l = false;
      bool comp_r = false;
      bool lim_l = false;
      bool lim_r = false;
      float post_abs_l = 0.0f;
      float post_abs_r = 0.0f;

      float out_l = process_channel(&s_left, raw_l, &gate_l, &comp_l, &lim_l,
                                    &post_abs_l);
      float out_r = process_channel(&s_right, raw_r, &gate_r, &comp_r, &lim_r,
                                    &post_abs_r);

      samples[i] = (int16_t)out_l;
      samples[i + 1] = (int16_t)out_r;

      gate_active = gate_active || gate_l || gate_r;
      comp_active = comp_active || comp_l || comp_r;
      limiter_active = limiter_active || lim_l || lim_r;

      float mono = 0.5f * (((float)samples[i] / 32768.0f) +
                           ((float)samples[i + 1] / 32768.0f));
      float abs_mono = fabsf(mono);
      float sample_energy = mono * mono;
      if (abs_mono > block_peak) {
        block_peak = abs_mono;
      }
      block_energy += sample_energy;

      s_low_lpf_l += ((((float)samples[i] / 32768.0f) - s_low_lpf_l) *
                      LOW_CROSSOVER_ALPHA);
      s_low_lpf_r += ((((float)samples[i + 1] / 32768.0f) - s_low_lpf_r) *
                      LOW_CROSSOVER_ALPHA);
      float low = 0.5f * (s_low_lpf_l + s_low_lpf_r);
      float high = mono - low;
      block_low += fabsf(low);
      s_mid_env +=
          ((fabsf(mono) - fabsf(high) - s_mid_env) * MID_ENVELOPE_ALPHA);
      block_mid += fabsf(s_mid_env);
      block_high += fabsf(high);
      frames++;
    }
  }

  if (frames == 0) {
    return;
  }

  float block_rms = sqrtf(block_energy / (float)frames);
  float quiet_reference = fminf(block_rms, block_peak);
  if (quiet_reference < s_noise_floor) {
    s_noise_floor += (quiet_reference - s_noise_floor) * NOISE_FLOOR_FALL;
  } else {
    s_noise_floor += (quiet_reference - s_noise_floor) * NOISE_FLOOR_RISE;
  }
  s_noise_floor = clampf_local(s_noise_floor, 0.0005f, 0.08f);

  s_peak_norm += (block_peak - s_peak_norm) * STATS_SMOOTH_ALPHA;
  s_rms_norm += (block_rms - s_rms_norm) * RMS_ENVELOPE_ALPHA;
  s_low_env += (((block_low / (float)frames) - s_low_env) * BAND_ENVELOPE_ALPHA);
  s_mid_env += (((block_mid / (float)frames) - s_mid_env) * BAND_ENVELOPE_ALPHA);
  s_high_env +=
      (((block_high / (float)frames) - s_high_env) * BAND_ENVELOPE_ALPHA);

  s_stats.frames_processed += (uint32_t)frames;
  if (gate_active) {
    s_stats.gate_active_frames += (uint32_t)frames;
  }
  if (comp_active) {
    s_stats.compressor_active_frames += (uint32_t)frames;
  }
  if (limiter_active) {
    s_stats.limiter_events++;
  }

  s_stats.peak_dbfs_x100 = dbfs_x100_from_norm(s_peak_norm);
  s_stats.rms_dbfs_x100 = dbfs_x100_from_norm(s_rms_norm);
  s_stats.noise_floor_dbfs_x100 = dbfs_x100_from_norm(s_noise_floor);
  s_stats.low_band_dbfs_x100 = dbfs_x100_from_norm(s_low_env);
  s_stats.mid_band_dbfs_x100 = dbfs_x100_from_norm(s_mid_env);
  s_stats.high_band_dbfs_x100 = dbfs_x100_from_norm(s_high_env);
  if (s_mode == AUDIO_DSP_MODE_LIMITER_ONLY) {
    s_stats.gate_gain_pct = 100;
    s_stats.compressor_gain_pct = 100;
  } else {
    s_stats.gate_gain_pct = (uint8_t)lroundf(
        clampf_local(0.5f * (s_left.gate_gain + s_right.gate_gain), 0.0f, 1.0f) *
        100.0f);
    s_stats.compressor_gain_pct = (uint8_t)lroundf(clampf_local(
        0.5f * (s_left.comp_gain + s_right.comp_gain), 0.0f, 1.0f) * 100.0f);
  }
}

void audio_dsp_get_stats(audio_dsp_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  memcpy(stats, &s_stats, sizeof(*stats));
}
