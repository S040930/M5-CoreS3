#include "app_core.h"

#include "airplay_service.h"
#include "auto_brightness.h"
#include "bsp/display.h"
#include "iot_board.h"
#include "audio/audio_stream.h"
#include "config_hash.h"
#include "nvs_flash.h"
#include "realtime_voice.h"
#include "receiver_state.h"
#include "env_monitor.h"
#include "resource/resource_manager.h"
#include "screen_ui.h"
#include "sedentary_monitor.h"
#include "settings.h"
#include "wifi.h"
#include "wifi_credentials.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "app_core";
static const int64_t RESOURCE_LOG_INTERVAL_US = 30000000LL;
static StaticTask_t s_net_mon_tcb;
static StackType_t *s_net_mon_stack = NULL;
#define NET_MON_STACK_SIZE 4096
#if CONFIG_FREERTOS_UNICORE
#define NET_MON_TASK_CORE 0
#else
#define NET_MON_TASK_CORE 0
#endif

static void app_core_on_airplay_active(bool active) {
  realtime_voice_on_airplay_state_changed(active);
}

static void app_core_on_resource_event(resource_event_t event, void *ctx) {
  (void)ctx;
  switch (event) {
  case RESOURCE_EVENT_VOICE_STARTED:
    ESP_LOGI(TAG, "resource event: voice started");
    break;
  case RESOURCE_EVENT_VOICE_STOPPED:
    ESP_LOGI(TAG, "resource event: voice stopped");
    break;
  case RESOURCE_EVENT_AIRPLAY_STARTED:
    ESP_LOGI(TAG, "resource event: airplay started");
    break;
  case RESOURCE_EVENT_AIRPLAY_STOPPED:
    ESP_LOGI(TAG, "resource event: airplay stopped");
    break;
  }
}

static screen_ui_voice_state_t realtime_to_screen_voice_state(int state) {
  switch ((realtime_voice_state_t)state) {
  case REALTIME_VOICE_STATE_STANDBY:    return SCREEN_UI_VOICE_STANDBY;
  case REALTIME_VOICE_STATE_CONNECTING: return SCREEN_UI_VOICE_CONNECTING;
  case REALTIME_VOICE_STATE_LISTENING:  return SCREEN_UI_VOICE_LISTENING;
  case REALTIME_VOICE_STATE_SENDING:    return SCREEN_UI_VOICE_SENDING;
  case REALTIME_VOICE_STATE_THINKING:   return SCREEN_UI_VOICE_THINKING;
  case REALTIME_VOICE_STATE_SPEAKING:   return SCREEN_UI_VOICE_SPEAKING;
  case REALTIME_VOICE_STATE_ERROR:      return SCREEN_UI_VOICE_ERROR;
  case REALTIME_VOICE_STATE_OFF:
  default:                              return SCREEN_UI_VOICE_OFF;
  }
}

static void voice_ui_state_bridge(int state, const char *user, const char *assistant, const char *error) {
  screen_ui_set_voice_state(realtime_to_screen_voice_state(state), user, assistant, error);
}

static void voice_ui_network_busy_bridge(bool busy) {
  screen_ui_set_voice_network_busy(busy);
}

static void network_query_for_voice(voice_network_snapshot_t *out) {
  receiver_state_snapshot_t snap;
  receiver_state_get_snapshot(&snap);
  out->faulted = snap.faulted;
  out->config_required = snap.config_required;
  out->recovering = snap.recovering;
  out->network_ready = snap.network_ready;
  out->discoverable = snap.discoverable;
}

static bool s_airplay_started = false;
static bool s_voice_started = false;

static void push_screen_ui_state(void) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);

  screen_ui_set_state(
      receiver_state_map_screen_ui(snapshot.state), snapshot.network_ready,
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
    ESP_LOGI(TAG, "AirPlay start skipped: already started");
    airplay_service_set_active_callback(app_core_on_airplay_active);
    airplay_service_refresh_playback();
    return ESP_OK;
  }

  char device_name[65];
  settings_get_device_name(device_name, sizeof(device_name));
  ESP_LOGI(TAG, "Starting AirPlay service for device: %s", device_name);
  esp_err_t err = airplay_service_start(device_name);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AirPlay service start failed: %s", esp_err_to_name(err));
    receiver_state_set_recovering(true);
    return err;
  }

  s_airplay_started = true;
  receiver_state_set_recovering(false);
  airplay_service_set_active_callback(app_core_on_airplay_active);
  airplay_service_refresh_playback();
  ESP_LOGI(TAG, "AirPlay ready (mDNS published + RTSP listening)");
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

