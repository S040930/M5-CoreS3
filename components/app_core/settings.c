#include "settings.h"

#include "audio_volume.h"
#include "config_hash.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_credentials.h"

#include <string.h>

static const char *TAG = "app_settings";

#define NVS_NAMESPACE "airplay"

static char s_device_name[65] = SETTINGS_DEFAULT_DEVICE_NAME;

static char s_voice_url[256] = {0};
static char s_voice_model[128] = {0};
static char s_voice_api_key[128] = {0};
static bool s_voice_loaded = false;

static esp_err_t persist_voice_str(const char *key, const char *value) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_str(nvs, key, value != NULL ? value : "");
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  return err;
}

esp_err_t settings_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
      err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    return err;
  }

  nvs_handle_t nvs;
  err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "NVS namespace not found, using defaults");
    return ESP_OK;
  }

  size_t len = sizeof(s_device_name);
  esp_err_t name_err = nvs_get_str(nvs, "device_name", s_device_name, &len);
  if (name_err == ESP_OK)
    ESP_LOGI(TAG, "device name: %s", s_device_name);

  len = sizeof(s_voice_url);
  if (nvs_get_str(nvs, "voice_url", s_voice_url, &len) == ESP_OK)
    s_voice_loaded = true;
  len = sizeof(s_voice_model);
  if (nvs_get_str(nvs, "voice_model", s_voice_model, &len) == ESP_OK)
    s_voice_loaded = true;
  len = sizeof(s_voice_api_key);
  if (nvs_get_str(nvs, "voice_api_key", s_voice_api_key, &len) == ESP_OK)
    s_voice_loaded = true;

  nvs_close(nvs);
  return ESP_OK;
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (!name || len == 0) return ESP_ERR_INVALID_ARG;
  strncpy(name, s_device_name, len - 1);
  name[len - 1] = '\0';
  return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name) {
  if (!name) return ESP_ERR_INVALID_ARG;
  strncpy(s_device_name, name, sizeof(s_device_name) - 1);
  s_device_name[sizeof(s_device_name) - 1] = '\0';

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  err = nvs_set_str(nvs, "device_name", s_device_name);
  if (err == ESP_OK) nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

esp_err_t settings_get_void_codec(char *codec, size_t len) {
  if (!codec || len < 5) return ESP_ERR_INVALID_ARG;
  strcpy(codec, "alac");
  return ESP_OK;
}

esp_err_t settings_get_voice_url(char *url, size_t len) {
  if (!url || len == 0) return ESP_ERR_INVALID_ARG;
  strncpy(url, s_voice_url, len - 1);
  url[len - 1] = '\0';
  return strlen(s_voice_url) > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t settings_get_voice_model(char *model, size_t len) {
  if (!model || len == 0) return ESP_ERR_INVALID_ARG;
  strncpy(model, s_voice_model, len - 1);
  model[len - 1] = '\0';
  return strlen(s_voice_model) > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t settings_get_voice_api_key(char *api_key, size_t len) {
  if (!api_key || len == 0) return ESP_ERR_INVALID_ARG;
  strncpy(api_key, s_voice_api_key, len - 1);
  api_key[len - 1] = '\0';
  return strlen(s_voice_api_key) > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t settings_set_voice_url(const char *url) {
  if (!url) return ESP_ERR_INVALID_ARG;
  strncpy(s_voice_url, url, sizeof(s_voice_url) - 1);
  s_voice_url[sizeof(s_voice_url) - 1] = '\0';
  s_voice_loaded = true;
  return persist_voice_str("voice_url", s_voice_url);
}

esp_err_t settings_set_voice_model(const char *model) {
  if (!model) return ESP_ERR_INVALID_ARG;
  strncpy(s_voice_model, model, sizeof(s_voice_model) - 1);
  s_voice_model[sizeof(s_voice_model) - 1] = '\0';
  s_voice_loaded = true;
  return persist_voice_str("voice_model", s_voice_model);
}

esp_err_t settings_set_voice_api_key(const char *api_key) {
  if (!api_key) return ESP_ERR_INVALID_ARG;
  strncpy(s_voice_api_key, api_key, sizeof(s_voice_api_key) - 1);
  s_voice_api_key[sizeof(s_voice_api_key) - 1] = '\0';
  s_voice_loaded = true;
  return persist_voice_str("voice_api_key", s_voice_api_key);
}

bool settings_has_voice_config(void) {
  return s_voice_loaded;
}
