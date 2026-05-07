#include "audio_output.h"
#include "audio_output_common.h"

#include "audio_resample.h"
#include "driver/i2s_std.h"
#include "iot_board.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
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
static bool s_hw_mute_applied = false;
static bool s_hw_mute_state_init = false;
/** Last values pushed to codec/AW88298; avoid redundant SPI when unchanged. */
static bool s_hw_push_valid = false;
static int s_hw_push_vol = 0;
static bool s_hw_push_mute = false;
static SemaphoreHandle_t s_owner_lock = NULL;
static bool s_external_owner = false;
static bool s_worker_was_running = false;
static char s_owner_tag[24] = {0};

static audio_output_ref_tap_fn s_ref_tap_fn = NULL;
static void *s_ref_tap_ctx = NULL;
static portMUX_TYPE s_ref_tap_spin = portMUX_INITIALIZER_UNLOCKED;

/* ---- I2S bus state (RX + TX share one I2S peripheral on CoreS3) ---- */
static i2s_bus_state_t s_i2s_state = I2S_BUS_IDLE;
static bool s_i2s_rx_open = false;
static bool s_i2s_tx_open = false;

static void i2s_state_update(void) {
    i2s_bus_state_t prev = s_i2s_state;
    if (s_i2s_rx_open && s_i2s_tx_open)      s_i2s_state = I2S_BUS_FULL_DUPLEX;
    else if (s_i2s_rx_open && !s_i2s_tx_open) s_i2s_state = I2S_BUS_RX_ONLY;
    else if (!s_i2s_rx_open && s_i2s_tx_open) s_i2s_state = I2S_BUS_TX_ONLY;
    else                                       s_i2s_state = I2S_BUS_IDLE;
    if (s_i2s_state != prev) {
        const char *names[] = { "IDLE", "RX_ONLY", "TX_ONLY", "FULL_DUPLEX" };
        ESP_LOGI(TAG, "I2S bus: %s → %s (rx=%d tx=%d)",
                 names[prev], names[s_i2s_state],
                 s_i2s_rx_open ? 1 : 0, s_i2s_tx_open ? 1 : 0);
    }
}

void audio_output_notify_i2s_rx(bool open) {
    s_i2s_rx_open = open;
    i2s_state_update();
}

void audio_output_notify_i2s_tx(bool open) {
    s_i2s_tx_open = open;
    i2s_state_update();
}

i2s_bus_state_t audio_output_get_i2s_state(void) { return s_i2s_state; }

