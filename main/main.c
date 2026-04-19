#include "airplay_service.h"
#include "audio/audio_stream.h"
#include "buttons.h"
#include "display.h"
#include "dns_server.h"
#include "ethernet.h"
#include "led.h"
#include "nvs_flash.h"
#include "playback_control.h"
#include "receiver_state.h"
#include "settings.h"
#include "spiram_task.h"
#include "status_service.h"
#include "network/structured_trace.h"
#include "usb_control_service.h"
#include "web_server.h"
#include "wifi.h"
#include "spiffs_storage.h"

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
#include "log_stream.h"
#endif

#if defined(CONFIG_BOARD_M5STACK_CORES3)
#include "iot_board.h"
#endif

#if defined(CONFIG_BT_A2DP_ENABLE) && !CONFIG_PRODUCT_AIRPLAY_ONLY
#include "a2dp_sink.h"
#include "rtsp/rtsp_events.h"
#endif

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define AP_IP_ADDR 0x0104A8C0

static bool s_airplay_started = false;
#if defined(CONFIG_BOARD_M5STACK_CORES3)
static bool s_board_ready = false;
#endif

static esp_err_t ensure_board_ready(void) {
#if defined(CONFIG_BOARD_M5STACK_CORES3)
  if (s_board_ready) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "board init start: %s", iot_board_get_info());
  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    return err;
  }
  s_board_ready = true;
  ESP_LOGI(TAG, "board init ok");
#endif
  return ESP_OK;
}

static esp_err_t ensure_airplay_prereqs(void) {
  return ensure_board_ready();
}

static esp_err_t start_airplay_services(void) {
  if (s_airplay_started) {
    return ESP_OK;
  }

  esp_err_t err = ensure_airplay_prereqs();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "AirPlay prerequisites unavailable: %s",
             esp_err_to_name(err));
    return err;
  }

  err = airplay_service_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AirPlay service start failed: %s", esp_err_to_name(err));
    receiver_state_set_recovering(true);
    return err;
  }

  s_airplay_started = true;
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
  ESP_LOGI(TAG, "AirPlay ready");
  return ESP_OK;
}

static void stop_airplay_services(void) {
  if (!s_airplay_started) {
    return;
  }

  airplay_service_stop();
  s_airplay_started = false;
  playback_control_set_source(PLAYBACK_SOURCE_NONE);
  ESP_LOGI(TAG, "AirPlay stopped");
}

static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;
  bool had_network = ethernet_is_connected() || wifi_is_connected();
  bool had_eth = ethernet_is_connected();
  bool dns_running = false;
  bool wifi_started = wifi_is_connected() || !ethernet_is_connected();

  if (wifi_is_ap_enabled()) {
    dns_server_start(AP_IP_ADDR);
    dns_running = true;
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool eth_up = ethernet_is_connected();
    bool wifi_up = wifi_is_connected();
    bool ap_up = wifi_is_ap_enabled();
    bool has_network = eth_up || wifi_up;

    if (eth_up && !had_eth && wifi_started) {
      ESP_LOGI(TAG, "Ethernet connected, stopping WiFi fallback");
      wifi_stop();
      wifi_started = false;
      wifi_up = false;
    }

    if (!eth_up && had_eth) {
      ESP_LOGI(TAG, "Ethernet lost, restoring WiFi fallback");
      if (wifi_init_apsta(NULL, NULL) == ESP_OK) {
        wifi_started = true;
        status_service_note_reconnect();
      } else {
        receiver_state_set_faulted(true);
      }
    }

    receiver_state_set_setup_ap_enabled(ap_up);
    receiver_state_set_network_ready(has_network);

    if (has_network && !had_network) {
      receiver_state_set_recovering(false);
      status_service_note_reconnect();
      start_airplay_services();
    } else if (!has_network && had_network) {
      receiver_state_set_recovering(true);
      stop_airplay_services();
    } else if (has_network && !s_airplay_started) {
      start_airplay_services();
    }

    if (ap_up && !dns_running) {
      dns_server_start(AP_IP_ADDR);
      dns_running = true;
    } else if (!ap_up && dns_running) {
      dns_server_stop();
      dns_running = false;
    }

    had_eth = eth_up;
    had_network = has_network;
  }
}

