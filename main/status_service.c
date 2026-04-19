#include "status_service.h"

#include "esp_app_desc.h"
#include "audio/audio_receiver.h"
#include "ethernet.h"
#include "esp_system.h"
#include "receiver_state.h"
#include "rtsp/rtsp_events.h"
#include "settings.h"
#include "wifi.h"

#include <string.h>

static bool s_initialized = false;
static uint32_t s_reconnect_count = 0;
static char s_track_title[64];
static char s_track_artist[64];

static void copy_str(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;

  if (event == RTSP_EVENT_METADATA && data) {
    copy_str(s_track_title, sizeof(s_track_title), data->metadata.title);
    copy_str(s_track_artist, sizeof(s_track_artist), data->metadata.artist);
  } else if (event == RTSP_EVENT_DISCONNECTED) {
    s_track_title[0] = '\0';
    s_track_artist[0] = '\0';
  }
}

esp_err_t status_service_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }

  memset(s_track_title, 0, sizeof(s_track_title));
  memset(s_track_artist, 0, sizeof(s_track_artist));
  rtsp_events_register(on_rtsp_event, NULL);
  s_initialized = true;
  return ESP_OK;
}

void status_service_note_reconnect(void) { s_reconnect_count++; }

void status_service_get_snapshot(status_service_snapshot_t *snapshot) {
  if (!snapshot) {
    return;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  settings_get_device_name(snapshot->device_name, sizeof(snapshot->device_name));

  snapshot->wifi_connected = wifi_is_connected();
  snapshot->eth_connected = ethernet_is_connected();
  snapshot->ap_enabled = wifi_is_ap_enabled();
  snapshot->playing = audio_receiver_is_playing();
  snapshot->free_heap = esp_get_free_heap_size();
  snapshot->min_free_heap = esp_get_minimum_free_heap_size();
  snapshot->reconnect_count = s_reconnect_count;

  if (snapshot->eth_connected) {
    ethernet_get_ip_str(snapshot->ip, sizeof(snapshot->ip));
    ethernet_get_mac_str(snapshot->mac, sizeof(snapshot->mac));
  } else {
    wifi_get_ip_str(snapshot->ip, sizeof(snapshot->ip));
    wifi_get_mac_str(snapshot->mac, sizeof(snapshot->mac));
  }

  const esp_app_desc_t *app_desc = esp_app_get_description();
  copy_str(snapshot->firmware_version, sizeof(snapshot->firmware_version),
           app_desc ? app_desc->version : "");

  receiver_state_snapshot_t receiver_snapshot = {0};
  receiver_state_get_snapshot(&receiver_snapshot);
  copy_str(snapshot->receiver_state, sizeof(snapshot->receiver_state),
           receiver_state_to_str(receiver_snapshot.state));

  copy_str(snapshot->track_title, sizeof(snapshot->track_title), s_track_title);
  copy_str(snapshot->track_artist, sizeof(snapshot->track_artist),
           s_track_artist);

  if (receiver_snapshot.streaming || receiver_snapshot.session_establishing) {
    copy_str(snapshot->playback_source, sizeof(snapshot->playback_source),
             "airplay");
  } else {
    copy_str(snapshot->playback_source, sizeof(snapshot->playback_source),
             "idle");
  }

  audio_pipeline_get_snapshot(&snapshot->pipeline);
}
