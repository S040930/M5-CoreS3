#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  DEVICE_MODE_AIRPLAY = 0,
  DEVICE_MODE_VOICE,
} device_mode_t;

typedef enum {
  REALTIME_VOICE_STATE_OFF = 0,
  REALTIME_VOICE_STATE_STANDBY,
  REALTIME_VOICE_STATE_CONNECTING,
  REALTIME_VOICE_STATE_LISTENING,
  REALTIME_VOICE_STATE_SENDING,
  REALTIME_VOICE_STATE_THINKING,
  REALTIME_VOICE_STATE_SPEAKING,
  REALTIME_VOICE_STATE_ERROR,
} realtime_voice_state_t;

typedef struct {
  char url[256];
  char api_key[256];
  char model[128];
} realtime_voice_config_t;

esp_err_t realtime_voice_start(void);
void realtime_voice_stop(void);
void realtime_voice_set_enabled(bool enabled);
bool realtime_voice_is_enabled(void);
void realtime_voice_on_airplay_state_changed(bool active);
device_mode_t realtime_voice_get_mode(void);
void realtime_voice_notify_user_speech_start(void);
void realtime_voice_interrupt_response(void);
void realtime_voice_reset_session(void);
void realtime_voice_set_config(const realtime_voice_config_t *config);
bool realtime_voice_config_ready(void);

/** When activation phrase is off, always true; otherwise reflects session armed state. */
bool realtime_voice_is_activation_armed(void);

/** True while assistant TTS is playing or a server response is in flight. */
bool realtime_voice_is_response_active(void);