#if defined(CONFIG_BT_A2DP_ENABLE) && !CONFIG_PRODUCT_AIRPLAY_ONLY
static void on_bt_state_changed(bool connected) {
  if (connected) {
    stop_airplay_services();
    playback_control_set_source(PLAYBACK_SOURCE_BLUETOOTH);
  } else {
    playback_control_set_source(PLAYBACK_SOURCE_NONE);
    if (ethernet_is_connected() || wifi_is_connected()) {
      start_airplay_services();
    }
  }
}

static void on_airplay_client_event(rtsp_event_t event,
                                    const rtsp_event_data_t *data,
                                    void *user_data) {
  (void)data;
  (void)user_data;
  if (bt_a2dp_sink_is_connected()) {
    return;
  }

  if (event == RTSP_EVENT_CLIENT_CONNECTED) {
    bt_a2dp_sink_set_discoverable(false);
  } else if (event == RTSP_EVENT_DISCONNECTED) {
    bt_a2dp_sink_set_discoverable(true);
  }
}
#endif

void app_main(void) {
  receiver_state_init();
  receiver_state_dispatch(RECEIVER_EVENT_BOOT);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ret = nvs_flash_erase();
    if (ret == ESP_OK) {
      ret = nvs_flash_init();
    }
  }
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

  spiffs_storage_init();

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
  ret = log_stream_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "log stream init failed: %s", esp_err_to_name(ret));
  }
#endif

  ret = structured_trace_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "structured trace init failed: %s", esp_err_to_name(ret));
  }
  structured_trace_emit_simple("system", "boot", ESP_OK);

  ret = status_service_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "status service init failed: %s", esp_err_to_name(ret));
  }

  ret = usb_control_service_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "usb control init failed: %s", esp_err_to_name(ret));
  }

  ret = usb_control_service_start();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "usb control start failed: %s", esp_err_to_name(ret));
  }

  ret = playback_control_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "playback control init failed: %s", esp_err_to_name(ret));
    receiver_state_set_faulted(true);
    return;
  }

  led_init();

#if defined(CONFIG_DISPLAY_ENABLED) && CONFIG_DISPLAY_ENABLED
#if defined(CONFIG_DISPLAY_BUS_SPI)
  display_init(iot_board_get_handle(BOARD_SPI_DISP_ID));
#else
  display_init(iot_board_get_handle(BOARD_I2C_DISP_ID));
#endif
#endif

  ret = audio_realtime_preallocate();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "audio preallocation failed: %s", esp_err_to_name(ret));
  }

  bool eth_available = false;
  esp_err_t err = ethernet_init();
  if (err == ESP_OK) {
    for (int i = 0; i < 25 && !ethernet_is_link_up(); i++) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ethernet_is_link_up() && !ethernet_is_connected()) {
      for (int i = 0; i < 50 && !ethernet_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
    eth_available = ethernet_is_connected();
  } else if (err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
  }

  if (!eth_available) {
    ret = wifi_init_apsta(NULL, NULL);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
      receiver_state_set_faulted(true);
      structured_trace_emit_simple("network", "wifi_init_failed", ret);
    }

    receiver_state_set_setup_ap_enabled(wifi_is_ap_enabled());
    if (ret == ESP_OK && settings_has_wifi_credentials()) {
      if (!wifi_wait_connected(30000)) {
        ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
      }
    } else {
      ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
    }
  }

  web_server_start(80);
  task_create_spiram(network_monitor_task, "net_mon", 4096, NULL, 5, NULL,
                     NULL);

  bool connected = eth_available || wifi_is_connected();
  receiver_state_set_network_ready(connected);
  receiver_state_set_setup_ap_enabled(wifi_is_ap_enabled());
  structured_trace_emit_simple("network", connected ? "network_up" : "network_down",
                               ESP_OK);
  if (connected) {
    start_airplay_services();
  }

#if defined(CONFIG_BT_A2DP_ENABLE) && !CONFIG_PRODUCT_AIRPLAY_ONLY
  {
    char bt_name[65];
    settings_get_device_name(bt_name, sizeof(bt_name));
    esp_err_t bt_err = bt_a2dp_sink_init(bt_name, on_bt_state_changed);
    if (bt_err == ESP_OK) {
      rtsp_events_register(on_airplay_client_event, NULL);
    }
  }
#endif

  buttons_init();

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