static void load_voice_config(void) {
  realtime_voice_config_t cfg = {0};
  bool from_nvs = true;

  if (settings_get_voice_api_key(cfg.api_key, sizeof(cfg.api_key)) != ESP_OK) {
    from_nvs = false;
    cfg.api_key[0] = '\0';
  }
  if (settings_get_voice_url(cfg.url, sizeof(cfg.url)) != ESP_OK) {
    from_nvs = false;
    cfg.url[0] = '\0';
  }
  if (settings_get_voice_model(cfg.model, sizeof(cfg.model)) != ESP_OK) {
    from_nvs = false;
    cfg.model[0] = '\0';
  }

  /* Fall back to compile-time defaults for any missing NVS fields. */
  if (cfg.url[0] == '\0' && strlen(CONFIG_VOICE_REALTIME_URL) > 0) {
    snprintf(cfg.url, sizeof(cfg.url), "%s", CONFIG_VOICE_REALTIME_URL);
  }
  if (cfg.model[0] == '\0' && strlen(CONFIG_VOICE_MODEL) > 0) {
    snprintf(cfg.model, sizeof(cfg.model), "%s", CONFIG_VOICE_MODEL);
  }
  if (cfg.api_key[0] == '\0' && strlen(CONFIG_VOICE_API_KEY) > 0) {
    snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", CONFIG_VOICE_API_KEY);
  }
  realtime_voice_set_config(&cfg);

  if (realtime_voice_config_ready()) {
    ESP_LOGI(TAG, "Voice config loaded (%s)", from_nvs ? "NVS" : "compile-time defaults");
  } else {
    ESP_LOGW(TAG, "Voice config incomplete: API key not set. "
                  "Use settings API or menuconfig to configure.");
  }
}

static void provision_voice_config_if_needed(void) {
  /* If NVS has no voice config but compile-time values exist, seed NVS. */
  if (settings_has_voice_config()) {
    return;
  }
  bool any_written = false;
  if (strlen(CONFIG_VOICE_REALTIME_URL) > 0) {
    settings_set_voice_url(CONFIG_VOICE_REALTIME_URL);
    any_written = true;
  }
  if (strlen(CONFIG_VOICE_MODEL) > 0) {
    settings_set_voice_model(CONFIG_VOICE_MODEL);
    any_written = true;
  }
  if (strlen(CONFIG_VOICE_API_KEY) > 0) {
    settings_set_voice_api_key(CONFIG_VOICE_API_KEY);
    any_written = true;
  }
  if (any_written) {
    ESP_LOGI(TAG, "Provisioned compile-time voice config into NVS");
  }
}

static void start_voice_services(void) {
  if (s_voice_started) {
    return;
  }
  load_voice_config();
  esp_err_t board_err = iot_board_init();
  if (board_err != ESP_OK) {
    ESP_LOGW(TAG, "Voice requires board init: %s", esp_err_to_name(board_err));
    return;
  }
  esp_err_t err = realtime_voice_start();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Realtime voice start failed: %s", esp_err_to_name(err));
    return;
  }
  realtime_voice_set_enabled(CONFIG_VOICE_MODE_DEFAULT_ENABLED);
  s_voice_started = true;
  if (env_monitor_start() != ESP_OK) {
    ESP_LOGW(TAG, "Env monitor start skipped or failed");
  }
  if (sedentary_monitor_start() != ESP_OK) {
    ESP_LOGW(TAG, "Sedentary monitor start skipped or failed");
  }
}

static void stop_voice_services(void) {
  if (!s_voice_started) {
    return;
  }
  env_monitor_stop();
  sedentary_monitor_stop();
  realtime_voice_stop();
  s_voice_started = false;
}

