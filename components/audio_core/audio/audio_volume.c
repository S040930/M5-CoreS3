#include "audio_volume.h"

#include <math.h>

#include "audio_output.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "audio_vol";

#define NVS_NAMESPACE "airplay"
#define NVS_KEY_VOLUME "volume_db"

static float s_volume_db = -15.0f;
static bool s_loaded = false;

static float volume_db_to_linear(float db);

esp_err_t audio_volume_load(float *volume_db) {
  if (!volume_db) return ESP_ERR_INVALID_ARG;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return err;

  int32_t vol_fixed;
  err = nvs_get_i32(nvs, NVS_KEY_VOLUME, &vol_fixed);
  nvs_close(nvs);

  if (err == ESP_OK) {
    s_volume_db = (float)vol_fixed / 100.0f;
    s_loaded = true;
    ESP_LOGI(TAG, "loaded: %.2f dB", s_volume_db);
  }

  audio_output_set_target_volume_db(s_volume_db);
  *volume_db = s_volume_db;
  return err;
}

esp_err_t audio_volume_get(float *volume_db) {
  if (!volume_db) return ESP_ERR_INVALID_ARG;
  *volume_db = s_volume_db;
  return ESP_OK;
}

esp_err_t audio_volume_save(float volume_db) {
  if (s_loaded && volume_db == s_volume_db) return ESP_OK;

  audio_output_set_target_volume_db(volume_db);
  s_volume_db = volume_db;
  s_loaded = true;
  return ESP_OK;
}

esp_err_t audio_volume_persist(void) {
  if (!s_loaded) return ESP_OK;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return err;
  }

  int32_t vol_fixed = (int32_t)(s_volume_db * 100.0f);
  err = nvs_set_i32(nvs, NVS_KEY_VOLUME, vol_fixed);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);

  if (err == ESP_OK)
    ESP_LOGI(TAG, "persisted: %.2f dB", s_volume_db);
  else
    ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
  return err;
}

int32_t audio_volume_db_to_q15(float volume_db) {
  if (volume_db <= -30.0f) return 0;
  if (volume_db >= 0.0f) return 32768;
  float linear = volume_db_to_linear(volume_db);
  if (linear < 0.0f) linear = 0.0f;
  if (linear > 1.0f) linear = 1.0f;
  return (int32_t)(linear * 32768.0f);
}

static float volume_db_to_linear(float db) {
  return powf(10.0f, db / 20.0f);
}
