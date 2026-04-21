#include "app_core.h"

#include "airplay_service.h"
#include "audio/audio_stream.h"
#include "nvs_flash.h"
#include "receiver_state.h"
#include "screen_ui.h"
#include "settings.h"
#include "wifi.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "app_core";
static const int64_t RESOURCE_LOG_INTERVAL_US = 30000000LL;

static bool s_airplay_started = false;

static screen_ui_state_t map_screen_state(receiver_state_t state) {
  switch (state) {
  case RECEIVER_STATE_BOOT:
    return SCREEN_UI_STATE_BOOT;
  case RECEIVER_STATE_CONFIG_REQUIRED:
    return SCREEN_UI_STATE_CONFIG_REQUIRED;
  case RECEIVER_STATE_NETWORK_READY:
    return SCREEN_UI_STATE_NETWORK_READY;
  case RECEIVER_STATE_DISCOVERABLE:
    return SCREEN_UI_STATE_DISCOVERABLE;
  case RECEIVER_STATE_SESSION_ESTABLISHING:
    return SCREEN_UI_STATE_SESSION_ESTABLISHING;
  case RECEIVER_STATE_STREAMING:
    return SCREEN_UI_STATE_STREAMING;
  case RECEIVER_STATE_RECOVERING:
    return SCREEN_UI_STATE_RECOVERING;
  case RECEIVER_STATE_FAULT:
  default:
    return SCREEN_UI_STATE_FAULT;
  }
}

static void push_screen_ui_state(void) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);

  screen_ui_set_state(
      map_screen_state(snapshot.state), snapshot.network_ready,
      snapshot.discoverable || snapshot.session_establishing || snapshot.streaming,
      snapshot.streaming);
}

static void log_runtime_resources(const char *reason) {
  ESP_LOGI(TAG,
           "resource snapshot[%s]: internal_free=%lu internal_largest=%lu "
           "spiram_free=%lu",
           reason,
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static esp_err_t start_airplay_services(void) {
  if (s_airplay_started) {
    return ESP_OK;
  }

  esp_err_t err = airplay_service_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AirPlay service start failed: %s", esp_err_to_name(err));
    receiver_state_set_recovering(true);
    return err;
  }

  s_airplay_started = true;
  receiver_state_set_recovering(false);
  ESP_LOGI(TAG, "AirPlay ready");
  return ESP_OK;
}

static void stop_airplay_services(void) {
  if (!s_airplay_started) {
    return;
  }

  airplay_service_stop();
  s_airplay_started = false;
  ESP_LOGI(TAG, "AirPlay stopped");
}

static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;

  bool had_network = wifi_is_connected();
  int64_t last_resource_log_us = esp_timer_get_time();
  receiver_state_set_network_ready(had_network);
  push_screen_ui_state();
  log_runtime_resources(had_network ? "network_monitor_start_online"
                                    : "network_monitor_start_idle");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool has_network = wifi_is_connected();
    receiver_state_set_network_ready(has_network);
    push_screen_ui_state();

    if (has_network && !had_network) {
      receiver_state_set_config_required(false);
      receiver_state_set_recovering(false);
      start_airplay_services();
      log_runtime_resources("wifi_connected");
    } else if (!has_network && had_network) {
      receiver_state_set_recovering(true);
      stop_airplay_services();
      log_runtime_resources("wifi_disconnected");
    } else if (has_network && !s_airplay_started) {
      start_airplay_services();
    }

    int64_t now_us = esp_timer_get_time();
    if ((now_us - last_resource_log_us) >= RESOURCE_LOG_INTERVAL_US) {
      log_runtime_resources(has_network ? "periodic_online" : "periodic_idle");
      last_resource_log_us = now_us;
    }

    had_network = has_network;
  }
}

static esp_err_t init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ret = nvs_flash_erase();
    if (ret == ESP_OK) {
      ret = nvs_flash_init();
    }
  }
  return ret;
}

static esp_err_t provision_wifi_credentials_if_needed(void) {
#if CONFIG_WIFI_CREDENTIALS_COMPILE_TIME
  if (strlen(CONFIG_WIFI_SSID) == 0) {
    return ESP_OK;
  }

  esp_err_t err =
      settings_set_wifi_credentials(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to provision compile-time Wi-Fi credentials: %s",
             esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "Provisioned compile-time Wi-Fi credentials into NVS");
#endif
  return ESP_OK;
}

void app_core_run(void) {
  receiver_state_init();
  receiver_state_dispatch(RECEIVER_EVENT_BOOT);
  if (screen_ui_init() != ESP_OK) {
    ESP_LOGW(TAG, "screen UI init failed; continuing without local display");
  }
  push_screen_ui_state();
  log_runtime_resources("boot");

  esp_err_t ret = init_nvs();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    receiver_state_set_faulted(true);
    push_screen_ui_state();
    return;
  }

  ret = settings_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "settings init failed: %s", esp_err_to_name(ret));
    receiver_state_set_faulted(true);
    push_screen_ui_state();
    return;
  }

  ret = provision_wifi_credentials_if_needed();
  if (ret != ESP_OK) {
    receiver_state_set_faulted(true);
    push_screen_ui_state();
    return;
  }

  ret = audio_realtime_preallocate();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "audio preallocation failed: %s", esp_err_to_name(ret));
  }
  log_runtime_resources("post_audio_prealloc");

  if (!settings_has_wifi_credentials()) {
    ESP_LOGW(TAG, "No Wi-Fi credentials configured; staying idle");
    receiver_state_set_config_required(true);
    receiver_state_set_network_ready(false);
    push_screen_ui_state();
    log_runtime_resources("idle_no_credentials");
  } else {
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(ret));
      receiver_state_set_faulted(true);
      push_screen_ui_state();
      return;
    }

    if (wifi_wait_connected(30000)) {
      receiver_state_set_network_ready(true);
      start_airplay_services();
      push_screen_ui_state();
      log_runtime_resources("startup_connected");
    } else {
      receiver_state_set_recovering(true);
      push_screen_ui_state();
      log_runtime_resources("startup_wait_connect_timeout");
    }

    BaseType_t ok =
        xTaskCreate(network_monitor_task, "net_mon", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
      ESP_LOGE(TAG, "Failed to start network monitor");
      receiver_state_set_faulted(true);
      push_screen_ui_state();
      return;
    }
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
