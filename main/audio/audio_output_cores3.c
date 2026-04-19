#include "audio_output.h"
#include "audio_output_common.h"

#include "audio_resample.h"
#include "driver/i2s_std.h"
#include "iot_board.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

#define TAG                "audio_output"
#define CORES3_CHANNELS    2
#define CORES3_CHANNELMASK 0x03
#define TEST_TONE_RATE     44100
#define TEST_TONE_CHUNK_MS 20
#define TEST_TONE_PI       3.14159265358979323846

#define VOLUME_MIN_DB            (-30.0f)
#define VOLUME_MAX_DB            0.0f
#define CORES3_HW_VOLUME_MIN     0
#define CORES3_HW_VOLUME_MAX     100
#define VOLUME_RAMP_STEP_DB      1.5f
#define VOLUME_RAMP_INTERVAL_US  25000

static esp_codec_dev_handle_t s_speaker_handle = NULL;
static uint32_t s_output_rate = AO_OUTPUT_RATE;
static int s_output_mclk_multiple = 0;
static bool s_output_open = false;
static portMUX_TYPE s_volume_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_target_volume_db = -15.0f;
static float s_current_volume_db = -15.0f;
static bool s_volume_state_init = false;
static bool s_output_muted = false;
static int64_t s_last_ramp_update_us = 0;

static float clamp_volume_db(float db) {
  if (db < VOLUME_MIN_DB) {
    return VOLUME_MIN_DB;
  }
  if (db > VOLUME_MAX_DB) {
    return VOLUME_MAX_DB;
  }
  return db;
}

static int db_to_hw_volume(float db) {
  float clamped = clamp_volume_db(db);
  float ratio = (clamped - VOLUME_MIN_DB) / (VOLUME_MAX_DB - VOLUME_MIN_DB);
  int hw = (int)lroundf((float)CORES3_HW_VOLUME_MIN +
                        ratio * (float)(CORES3_HW_VOLUME_MAX - CORES3_HW_VOLUME_MIN));
  if (hw < CORES3_HW_VOLUME_MIN) {
    hw = CORES3_HW_VOLUME_MIN;
  }
  if (hw > CORES3_HW_VOLUME_MAX) {
    hw = CORES3_HW_VOLUME_MAX;
  }
  return hw;
}

static void apply_volume_step(bool force) {
  if (s_speaker_handle == NULL) {
    return;
  }

  int64_t now_us = esp_timer_get_time();
  float target_db;
  float current_db;
  bool muted;
  int64_t last_update_us;

  portENTER_CRITICAL(&s_volume_lock);
  target_db = s_target_volume_db;
  current_db = s_current_volume_db;
  muted = s_output_muted;
  last_update_us = s_last_ramp_update_us;
  portEXIT_CRITICAL(&s_volume_lock);

  if (!force && (now_us - last_update_us) < VOLUME_RAMP_INTERVAL_US) {
    return;
  }

  float next_db = current_db;
  float diff = target_db - current_db;
  if (fabsf(diff) > 0.0001f) {
    if (diff > VOLUME_RAMP_STEP_DB) {
      next_db = current_db + VOLUME_RAMP_STEP_DB;
    } else if (diff < -VOLUME_RAMP_STEP_DB) {
      next_db = current_db - VOLUME_RAMP_STEP_DB;
    } else {
      next_db = target_db;
    }
  }

  int hw_volume = db_to_hw_volume(next_db);
  bool hw_mute = muted || (next_db <= VOLUME_MIN_DB + 0.01f);
  int mute_ret = esp_codec_dev_set_out_mute(s_speaker_handle, hw_mute);
  int vol_ret = esp_codec_dev_set_out_vol(s_speaker_handle, hw_volume);
  if (mute_ret != ESP_CODEC_DEV_OK || vol_ret != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "CoreS3 volume apply failed mute=%d vol=%d target=%.1f cur=%.1f",
             mute_ret, vol_ret, target_db, next_db);
    return;
  }

  portENTER_CRITICAL(&s_volume_lock);
  s_current_volume_db = next_db;
  s_last_ramp_update_us = now_us;
  portEXIT_CRITICAL(&s_volume_lock);
}

static int cores3_mclk_multiple_for_rate(uint32_t rate) {
  switch (rate) {
  case 44100:
  case 88200:
  case 176400:
    return I2S_MCLK_MULTIPLE_384;
  default:
    return 0;
  }
}

static void refresh_output_controls(void) {
  if (s_speaker_handle == NULL) {
    return;
  }
  apply_volume_step(true);
}

