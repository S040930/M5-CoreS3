#include "airplay_service.h"

#include "audio_pipeline.h"
#include "network/mdns_airplay.h"
#include "receiver_state.h"
#include "rtsp/rtsp_events.h"
#include "rtsp/rtsp_server.h"

static bool s_registered = false;
static bool s_infra_ready = false;
static bool s_started = false;

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
    break;
  }
}

esp_err_t airplay_service_start(void) {
  if (!s_registered) {
    rtsp_events_register(on_airplay_event, NULL);
    s_registered = true;
  }

  if (!s_infra_ready) {
    esp_err_t err = audio_pipeline_init();
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
