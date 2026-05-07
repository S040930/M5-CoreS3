#include "airplay_service.h"

#include "audio/audio_output.h"
#include "audio_pipeline.h"
#include "esp_log.h"
#include "iot_board.h"
#include "network/mdns_airplay.h"
#include "receiver_state.h"
#include "resource/resource_manager.h"
#include "screen_ui.h"
#include "rtsp/rtsp_events.h"
#include "rtsp/rtsp_server.h"
#include <string.h>

static const char *TAG = "airplay_service";

static airplay_service_active_cb_t s_active_cb;

static void notify_airplay_active(bool active) {
  if (s_active_cb != NULL) {
    s_active_cb(active);
  }
}

void airplay_service_set_active_callback(airplay_service_active_cb_t cb) { s_active_cb = cb; }

static bool s_registered = false;
static bool s_board_ready = false;
static bool s_infra_ready = false;
static bool s_mdns_ready = false;
static bool s_started = false;
static bool s_playback_desired = false;
static bool s_playback_running = false;
static char s_device_name[65] = {0};
static audio_pipeline_session_cfg_t s_pipeline_cfg = {
    .target_latency_ms = CONFIG_AUDIO_TARGET_LATENCY_MS,
};

void airplay_service_refresh_playback(void);

static void log_owner_state(const char *reason) {
  char owner_tag[24] = {0};
  bool owned = false;
  esp_err_t err =
      audio_output_get_external_owner(owner_tag, sizeof(owner_tag), &owned);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "owner diag failed[%s]: %s", reason, esp_err_to_name(err));
    return;
  }

  if (owned) {
    ESP_LOGW(TAG, "speaker owner[%s]: external=1 owner=%s", reason,
             owner_tag[0] ? owner_tag : "external");
  } else {
    ESP_LOGI(TAG, "speaker owner[%s]: external=0", reason);
  }
}

static void log_playback_state(const char *reason) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  ESP_LOGI(TAG,
           "playback state[%s]: receiver=%s desired=%d running=%d output_active=%d",
           reason, receiver_state_to_str(snapshot.state), s_playback_desired ? 1 : 0,
           s_playback_running ? 1 : 0, audio_output_is_active() ? 1 : 0);
}

static void sync_playback_worker(void) {
  if (!s_started) {
    return;
  }

  if (s_playback_running && !audio_output_is_active()) {
    ESP_LOGW(TAG, "playback worker marked stopped: output no longer active");
    s_playback_running = false;
    resource_manager_set_airplay_active(false);
    resource_manager_release(RESOURCE_OWNER_AIRPLAY);
    log_owner_state("output_inactive");
  }

  if (!s_playback_desired) {
    if (s_playback_running) {
      audio_pipeline_stop();
      s_playback_running = false;
      resource_manager_set_airplay_active(false);
      resource_manager_release(RESOURCE_OWNER_AIRPLAY);
      ESP_LOGI(TAG, "audio pipeline stopped (desired=0)");
    }
    log_playback_state("desired_off");
    return;
  }

  if (s_playback_running) {
    log_playback_state("already_running");
    return;
  }

  {
    char owner_tag[24] = {0};
    bool ext_owned = false;
    if (audio_output_get_external_owner(owner_tag, sizeof(owner_tag), &ext_owned) == ESP_OK &&
        ext_owned && owner_tag[0] != '\0') {
      ESP_LOGW(TAG, "speaker owner[pipeline_start_blocked]: external=1 owner=%s", owner_tag);
      log_playback_state("pipeline_blocked_external_owner");
      return;
    }
  }

  esp_err_t err = audio_pipeline_start(&s_pipeline_cfg);
  if (err == ESP_OK) {
    s_playback_running = true;
    resource_manager_set_airplay_active(true);
    resource_manager_acquire(RESOURCE_OWNER_AIRPLAY);
    ESP_LOGI(TAG, "audio pipeline started");
  } else {
    ESP_LOGW(TAG, "audio pipeline start failed: %s", esp_err_to_name(err));
    log_owner_state("pipeline_start_failed");
  }
  log_playback_state("sync");
}

static void push_screen_state_from_receiver(void) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  screen_ui_set_state(
      receiver_state_map_screen_ui(snapshot.state), snapshot.network_ready,
      snapshot.discoverable || snapshot.session_establishing || snapshot.streaming,
      snapshot.streaming);
}

static esp_err_t ensure_board_ready(void) {
  if (s_board_ready) {
    return ESP_OK;
  }

  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    return err;
  }

  s_board_ready = true;
  return ESP_OK;
}

