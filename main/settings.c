#include "settings.h"

#include "dac.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_random.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "settings";

#define NVS_NAMESPACE  "airplay"
#define NVS_KEY_VOLUME "volume_db"
#ifdef CONFIG_BT_A2DP_ENABLE
#define NVS_KEY_BT_VOLUME "bt_vol"
#endif
#define NVS_KEY_WIFI_SSID     "wifi_ssid"
#define NVS_KEY_WIFI_PASSWORD "wifi_pass"
#define NVS_KEY_DEVICE_NAME   "device_name"
#define NVS_KEY_EQ_GAINS      "eq_gains"
#define NVS_KEY_WEB_SECRET    "web_secret"

#define MAX_WIFI_SSID_LEN     32
#define MAX_WIFI_PASSWORD_LEN 64
#define MAX_DEVICE_NAME_LEN   64
#define MANAGEMENT_SECRET_BYTES 16
#define MANAGEMENT_SECRET_LEN   (MANAGEMENT_SECRET_BYTES * 2)

// Cached values  (defaults = 50 %)
static float g_volume_db = -15.0f;
static bool g_volume_loaded = false;

#ifdef CONFIG_BT_A2DP_ENABLE
static uint8_t g_bt_volume = 64; /* default: 50 % */
static bool g_bt_volume_loaded = false;
#endif

static float g_eq_gains[SETTINGS_EQ_BANDS];
static bool g_eq_loaded = false;
static char g_management_secret[MANAGEMENT_SECRET_LEN + 1];

static void bytes_to_hex(const uint8_t *bytes, size_t byte_len, char *out,
                         size_t out_len) {
  static const char hex[] = "0123456789abcdef";
  size_t needed = byte_len * 2 + 1;
  if (!bytes || !out || out_len < needed) {
    return;
  }

  for (size_t i = 0; i < byte_len; i++) {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 0x0F];
  }
  out[byte_len * 2] = '\0';
}

static esp_err_t ensure_management_secret(nvs_handle_t nvs) {
  size_t secret_len = sizeof(g_management_secret);
  esp_err_t err =
      nvs_get_str(nvs, NVS_KEY_WEB_SECRET, g_management_secret, &secret_len);
  if (err == ESP_OK && secret_len == sizeof(g_management_secret)) {
    return ESP_OK;
  }

  uint8_t random_bytes[MANAGEMENT_SECRET_BYTES];
  esp_fill_random(random_bytes, sizeof(random_bytes));
  bytes_to_hex(random_bytes, sizeof(random_bytes), g_management_secret,
               sizeof(g_management_secret));

  err = nvs_set_str(nvs, NVS_KEY_WEB_SECRET, g_management_secret);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Generated persistent web management secret");
  }
  return err;
}

esp_err_t settings_init(void) {
  // Load volume on init
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    int32_t vol_fixed;
    err = nvs_get_i32(nvs, NVS_KEY_VOLUME, &vol_fixed);
    if (err == ESP_OK) {
      g_volume_db = (float)vol_fixed / 100.0f;
      g_volume_loaded = true;
      ESP_LOGI(TAG, "Loaded volume: %.2f dB", g_volume_db);
    }

    /* Load EQ gains blob */
    size_t eq_size = sizeof(g_eq_gains);
    err = nvs_get_blob(nvs, NVS_KEY_EQ_GAINS, g_eq_gains, &eq_size);
    if (err == ESP_OK && eq_size == sizeof(g_eq_gains)) {
      g_eq_loaded = true;
      ESP_LOGI(TAG, "Loaded EQ gains (%d bands)", SETTINGS_EQ_BANDS);
    }

    nvs_close(nvs);
  }

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    err = ensure_management_secret(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize web management secret: %s",
               esp_err_to_name(err));
      return err;
    }
  } else {
    ESP_LOGE(TAG, "Failed to open NVS for management secret: %s",
             esp_err_to_name(err));
    return err;
  }

