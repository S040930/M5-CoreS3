#pragma once

#include "audio_pipeline.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char device_name[65];
  char ip[16];
  char mac[18];
  char firmware_version[32];
  char receiver_state[24];
  char playback_source[16];
  char track_title[64];
  char track_artist[64];
  bool wifi_connected;
  bool eth_connected;
  bool ap_enabled;
  bool authenticated_setup_mode;
  bool playing;
  uint32_t free_heap;
  uint32_t min_free_heap;
  uint32_t largest_internal_block;
  uint32_t free_psram;
  uint32_t largest_psram_block;
  uint32_t reconnect_count;
  audio_pipeline_snapshot_t pipeline;
} status_service_snapshot_t;

esp_err_t status_service_init(void);
void status_service_note_reconnect(void);
void status_service_get_snapshot(status_service_snapshot_t *snapshot);