static esp_err_t open_output(uint32_t rate) {
  int mclk_multiple = cores3_mclk_multiple_for_rate(rate);
  esp_codec_dev_sample_info_t sample_cfg = {
      .bits_per_sample = 16,
      .channel = CORES3_CHANNELS,
      .channel_mask = CORES3_CHANNELMASK,
      .sample_rate = rate,
      .mclk_multiple = mclk_multiple,
  };

  if (s_output_open && s_output_rate == rate &&
      s_output_mclk_multiple == mclk_multiple) {
    refresh_output_controls();
    return ESP_OK;
  }

  if (s_output_open) {
    esp_codec_dev_close(s_speaker_handle);
    s_output_open = false;
  }

  int ret = esp_codec_dev_open(s_speaker_handle, &sample_cfg);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "Failed to open speaker path at %" PRIu32 " Hz", rate);
    return ESP_FAIL;
  }

  refresh_output_controls();
  s_output_rate = rate;
  s_output_mclk_multiple = mclk_multiple;
  s_output_open = true;
  ESP_LOGI(TAG,
           "CoreS3 speaker open: rate=%" PRIu32 " bits=%d channels=%d "
           "mask=0x%x mclk=%dx",
           rate, sample_cfg.bits_per_sample, sample_cfg.channel,
           sample_cfg.channel_mask, mclk_multiple ? mclk_multiple : 256);
  return ESP_OK;
}

static esp_err_t hw_write_pcm(void *ctx, const int16_t *data, size_t bytes,
                              TickType_t wait) {
  (void)ctx;
  (void)wait;
  apply_volume_step(false);
  int ret = esp_codec_dev_write(s_speaker_handle, (void *)data, bytes);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "Speaker write failed (%d)", ret);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static void hw_flush(void *ctx) {
  (void)ctx;
  refresh_output_controls();
  if (!s_output_open) {
    if (open_output(s_output_rate) != ESP_OK) {
      ESP_LOGW(TAG, "Failed to re-open CoreS3 speaker path on flush");
    }
  }
}

static const audio_output_hw_ops_t s_hw_ops = {
    .write_pcm = hw_write_pcm,
    .flush_output = hw_flush,
    .task_name = "audio_play",
    .software_volume = false,
    .ctx = NULL,
};

esp_err_t audio_output_init(void) {
  s_speaker_handle =
      (esp_codec_dev_handle_t)iot_board_get_handle(BOARD_AUDIO_SPK_ID);
  ESP_RETURN_ON_FALSE(s_speaker_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                      "CoreS3 speaker path not initialized");

  esp_codec_set_disable_when_closed(s_speaker_handle, false);

  portENTER_CRITICAL(&s_volume_lock);
  if (!s_volume_state_init) {
    s_target_volume_db = -15.0f;
    s_current_volume_db = s_target_volume_db;
    s_volume_state_init = true;
  }
  s_last_ramp_update_us = 0;
  portEXIT_CRITICAL(&s_volume_lock);

  ESP_RETURN_ON_ERROR(open_output(AO_OUTPUT_RATE), TAG, "speaker open failed");
  audio_resample_init(44100, AO_OUTPUT_RATE, 2);
  audio_output_common_init(&s_hw_ops);
  return ESP_OK;
}

void audio_output_start(void) { audio_output_common_start(); }

void audio_output_stop(void) { audio_output_common_stop(); }

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  (void)wait;
  apply_volume_step(false);
  int ret = esp_codec_dev_write(s_speaker_handle, (void *)data, bytes);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "Speaker write failed (%d)", ret);
    return ESP_FAIL;
  }
  return ESP_OK;
}

void audio_output_set_sample_rate(uint32_t rate) {
  ESP_LOGI(TAG, "Setting speaker sample rate to %" PRIu32 " Hz", rate);
  if (open_output(rate) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to re-open CoreS3 speaker path");
  }
}

void audio_output_flush(void) { audio_output_common_flush(); }

void audio_output_set_source_rate(int rate) {
  audio_output_common_set_source_rate(rate);
}

bool audio_output_is_active(void) { return audio_output_common_is_active(); }

void audio_output_set_target_volume_db(float volume_db) {
  float clamped = clamp_volume_db(volume_db);
  bool changed = false;

  portENTER_CRITICAL(&s_volume_lock);
  changed = fabsf(s_target_volume_db - clamped) > 0.001f;
  s_target_volume_db = clamped;
  if (!s_volume_state_init) {
    s_current_volume_db = clamped;
    s_volume_state_init = true;
  }
  portEXIT_CRITICAL(&s_volume_lock);

  if (changed) {
    ESP_LOGI(TAG, "Volume target update: %.1f dB", clamped);
  }
  apply_volume_step(true);
}

void audio_output_set_muted(bool muted) {
  bool changed = false;

  portENTER_CRITICAL(&s_volume_lock);
  changed = (s_output_muted != muted);
  s_output_muted = muted;
  portEXIT_CRITICAL(&s_volume_lock);

  if (changed) {
    ESP_LOGI(TAG, "Output mute: %s", muted ? "on" : "off");
  }
  apply_volume_step(true);
}

