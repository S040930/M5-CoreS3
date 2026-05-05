#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  VOICE_FE_EVENT_NONE = 0,
  VOICE_FE_EVENT_AUDIO,
  VOICE_FE_EVENT_WAKE,
  VOICE_FE_EVENT_ERROR,
} voice_fe_event_type_t;

#ifndef VOICE_FE_MAX_PCM_FRAMES
#define VOICE_FE_MAX_PCM_FRAMES 1024
#endif

typedef struct {
  voice_fe_event_type_t type;
  uint64_t timestamp_ms;
  bool vad_speech;
  size_t frames;
  uint16_t slot_id;
  uint32_t slot_seq;
  esp_err_t err;
} voice_fe_event_t;

typedef struct {
  esp_codec_dev_handle_t mic;
} voice_frontend_config_t;

typedef struct {
  uint32_t mic_read_ok;
  uint32_t mic_read_fail;
  uint32_t feed_ok;
  uint32_t fetch_ok;
  uint32_t fetch_timeout_count;
  uint32_t fetch_yield_count;
  uint32_t yield_progress_count;
  uint32_t yield_no_progress_count;
  uint32_t last_loop_hz;
  uint32_t wake_detect_count;
  uint32_t wake_forward_count;
  uint32_t queue_drop;
  size_t feed_pending_frames;
  uint64_t last_read_ok_ms;
  uint64_t last_feed_ok_ms;
  uint64_t last_fetch_ms;
} voice_frontend_health_t;

/* Starts frontend processing on an already-opened mic handle. */
esp_err_t voice_frontend_start(const voice_frontend_config_t *cfg);
void voice_frontend_stop(void);
bool voice_frontend_is_running(void);
bool voice_frontend_read_event(voice_fe_event_t *out, TickType_t ticks);
bool voice_frontend_read_slot_pcm(uint16_t slot_id, uint32_t slot_seq, int16_t *dst,
                                  size_t cap_frames, size_t *out_frames);
bool voice_frontend_get_health(voice_frontend_health_t *out);