static bool owner_locked_by_external(void) {
  bool locked = false;
  if (s_owner_lock == NULL) {
    return false;
  }
  if (xSemaphoreTake(s_owner_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
    locked = s_external_owner;
    xSemaphoreGive(s_owner_lock);
  }
  return locked;
}

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
  // Map dB to perceived loudness using a logarithmic scale.
  float min_linear = powf(10.0f, VOLUME_MIN_DB / 20.0f);
  float linear = powf(10.0f, clamped / 20.0f);
  float ratio = (linear - min_linear) / (1.0f - min_linear);
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
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

static void hw_volume_push_cache_invalidate(void) { s_hw_push_valid = false; }

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
    // Keep mute state synchronized even when volume ramp tick is throttled.
    bool hw_mute = muted || (current_db <= VOLUME_MIN_DB + 0.01f);
    if (!s_hw_mute_state_init || hw_mute != s_hw_mute_applied) {
      int mute_ret = esp_codec_dev_set_out_mute(s_speaker_handle, hw_mute);
      if (mute_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "CoreS3 mute apply failed mute=%d target=%.1f cur=%.1f",
                 mute_ret, target_db, current_db);
      } else {
        s_hw_mute_applied = hw_mute;
        s_hw_mute_state_init = true;
        s_hw_push_mute = hw_mute;
        ESP_LOGD(TAG, "Volume state sync: mute=%d target=%.1f cur=%.1f",
                 hw_mute ? 1 : 0, target_db, current_db);
      }
    }
    return;
  }

  float next_db = force ? target_db : current_db;
  float diff = target_db - current_db;
  if (!force && fabsf(diff) > 0.0001f) {
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
  const bool same_hw =
      s_hw_push_valid && hw_volume == s_hw_push_vol && hw_mute == s_hw_push_mute;
  if (same_hw) {
    portENTER_CRITICAL(&s_volume_lock);
    s_current_volume_db = next_db;
    s_last_ramp_update_us = now_us;
    portEXIT_CRITICAL(&s_volume_lock);
    s_hw_mute_applied = hw_mute;
    s_hw_mute_state_init = true;
    if (fabsf(target_db - next_db) > 4.0f) {
      ESP_LOGW(TAG,
               "Volume ramp lag: target=%.1f current=%.1f hw=%d mute=%d force=%d",
               target_db, next_db, hw_volume, hw_mute ? 1 : 0, force ? 1 : 0);
    }
    return;
  }

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
  s_hw_mute_applied = hw_mute;
  s_hw_mute_state_init = true;
  s_hw_push_vol = hw_volume;
  s_hw_push_mute = hw_mute;
  s_hw_push_valid = true;

  if (fabsf(target_db - next_db) > 4.0f) {
    ESP_LOGW(TAG,
             "Volume ramp lag: target=%.1f current=%.1f hw=%d mute=%d force=%d",
             target_db, next_db, hw_volume, hw_mute ? 1 : 0, force ? 1 : 0);
  }
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

static esp_err_t codec_write_checked(const void *data, size_t bytes) {
  if (bytes > (size_t)INT_MAX) {
    ESP_LOGE(TAG, "Speaker write too large: %zu bytes", bytes);
    return ESP_ERR_INVALID_ARG;
  }

  audio_output_ref_tap_fn tap_fn = NULL;
  void *tap_ctx = NULL;
  portENTER_CRITICAL(&s_ref_tap_spin);
  tap_fn = s_ref_tap_fn;
  tap_ctx = s_ref_tap_ctx;
  portEXIT_CRITICAL(&s_ref_tap_spin);
  if (tap_fn != NULL && data != NULL && bytes >= sizeof(int16_t) * CORES3_CHANNELS) {
    size_t frame_bytes = sizeof(int16_t) * CORES3_CHANNELS;
    if ((bytes % frame_bytes) == 0U) {
      size_t frames = bytes / frame_bytes;
      tap_fn((const int16_t *)data, frames, s_output_rate, tap_ctx);
    }
  }

  int ret = esp_codec_dev_write(s_speaker_handle, (void *)data, (int)bytes);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "Speaker write failed (%d)", ret);
    return ESP_FAIL;
  }

  return ESP_OK;
}

static esp_err_t open_output_internal(uint32_t rate, bool allow_external_owner,
                                      const char *owner_label) {
  if (!allow_external_owner && owner_locked_by_external()) {
    return ESP_ERR_INVALID_STATE;
  }
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
    hw_volume_push_cache_invalidate();
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
  audio_output_notify_i2s_tx(true);
  ESP_LOGI(TAG,
           "CoreS3 speaker open: rate=%" PRIu32 " bits=%d channels=%d "
           "mask=0x%x mclk=%dx%s%s",
           rate, sample_cfg.bits_per_sample, sample_cfg.channel,
           sample_cfg.channel_mask, mclk_multiple ? mclk_multiple : 256,
           owner_label != NULL ? " owner=" : "",
           owner_label != NULL ? owner_label : "");
  return ESP_OK;
}

static esp_err_t hw_write_pcm(void *ctx, const int16_t *data, size_t bytes,
                              TickType_t wait) {
  (void)ctx;
  (void)wait;
  apply_volume_step(false);
  return codec_write_checked(data, bytes);
}