static void reconcile_voice_mode(void) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  bool airplay_active = snapshot.session_establishing || snapshot.streaming;
  realtime_voice_on_airplay_state_changed(airplay_active);
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

    static int64_t s_last_netmon_stack_log_us = 0;
    int64_t now_stack_us = esp_timer_get_time();
    if ((now_stack_us - s_last_netmon_stack_log_us) >= 30000000LL) {
      s_last_netmon_stack_log_us = now_stack_us;
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
      ESP_LOGI(TAG, "stack watermark[net_mon]: free_words=%lu free_bytes=%lu",
               (unsigned long)watermark,
               (unsigned long)(watermark * sizeof(StackType_t)));
    }

    bool has_network = wifi_is_connected();
    receiver_state_set_network_ready(has_network);
    push_screen_ui_state();
    reconcile_voice_mode();
    airplay_service_refresh_playback();

    if (has_network && !had_network) {
      receiver_state_set_config_required(false);
      receiver_state_set_recovering(false);
      start_airplay_services();
      start_voice_services();
      reconcile_voice_mode();
      airplay_service_refresh_playback();
      log_runtime_resources("wifi_connected");
    } else if (!has_network && had_network) {
      receiver_state_set_recovering(true);
      stop_voice_services();
      stop_airplay_services();
      log_runtime_resources("wifi_disconnected");
    } else if (has_network && !s_airplay_started) {
      start_airplay_services();
      start_voice_services();
      reconcile_voice_mode();
      airplay_service_refresh_playback();
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
      wifi_creds_save(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
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
  resource_manager_init();
  resource_manager_register_callback(app_core_on_resource_event, NULL);
  receiver_state_dispatch(RECEIVER_EVENT_BOOT);
  /* Suppress noisy SPI master debug logs that are unrelated to voice input */
  esp_log_level_set("spi_master", ESP_LOG_WARN);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("event", ESP_LOG_WARN);
  esp_log_level_set("esp-netif", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
  esp_log_level_set("RANDOM", ESP_LOG_WARN);
  esp_log_level_set(" Stations", ESP_LOG_WARN);
  esp_log_level_set("wifi:bcnd", ESP_LOG_WARN);
  esp_log_level_set("transport", ESP_LOG_WARN);
  esp_log_level_set("esp-tls", ESP_LOG_WARN);
  esp_log_level_set("i2s_common", ESP_LOG_INFO);
  esp_log_level_set("I2S_IF", ESP_LOG_INFO);
  esp_log_level_set("ES7210", ESP_LOG_INFO);
  esp_log_level_set("Adev_Codec", ESP_LOG_INFO);
  esp_log_level_set("realtime_voice", ESP_LOG_INFO);

  if (screen_ui_init() != ESP_OK) {
    ESP_LOGW(TAG, "screen UI init failed; continuing without local display");
  } else {
    screen_ui_set_voice_ptt_callback(realtime_voice_notify_user_speech_start);
  }
  {
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t ab_err = i2c_master_get_bus_handle(CONFIG_BSP_I2C_NUM, &i2c_bus);
    if (ab_err == ESP_OK && i2c_bus != NULL) {
      ab_err = auto_brightness_start(i2c_bus, bsp_display_brightness_set);
      if (ab_err != ESP_OK) {
        ESP_LOGW(TAG, "auto brightness start failed: %s", esp_err_to_name(ab_err));
      }
    } else {
      ESP_LOGW(TAG, "I2C bus handle unavailable, auto brightness skipped");
    }
  }
  realtime_voice_set_ui_state_cb(voice_ui_state_bridge);
  realtime_voice_set_ui_network_busy_cb(voice_ui_network_busy_bridge);
  realtime_voice_set_network_query_cb(network_query_for_voice);
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

  ESP_LOGI(TAG, "config build hash: %s", CONFIG_BUILD_HASH);

  provision_voice_config_if_needed();

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

  if (!wifi_creds_have()) {
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
      /* AirPlay-only mode: keep voice stack disabled. */
      airplay_service_refresh_playback();
      push_screen_ui_state();
      log_runtime_resources("startup_connected");
    } else {
      receiver_state_set_recovering(true);
      push_screen_ui_state();
      log_runtime_resources("startup_wait_connect_timeout");
    }

    if (!s_net_mon_stack) {
      s_net_mon_stack = heap_caps_malloc(NET_MON_STACK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (!s_net_mon_stack) {
        s_net_mon_stack = heap_caps_malloc(NET_MON_STACK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      }
    }
    TaskHandle_t net_mon_handle = NULL;
    if (s_net_mon_stack) {
      net_mon_handle = xTaskCreateStaticPinnedToCore(
          network_monitor_task, "net_mon",
          NET_MON_STACK_SIZE / sizeof(StackType_t), NULL, 5,
          s_net_mon_stack, &s_net_mon_tcb, NET_MON_TASK_CORE);
    }
    if (net_mon_handle == NULL) {
      ESP_LOGW(
          TAG,
          "Failed to start network monitor (int_free=%lu largest=%lu), "
          "running monitor in app_core task",
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      push_screen_ui_state();
      network_monitor_task(NULL);
    }
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