esp_err_t audio_output_get_diag(audio_output_diag_t *diag) {
  ESP_RETURN_ON_FALSE(diag != NULL, ESP_ERR_INVALID_ARG, TAG,
                      "diag pointer is required");

  memset(diag, 0, sizeof(*diag));
  diag->speaker_open = s_output_open;
  diag->output_rate = s_output_rate;
  diag->bits_per_sample = 16;
  diag->channels = CORES3_CHANNELS;
  diag->channel_mask = CORES3_CHANNELMASK;
  diag->mclk_multiple = s_output_mclk_multiple;
  diag->volume = -1;
  diag->current_volume_db = -15.0f;
  diag->target_volume_db = -15.0f;
  diag->ramping = false;
  diag->reg04 = -1;
  diag->reg05 = -1;
  diag->reg06 = -1;
  diag->reg0c = -1;
  diag->reg12 = -1;
  diag->reg14 = -1;

  ESP_RETURN_ON_FALSE(s_speaker_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                      "speaker handle unavailable");

  bool muted = false;
  int volume = 0;
  if (esp_codec_dev_get_out_mute(s_speaker_handle, &muted) == ESP_CODEC_DEV_OK) {
    diag->muted = muted;
  }
  if (esp_codec_dev_get_out_vol(s_speaker_handle, &volume) == ESP_CODEC_DEV_OK) {
    diag->volume = volume;
  }

  portENTER_CRITICAL(&s_volume_lock);
  diag->current_volume_db = s_current_volume_db;
  diag->target_volume_db = s_target_volume_db;
  diag->ramping = fabsf(s_target_volume_db - s_current_volume_db) > 0.001f;
  portEXIT_CRITICAL(&s_volume_lock);

  (void)esp_codec_dev_read_reg(s_speaker_handle, 0x04, &diag->reg04);
  (void)esp_codec_dev_read_reg(s_speaker_handle, 0x05, &diag->reg05);
  (void)esp_codec_dev_read_reg(s_speaker_handle, 0x06, &diag->reg06);
  (void)esp_codec_dev_read_reg(s_speaker_handle, 0x0C, &diag->reg0c);
  (void)esp_codec_dev_read_reg(s_speaker_handle, 0x12, &diag->reg12);
  (void)esp_codec_dev_read_reg(s_speaker_handle, 0x14, &diag->reg14);
  return ESP_OK;
}

esp_err_t audio_output_play_test_tone(uint32_t frequency_hz, uint32_t duration_ms,
                                      uint8_t amplitude_pct) {
  ESP_RETURN_ON_FALSE(s_speaker_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                      "speaker handle unavailable");

  if (frequency_hz == 0) {
    frequency_hz = 1000;
  }
  if (duration_ms == 0) {
    duration_ms = 800;
  }
  if (amplitude_pct == 0) {
    amplitude_pct = 20;
  }
  if (amplitude_pct > 100) {
    amplitude_pct = 100;
  }

  bool restart_worker = audio_output_is_active();
  if (restart_worker) {
    audio_output_common_stop();
  }

  esp_err_t err = open_output(TEST_TONE_RATE);
  if (err != ESP_OK) {
    if (restart_worker) {
      audio_output_common_start();
    }
    return err;
  }

  size_t chunk_frames = (TEST_TONE_RATE * TEST_TONE_CHUNK_MS) / 1000U;
  if (chunk_frames == 0) {
    chunk_frames = TEST_TONE_RATE / 50U;
  }
  int16_t *pcm = calloc(chunk_frames * CORES3_CHANNELS, sizeof(int16_t));
  if (pcm == NULL) {
    if (restart_worker) {
      audio_output_common_start();
    }
    return ESP_ERR_NO_MEM;
  }

  double phase = 0.0;
  double phase_step =
      (2.0 * TEST_TONE_PI * (double)frequency_hz) / (double)TEST_TONE_RATE;
  float amplitude = 32767.0f * ((float)amplitude_pct / 100.0f);
  uint32_t remaining_ms = duration_ms;

  while (remaining_ms > 0) {
    uint32_t this_chunk_ms =
        remaining_ms > TEST_TONE_CHUNK_MS ? TEST_TONE_CHUNK_MS : remaining_ms;
    size_t frames = (TEST_TONE_RATE * this_chunk_ms) / 1000U;
    if (frames == 0) {
      frames = 1;
    }

    for (size_t i = 0; i < frames; i++) {
      int16_t sample = (int16_t)(sinf((float)phase) * amplitude);
      pcm[i * CORES3_CHANNELS] = sample;
      pcm[i * CORES3_CHANNELS + 1] = sample;
      phase += phase_step;
      if (phase >= 2.0 * TEST_TONE_PI) {
        phase -= 2.0 * TEST_TONE_PI;
      }
    }

    err = hw_write_pcm(NULL, pcm, frames * CORES3_CHANNELS * sizeof(int16_t),
                       portMAX_DELAY);
    if (err != ESP_OK) {
      break;
    }
    remaining_ms -= this_chunk_ms;
  }

  free(pcm);
  if (restart_worker) {
    audio_output_common_start();
  }
  return err;
}
