#include "wifi_credentials.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "net_wifi_creds";

#define NVS_NAMESPACE "airplay"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

esp_err_t wifi_creds_get_ssid(char *ssid, size_t len) {
  if (!ssid || len == 0) return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
  size_t req = len;
  err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &req);
  nvs_close(nvs);
  if (err == ESP_OK && req > len) return ESP_ERR_NVS_INVALID_LENGTH;
  return err;
}

esp_err_t wifi_creds_get_password(char *password, size_t len) {
  if (!password || len == 0) return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
  size_t req = len;
  err = nvs_get_str(nvs, NVS_KEY_PASS, password, &req);
  nvs_close(nvs);
  if (err == ESP_OK && req > len) return ESP_ERR_NVS_INVALID_LENGTH;
  return err;
}

esp_err_t wifi_creds_save(const char *ssid, const char *password) {
  if (!ssid || strlen(ssid) == 0 || strlen(ssid) > MAX_SSID_LEN)
    return ESP_ERR_INVALID_ARG;
  if (!password || strlen(password) > MAX_PASS_LEN)
    return ESP_ERR_INVALID_ARG;

  char old_ssid[MAX_SSID_LEN + 1] = {0};
  char old_pass[MAX_PASS_LEN + 1] = {0};
  if (wifi_creds_get_ssid(old_ssid, sizeof(old_ssid)) == ESP_OK &&
      wifi_creds_get_password(old_pass, sizeof(old_pass)) == ESP_OK &&
      strcmp(old_ssid, ssid) == 0 && strcmp(old_pass, password) == 0) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return err; }
  err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
  if (err == ESP_OK) err = nvs_set_str(nvs, NVS_KEY_PASS, password);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err == ESP_OK) ESP_LOGI(TAG, "saved: SSID=%s", ssid);
  else ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
  return err;
}

esp_err_t wifi_creds_clear(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return err; }
  esp_err_t e1 = nvs_erase_key(nvs, NVS_KEY_SSID);
  if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) { nvs_close(nvs); return e1; }
  esp_err_t e2 = nvs_erase_key(nvs, NVS_KEY_PASS);
  if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) { nvs_close(nvs); return e2; }
  err = nvs_commit(nvs);
  nvs_close(nvs);
  if (err == ESP_OK) ESP_LOGI(TAG, "cleared");
  return err;
}

bool wifi_creds_have(void) {
  char buf[MAX_SSID_LEN + 1];
  return wifi_creds_get_ssid(buf, sizeof(buf)) == ESP_OK;
}