static void hw_flush(void *ctx) {
  (void)ctx;
  refresh_output_controls();
  if (!s_output_open) {
    if (open_output_internal(s_output_rate, false, NULL) != ESP_OK) {
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
  s_hw_mute_applied = false;
  s_hw_mute_state_init = false;
  portEXIT_CRITICAL(&s_volume_lock);

  ESP_RETURN_ON_ERROR(open_output_internal(AO_OUTPUT_RATE, false, NULL), TAG, "speaker open failed");
  audio_resample_init(44100, AO_OUTPUT_RATE, 2);
  audio_output_common_init(&s_hw_ops);
  return ESP_OK;
}

void audio_output_start(void) {
  if (owner_locked_by_external()) {
    ESP_LOGW(TAG, "start ignored: speaker owned by %s", s_owner_tag);
    return;
  }
  if (!s_output_open) {
    if (open_output_internal(s_output_rate, false, NULL) != ESP_OK) {
      ESP_LOGW(TAG, "Failed to open CoreS3 speaker path on start");
      return;
    }
  }
  audio_output_common_start();
}

void audio_output_stop(void) {
  if (owner_locked_by_external()) {
    return;
  }
  audio_output_common_stop();
  if (s_output_open && s_speaker_handle != NULL) {
    hw_volume_push_cache_invalidate();
    int ret = esp_codec_dev_close(s_speaker_handle);
    if (ret != ESP_CODEC_DEV_OK) {
      ESP_LOGD(TAG, "Speaker close skipped (already closed: %d)", ret);
    }
    s_output_open = false;
    audio_output_notify_i2s_tx(false);
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  if (owner_locked_by_external()) {
    return ESP_ERR_INVALID_STATE;
  }
  (void)wait;
  apply_volume_step(false);
  return codec_write_checked(data, bytes);
}

void audio_output_set_sample_rate(uint32_t rate) {
  if (owner_locked_by_external()) {
    ESP_LOGW(TAG, "sample rate update ignored: speaker owned by %s", s_owner_tag);
    return;
  }
  ESP_LOGI(TAG, "Setting speaker sample rate to %" PRIu32 " Hz", rate);
  if (open_output_internal(rate, false, NULL) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to re-open CoreS3 speaker path");
  }
}

esp_err_t audio_output_external_open(uint32_t rate) {
  if (!owner_locked_by_external()) {
    return ESP_ERR_INVALID_STATE;
  }
  if (open_output_internal(rate, true, "external") != ESP_OK) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t audio_output_external_write(const void *data, size_t bytes, TickType_t wait) {
  if (!owner_locked_by_external() || !s_output_open) {
    return ESP_ERR_INVALID_STATE;
  }
  (void)wait;
  apply_volume_step(false);
  return codec_write_checked(data, bytes);
}

void audio_output_external_close(void) {
  if (!s_output_open || s_speaker_handle == NULL) {
    return;
  }
  hw_volume_push_cache_invalidate();
  int ret = esp_codec_dev_close(s_speaker_handle);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGD(TAG, "Speaker close skipped (already closed: %d)", ret);
  }
  s_output_open = false;
  audio_output_notify_i2s_tx(false);
  ESP_LOGI(TAG, "CoreS3 speaker closed owner=external");
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

void audio_output_set_fidelity_mode(audio_fidelity_mode_t mode) {
  audio_output_common_set_fidelity_mode(mode);
}

audio_fidelity_mode_t audio_output_get_fidelity_mode(void) {
  return audio_output_common_get_fidelity_mode();
}

esp_err_t audio_output_acquire_external(const char *owner_tag, bool stop_worker) {
  if (s_owner_lock == NULL) {
    s_owner_lock = xSemaphoreCreateMutex();
    if (s_owner_lock == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  if (xSemaphoreTake(s_owner_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (s_external_owner) {
    if (owner_tag != NULL && strncmp(s_owner_tag, owner_tag, sizeof(s_owner_tag)) == 0) {
      if (stop_worker && audio_output_common_is_active()) {
        s_worker_was_running = true;
        xSemaphoreGive(s_owner_lock);
        audio_output_common_stop();
        ESP_LOGI(TAG, "speaker ownership escalated for %s (worker=stopped)",
                 s_owner_tag[0] ? s_owner_tag : "external");
        return ESP_OK;
      }
      xSemaphoreGive(s_owner_lock);
      return ESP_OK;
    }
    xSemaphoreGive(s_owner_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_external_owner = true;
  s_owner_tag[0] = '\0';
  if (owner_tag != NULL) {
    snprintf(s_owner_tag, sizeof(s_owner_tag), "%s", owner_tag);
  }
  s_worker_was_running = stop_worker && audio_output_common_is_active();
  xSemaphoreGive(s_owner_lock);

  if (stop_worker) {
    audio_output_common_stop();
  }
  ESP_LOGI(TAG, "speaker ownership acquired by %s (worker=%s, speaker_open=%d)",
           s_owner_tag[0] ? s_owner_tag : "external",
           stop_worker ? "stopped" : "kept", s_output_open ? 1 : 0);
  return ESP_OK;
}

esp_err_t audio_output_release_external(const char *owner_tag, bool resume_worker) {
  if (s_owner_lock == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(s_owner_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  if (!s_external_owner) {
    xSemaphoreGive(s_owner_lock);
    return ESP_OK;
  }
  if (owner_tag != NULL && s_owner_tag[0] != '\0' &&
      strncmp(s_owner_tag, owner_tag, sizeof(s_owner_tag)) != 0) {
    xSemaphoreGive(s_owner_lock);
    return ESP_ERR_INVALID_STATE;
  }
  bool should_resume = resume_worker && s_worker_was_running;
  s_external_owner = false;
  s_worker_was_running = false;
  s_owner_tag[0] = '\0';
  xSemaphoreGive(s_owner_lock);

  if (should_resume) {
    if (!s_output_open) {
      (void)open_output_internal(s_output_rate, false, NULL);
    }
    audio_output_common_start();
  }
  ESP_LOGI(TAG, "speaker ownership released (resume_worker=%d, speaker_open=%d)",
           should_resume ? 1 : 0, s_output_open ? 1 : 0);
  return ESP_OK;
}

esp_err_t audio_output_get_external_owner(char *owner_tag, size_t owner_tag_len,
                                          bool *owned) {
  ESP_RETURN_ON_FALSE(owned != NULL, ESP_ERR_INVALID_ARG, TAG,
                      "owned pointer is required");

  *owned = false;
  if (owner_tag != NULL && owner_tag_len > 0) {
    owner_tag[0] = '\0';
  }

  if (s_owner_lock == NULL) {
    return ESP_OK;
  }
  if (xSemaphoreTake(s_owner_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  *owned = s_external_owner;
  if (owner_tag != NULL && owner_tag_len > 0 && s_external_owner) {
    snprintf(owner_tag, owner_tag_len, "%s",
             s_owner_tag[0] ? s_owner_tag : "external");
  }

  xSemaphoreGive(s_owner_lock);
  return ESP_OK;
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
  if (owner_locked_by_external()) {
    ESP_LOGW(TAG, "test tone blocked: speaker owned by %s", s_owner_tag);
    return ESP_ERR_INVALID_STATE;
  }

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

  esp_err_t err = open_output_internal(TEST_TONE_RATE, false, NULL);
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
  int16_t *pcm = heap_caps_malloc(chunk_frames * CORES3_CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcm) pcm = calloc(chunk_frames * CORES3_CHANNELS, sizeof(int16_t));
  if (pcm) memset(pcm, 0, chunk_frames * CORES3_CHANNELS * sizeof(int16_t));
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

void audio_output_set_ref_tap(audio_output_ref_tap_fn fn, void *ctx) {
  portENTER_CRITICAL(&s_ref_tap_spin);
  s_ref_tap_fn = fn;
  s_ref_tap_ctx = ctx;
  portEXIT_CRITICAL(&s_ref_tap_spin);
}

esp_err_t audio_output_get_mic_handle(void **out_handle) {
  if (out_handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  void *handle = iot_board_get_handle(BOARD_AUDIO_MIC_ID);
  if (handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  *out_handle = handle;
  return ESP_OK;
}

esp_err_t audio_output_get_spk_handle(void **out_handle) {
  if (out_handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_speaker_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  *out_handle = (void *)s_speaker_handle;
  return ESP_OK;
}