#if CONFIG_WIFI_FACTORY_DEFAULT_ENABLE
  if (strlen(CONFIG_WIFI_FACTORY_SSID) > 0 && !settings_has_wifi_credentials()) {
    esp_err_t werr = settings_set_wifi_credentials(CONFIG_WIFI_FACTORY_SSID,
                                                 CONFIG_WIFI_FACTORY_PASSWORD);
    if (werr == ESP_OK) {
      ESP_LOGI(TAG, "Seeded factory WiFi from Kconfig (first boot / empty NVS)");
    } else {
      ESP_LOGW(TAG, "Factory WiFi seed failed: %s", esp_err_to_name(werr));
    }
  }
#endif

  return ESP_OK;
}

esp_err_t settings_get_volume(float *volume_db) {
  if (!volume_db) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!g_volume_loaded) {
    return ESP_ERR_NOT_FOUND;
  }

  *volume_db = g_volume_db;
  return ESP_OK;
}

esp_err_t settings_set_volume(float volume_db) {
  // Skip if unchanged
  if (g_volume_loaded && volume_db == g_volume_db) {
    return ESP_OK;
  }

  dac_set_volume(volume_db);

  g_volume_db = volume_db;
  g_volume_loaded = true;
  return ESP_OK;
}

esp_err_t settings_persist_volume(void) {
  if (!g_volume_loaded) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  int32_t vol_fixed = (int32_t)(g_volume_db * 100.0f);
  err = nvs_set_i32(nvs, NVS_KEY_VOLUME, vol_fixed);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Persisted volume: %.2f dB", g_volume_db);
  } else {
    ESP_LOGE(TAG, "Failed to persist volume: %s", esp_err_to_name(err));
  }

  return err;
}

#ifdef CONFIG_BT_A2DP_ENABLE
esp_err_t settings_get_bt_volume(uint8_t *volume) {
  if (!volume) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!g_bt_volume_loaded) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
      return err;
    }
    err = nvs_get_u8(nvs, NVS_KEY_BT_VOLUME, &g_bt_volume);
    nvs_close(nvs);
    if (err != ESP_OK) {
      return err;
    }
    g_bt_volume_loaded = true;
  }
  *volume = g_bt_volume;
  return ESP_OK;
}

esp_err_t settings_set_bt_volume(uint8_t volume) {
  if (g_bt_volume_loaded && volume == g_bt_volume) {
    return ESP_OK;
  }

  g_bt_volume = volume;
  g_bt_volume_loaded = true;
  return ESP_OK;
}

esp_err_t settings_persist_bt_volume(void) {
  if (!g_bt_volume_loaded) {
    return ESP_OK;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_u8(nvs, NVS_KEY_BT_VOLUME, g_bt_volume);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Persisted BT volume: %d/127", g_bt_volume);
  } else {
    ESP_LOGE(TAG, "Failed to persist BT volume: %s", esp_err_to_name(err));
  }
  return err;
}
#endif

