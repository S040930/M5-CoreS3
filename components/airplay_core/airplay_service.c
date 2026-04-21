#include "airplay_service.h"

#include "audio_pipeline.h"
#include "iot_board.h"
#include "network/mdns_airplay.h"
#include "receiver_state.h"
#include "screen_ui.h"
#include "rtsp/rtsp_events.h"
#include "rtsp/rtsp_server.h"
#include <string.h>

static bool s_registered = false;
static bool s_board_ready = false;
static bool s_infra_ready = false;
static bool s_started = false;

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

static void push_screen_state_from_receiver(void) {
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  screen_ui_set_state(
      map_screen_state(snapshot.state), snapshot.network_ready,
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
    receiver_state_set_session_establishing(true);
    break;
  case RTSP_EVENT_PLAYING:
    receiver_state_set_streaming(true);
    break;
  case RTSP_EVENT_PAUSED:
    receiver_state_set_streaming(false);
    receiver_state_set_session_establishing(true);
    break;
  case RTSP_EVENT_DISCONNECTED:
    receiver_state_set_streaming(false);
    receiver_state_set_session_establishing(false);
    receiver_state_set_discoverable(true);
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

esp_err_t airplay_service_start(void) {
  if (!s_registered) {
    rtsp_events_register(on_airplay_event, NULL);
    s_registered = true;
  }

  if (!s_infra_ready) {
    esp_err_t err = ensure_board_ready();
    if (err != ESP_OK) {
      return err;
    }

    err = audio_pipeline_init();
    if (err != ESP_OK) {
      return err;
    }

    err = mdns_airplay_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return err;
    }

    s_infra_ready = true;
  }

  if (s_started) {
    receiver_state_set_discoverable(true);
    return ESP_OK;
  }

  audio_pipeline_session_cfg_t cfg = {
      .target_latency_ms = CONFIG_AUDIO_TARGET_LATENCY_MS,
  };
  esp_err_t err = audio_pipeline_start(&cfg);
  if (err != ESP_OK) {
    return err;
  }

  err = rtsp_server_start();
  if (err != ESP_OK) {
    audio_pipeline_stop();
    return err;
  }

  s_started = true;
  receiver_state_set_discoverable(true);
  receiver_state_set_recovering(false);
  return ESP_OK;
}

void airplay_service_stop(void) {
  if (!s_started) {
    return;
  }

  rtsp_server_stop();
  audio_pipeline_stop();
  s_started = false;
  receiver_state_set_discoverable(false);
  receiver_state_set_session_establishing(false);
  receiver_state_set_streaming(false);
}

esp_err_t airplay_service_recover(void) {
  receiver_state_set_recovering(true);
  airplay_service_stop();
  return airplay_service_start();
}
