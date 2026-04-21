#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "sdkconfig.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define WIFI_FAST_RETRY_LIMIT 5

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_sta_connected = false;
static int s_retry_num = 0;
static esp_timer_handle_t s_retry_timer = NULL;

static const char *wifi_disconnect_reason_to_str(uint8_t reason) {
  switch (reason) {
  case 1:
    return "unspecified";
  case 2:
    return "auth expired";
  case 3:
    return "auth leave";
  case 4:
    return "assoc expired";
  case 5:
    return "assoc too many";
  case 6:
    return "not authed";
  case 7:
    return "not assoced";
  case 8:
    return "assoc leave";
  case 15:
    return "4-way handshake timeout";
  case 200:
    return "beacon timeout";
  case 201:
    return "no ap found";
  case 202:
    return "auth fail";
  case 203:
    return "assoc fail";
  case 204:
    return "handshake timeout";
  case 205:
    return "connection fail";
  default:
    return "unknown";
  }
}

static void retry_timer_callback(void *arg) {
  (void)arg;
  if (!s_sta_connected) {
    ESP_LOGI(TAG, "Retry timer fired, reconnecting (attempt %d)...",
             s_retry_num + 1);
    esp_wifi_connect();
  }
}

static void schedule_retry(void) {
  int delay_s = 5;
  if (s_retry_num > WIFI_FAST_RETRY_LIMIT) {
    int backoff_count = s_retry_num - WIFI_FAST_RETRY_LIMIT;
    delay_s = 5 * (1 << (backoff_count > 3 ? 3 : backoff_count));
    if (delay_s > 30) {
      delay_s = 30;
    }
  }
  ESP_LOGI(TAG, "Scheduling retry in %d seconds", delay_s);
  esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  (void)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_sta_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    wifi_event_sta_disconnected_t *disconnected =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGI(TAG, "Disconnected from AP, reason: %d (%s)", disconnected->reason,
             wifi_disconnect_reason_to_str(disconnected->reason));

    s_retry_num++;
    if (s_retry_num <= WIFI_FAST_RETRY_LIMIT) {
      ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_num,
               WIFI_FAST_RETRY_LIMIT);
      esp_wifi_connect();
    } else {
      schedule_retry();
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    s_sta_connected = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
  }
}

static esp_err_t wifi_init_base(void) {
  if (s_wifi_initialized) {
    return ESP_OK;
  }

  s_wifi_event_group = xEventGroupCreate();
  if (!s_wifi_event_group) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    return ret;
  }

  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    return ret;
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ret = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id);
  if (ret != ESP_OK) {
    return ret;
  }
  ret = esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip);
  if (ret != ESP_OK) {
    return ret;
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ret = esp_wifi_init(&cfg);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = esp_wifi_set_ps(WIFI_PS_NONE);
  if (ret != ESP_OK) {
    return ret;
  }

  const esp_timer_create_args_t timer_args = {
      .callback = retry_timer_callback,
      .name = "wifi_retry",
  };
  ret = esp_timer_create(&timer_args, &s_retry_timer);
  if (ret != ESP_OK) {
    return ret;
  }

  s_wifi_initialized = true;
  return ESP_OK;
}

esp_err_t wifi_init_sta(void) {
  esp_err_t ret = wifi_init_base();
  if (ret != ESP_OK) {
    return ret;
  }

  if (!s_sta_netif) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
      return ESP_ERR_NO_MEM;
    }
  }

  char ssid[33] = {0};
  char password[65] = {0};
  bool has_credentials = false;

#if CONFIG_WIFI_CREDENTIALS_COMPILE_TIME
  // Use compile-time credentials from sdkconfig, overriding NVS
  if (strlen(CONFIG_WIFI_SSID) > 0) {
    strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
    strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
    has_credentials = true;
    ESP_LOGI(TAG, "Using compile-time WiFi credentials for SSID: %s", ssid);
    
    // Save to NVS so they persist if compile-time override is disabled later
    settings_set_wifi_credentials(ssid, password);
  }
#else
  // Use NVS stored credentials
  if (settings_get_wifi_ssid(ssid, sizeof(ssid)) == ESP_OK &&
      settings_get_wifi_password(password, sizeof(password)) == ESP_OK &&
      strlen(ssid) > 0) {
    has_credentials = true;
  }
#endif

  if (!has_credentials) {
    ESP_LOGW(TAG, "No saved STA credentials found");
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
      return ret;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
      return ret;
    }
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    return ESP_ERR_NOT_FOUND;
  }

  wifi_config_t sta_config = {0};
  strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
  strncpy((char *)sta_config.sta.password, password,
          sizeof(sta_config.sta.password) - 1);
  sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
  sta_config.sta.pmf_cfg.capable = true;
  sta_config.sta.pmf_cfg.required = false;
  sta_config.sta.listen_interval = 1;
  sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  s_retry_num = 0;
  s_sta_connected = false;

  ret = esp_wifi_set_mode(WIFI_MODE_STA);
  if (ret != ESP_OK) {
    return ret;
  }
  ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
  if (ret != ESP_OK) {
    return ret;
  }

  ESP_LOGI(TAG, "Connecting STA to '%s'", ssid);
  return esp_wifi_start();
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (!s_wifi_event_group) {
    return false;
  }

  TickType_t timeout_ticks =
      timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                          pdFALSE, pdFALSE, timeout_ticks);

  if (bits & WIFI_CONNECTED_BIT) {
    return true;
  }
  if (bits & WIFI_FAIL_BIT) {
    ESP_LOGW(TAG, "WiFi connection unavailable");
  }
  return false;
}

void wifi_get_mac_str(char *mac_str, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(mac_str, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

bool wifi_is_connected(void) {
  return s_sta_connected;
}

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  if (!s_sta_netif || !ip_str || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  if (err == ESP_OK) {
    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
  }
  return err;
}

void wifi_stop(void) {
  if (!s_wifi_initialized) {
    return;
  }

  esp_timer_stop(s_retry_timer);
  esp_wifi_stop();
  esp_wifi_deinit();
  s_wifi_initialized = false;
  s_sta_connected = false;
  s_retry_num = 0;
  if (s_wifi_event_group) {
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  }
}