esp_err_t settings_get_wifi_ssid(char *ssid, size_t len) {
  if (!ssid || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  size_t required_size = len;
  err = nvs_get_str(nvs, NVS_KEY_WIFI_SSID, ssid, &required_size);
  nvs_close(nvs);

  if (err == ESP_OK && required_size > len) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  return err;
}

esp_err_t settings_get_wifi_password(char *password, size_t len) {
  if (!password || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return ESP_ERR_NOT_FOUND;
  }

  size_t required_size = len;
  err = nvs_get_str(nvs, NVS_KEY_WIFI_PASSWORD, password, &required_size);
  nvs_close(nvs);

  if (err == ESP_OK && required_size > len) {
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  return err;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  if (!ssid || strlen(ssid) == 0 || strlen(ssid) > MAX_WIFI_SSID_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!password || strlen(password) > MAX_WIFI_PASSWORD_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
  if (err == ESP_OK) {
    err = nvs_set_str(nvs, NVS_KEY_WIFI_PASSWORD, password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved WiFi credentials: SSID=%s", ssid);
  } else {
    ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
  }

  return err;
}

esp_err_t settings_clear_wifi_credentials(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  esp_err_t erase_ssid_err = nvs_erase_key(nvs, NVS_KEY_WIFI_SSID);
  if (erase_ssid_err != ESP_OK && erase_ssid_err != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(nvs);
    ESP_LOGE(TAG, "Failed to erase WiFi SSID: %s",
             esp_err_to_name(erase_ssid_err));
    return erase_ssid_err;
  }

  esp_err_t erase_pass_err = nvs_erase_key(nvs, NVS_KEY_WIFI_PASSWORD);
  if (erase_pass_err != ESP_OK && erase_pass_err != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(nvs);
    ESP_LOGE(TAG, "Failed to erase WiFi password: %s",
             esp_err_to_name(erase_pass_err));
    return erase_pass_err;
  }

  err = nvs_commit(nvs);
  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Cleared saved WiFi credentials");
  } else {
    ESP_LOGE(TAG, "Failed to clear WiFi credentials: %s",
             esp_err_to_name(err));
  }

  return err;
}

bool settings_has_wifi_credentials(void) {
  char ssid[MAX_WIFI_SSID_LEN + 1];
  return settings_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK;
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (!name || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t required_size = len;
    err = nvs_get_str(nvs, NVS_KEY_DEVICE_NAME, name, &required_size);
    nvs_close(nvs);

    if (err == ESP_OK && required_size <= len) {
      return ESP_OK;
    }
  }

  // Return default if not found or error
  strncpy(name, SETTINGS_DEFAULT_DEVICE_NAME, len - 1);
  name[len - 1] = '\0';
  return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name) {
  if (!name || strlen(name) == 0 || strlen(name) > MAX_DEVICE_NAME_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs, NVS_KEY_DEVICE_NAME, name);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Saved device name: %s", name);
  } else {
    ESP_LOGE(TAG, "Failed to save device name: %s", esp_err_to_name(err));
  }

  return err;
}

esp_err_t settings_get_management_secret(char *secret, size_t len) {
  if (!secret || len < sizeof(g_management_secret)) {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(secret, g_management_secret, sizeof(g_management_secret));
  return ESP_OK;
}

/* ================================================================== */
/*  EQ Gains                                                           */
/* ================================================================== */

esp_err_t settings_get_eq_gains(float gains_db[SETTINGS_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!g_eq_loaded) {
    return ESP_ERR_NOT_FOUND;
  }

  memcpy(gains_db, g_eq_gains, sizeof(g_eq_gains));
  return ESP_OK;
}

esp_err_t settings_set_eq_gains(const float gains_db[SETTINGS_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Skip write if unchanged (compare element-by-element to avoid
     memcmp on floats, which is flagged by
     bugprone-suspicious-memory-comparison) */
  if (g_eq_loaded) {
    bool unchanged = true;
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      if (gains_db[i] != g_eq_gains[i]) {
        unchanged = false;
        break;
      }
    }
    if (unchanged) {
      return ESP_OK;
    }
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_blob(nvs, NVS_KEY_EQ_GAINS, gains_db,
                     sizeof(float) * SETTINGS_EQ_BANDS);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }

  nvs_close(nvs);

  if (err == ESP_OK) {
    memcpy(g_eq_gains, gains_db, sizeof(g_eq_gains));
    g_eq_loaded = true;
    ESP_LOGI(TAG, "Saved EQ gains (%d bands)", SETTINGS_EQ_BANDS);
  } else {
    ESP_LOGE(TAG, "Failed to save EQ gains: %s", esp_err_to_name(err));
  }

  return err;
}

esp_err_t settings_clear_eq(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_erase_key(nvs, NVS_KEY_EQ_GAINS);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_commit(nvs);
    memset(g_eq_gains, 0, sizeof(g_eq_gains));
    g_eq_loaded = false;
    err = ESP_OK;
  }

  nvs_close(nvs);
  return err;
}

bool settings_has_eq(void) {
  return g_eq_loaded;
}