static void on_airplay_event(rtsp_event_t event, const rtsp_event_data_t *data,
                             void *user_data) {
  (void)data;
  (void)user_data;

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    ESP_LOGI(TAG, "RTSP event: client connected");
    receiver_state_set_session_establishing(true);
    s_playback_desired = false;
    notify_airplay_active(true);
    sync_playback_worker();
    log_playback_state("client_connected");
    break;
  case RTSP_EVENT_PLAYING:
    ESP_LOGI(TAG, "RTSP event: playing");
    receiver_state_set_streaming(true);
    screen_ui_set_playing(true);
    s_playback_desired = true;
    notify_airplay_active(true);
    sync_playback_worker();
    log_playback_state("playing");
    break;
  case RTSP_EVENT_PAUSED:
    ESP_LOGI(TAG, "RTSP event: paused");
    receiver_state_set_streaming(false);
    receiver_state_set_session_establishing(true);
    screen_ui_set_playing(false);
    s_playback_desired = false;
    notify_airplay_active(true);
    sync_playback_worker();
    log_playback_state("paused");
    break;
  case RTSP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "RTSP event: disconnected");
    receiver_state_set_streaming(false);
    receiver_state_set_session_establishing(false);
    receiver_state_set_discoverable(true);
    s_playback_desired = false;
    notify_airplay_active(false);
    sync_playback_worker();
    log_playback_state("disconnected");
    break;
  case RTSP_EVENT_METADATA:
    if (data != NULL) {
      screen_ui_metadata_t ui_meta = {0};
      memcpy(ui_meta.title, data->metadata.title, sizeof(ui_meta.title) - 1);
      memcpy(ui_meta.artist, data->metadata.artist, sizeof(ui_meta.artist) - 1);
      ui_meta.position_secs = data->metadata.position_secs;
      ui_meta.duration_secs = data->metadata.duration_secs;
      ui_meta.has_progress = data->metadata.duration_secs > 0;
      screen_ui_set_metadata(&ui_meta);
    }
    break;
  }
  push_screen_state_from_receiver();
}

esp_err_t airplay_service_start(const char *device_name) {
  ESP_LOGI(TAG, "airplay_service_start() called");
  if (device_name) {
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';
  }

  if (!s_registered) {
    rtsp_events_register(on_airplay_event, NULL);
    s_registered = true;
  }

  if (!s_infra_ready) {
    ESP_LOGI(TAG, "AirPlay infra init: board");
    esp_err_t err = ensure_board_ready();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
      return err;
    }

    ESP_LOGI(TAG, "AirPlay infra init: audio pipeline");
    err = audio_pipeline_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "audio pipeline init failed: %s", esp_err_to_name(err));
      return err;
    }

    s_infra_ready = true;
  }

  if (!s_mdns_ready) {
    ESP_LOGI(TAG, "AirPlay infra init: mDNS");
    esp_err_t err = mdns_airplay_init(s_device_name);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
      return err;
    }
    s_mdns_ready = true;
    ESP_LOGI(TAG, "AirPlay mDNS ready");
  }

  if (s_started) {
    ESP_LOGI(TAG, "AirPlay already started; forcing discoverable state");
    receiver_state_set_discoverable(true);
    airplay_service_refresh_playback();
    log_playback_state("already_started");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "AirPlay infra init: RTSP server");
  esp_err_t err = rtsp_server_start(s_device_name);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "RTSP server start failed: %s", esp_err_to_name(err));
    if (s_mdns_ready) {
      mdns_airplay_deinit();
      s_mdns_ready = false;
      ESP_LOGW(TAG, "mDNS rolled back because RTSP start failed");
    }
    return err;
  }
  if (!rtsp_server_is_running()) {
    ESP_LOGE(TAG, "RTSP server failed to enter running state");
    rtsp_server_stop();
    if (s_mdns_ready) {
      mdns_airplay_deinit();
      s_mdns_ready = false;
    }
    return ESP_FAIL;
  }

  s_started = true;
  receiver_state_set_discoverable(true);
  receiver_state_set_recovering(false);
  s_playback_desired = false;
  s_playback_running = false;
  airplay_service_refresh_playback();
  ESP_LOGI(TAG, "AirPlay service ready (mdns=1 rtsp=1 port=7000)");
  log_playback_state("service_started");
  return ESP_OK;
}

void airplay_service_stop(void) {
  if (!s_started && !s_mdns_ready) {
    return;
  }

  ESP_LOGI(TAG, "airplay_service_stop() called");
  if (s_started) {
    rtsp_server_stop();
    audio_pipeline_stop();
  }
  if (s_mdns_ready) {
    mdns_airplay_deinit();
    s_mdns_ready = false;
  }

  s_started = false;
  s_playback_desired = false;
  if (s_playback_running) {
    resource_manager_set_airplay_active(false);
    resource_manager_release(RESOURCE_OWNER_AIRPLAY);
  }
  s_playback_running = false;
  receiver_state_set_discoverable(false);
  receiver_state_set_session_establishing(false);
  receiver_state_set_streaming(false);
  notify_airplay_active(false);
}

esp_err_t airplay_service_recover(void) {
  ESP_LOGW(TAG, "airplay_service_recover() called");
  receiver_state_set_recovering(true);
  airplay_service_stop();
  return airplay_service_start(s_device_name);
}

void airplay_service_refresh_playback(void) {
  if (!s_started) {
    return;
  }

  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  bool should_play = snapshot.session_establishing || snapshot.streaming;
  if (should_play != s_playback_desired) {
    ESP_LOGI(TAG, "playback desired changed: %d -> %d",
             s_playback_desired ? 1 : 0, should_play ? 1 : 0);
    s_playback_desired = should_play;
  }
  sync_playback_worker();
}

bool airplay_service_is_active(void) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  return snapshot.session_establishing || snapshot.streaming;
}
