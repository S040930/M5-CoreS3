#include "app_core.h"

#include "airplay_service.h"
#include "audio/audio_stream.h"
#include "nvs_flash.h"
#include "receiver_state.h"
#include "settings.h"
#include "wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_core";

static bool s_airplay_started = false;

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
  receiver_state_set_network_ready(had_network);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool has_network = wifi_is_connected();
    receiver_state_set_network_ready(has_network);

    if (has_network && !had_network) {
      receiver_state_set_config_required(false);
      receiver_state_set_recovering(false);
      start_airplay_services();
    } else if (!has_network && had_network) {
      receiver_state_set_recovering(true);
      stop_airplay_services();
    } else if (has_network && !s_airplay_started) {
      start_airplay_services();
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

void app_core_run(void) {
  receiver_state_init();
  receiver_state_dispatch(RECEIVER_EVENT_BOOT);

  esp_err_t ret = init_nvs();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    receiver_state_set_faulted(true);
    return;
  }

  ret = settings_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "settings init failed: %s", esp_err_to_name(ret));
    receiver_state_set_faulted(true);
    return;
  }

  ret = audio_realtime_preallocate();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "audio preallocation failed: %s", esp_err_to_name(ret));
  }

  if (!settings_has_wifi_credentials()) {
    ESP_LOGW(TAG, "No Wi-Fi credentials configured; staying idle");
    receiver_state_set_config_required(true);
    receiver_state_set_network_ready(false);
  } else {
    ret = wifi_init_sta();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(ret));
      receiver_state_set_faulted(true);
      return;
    }

    if (wifi_wait_connected(30000)) {
      receiver_state_set_network_ready(true);
      start_airplay_services();
    } else {
      receiver_state_set_recovering(true);
    }

    BaseType_t ok =
        xTaskCreate(network_monitor_task, "net_mon", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
      ESP_LOGE(TAG, "Failed to start network monitor");
      receiver_state_set_faulted(true);
      return;
    }
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
