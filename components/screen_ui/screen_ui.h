#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SCREEN_UI_TEXT_MAX 64

typedef enum {
  SCREEN_UI_STATE_BOOT = 0,
  SCREEN_UI_STATE_CONFIG_REQUIRED,
  SCREEN_UI_STATE_NETWORK_READY,
  SCREEN_UI_STATE_DISCOVERABLE,
  SCREEN_UI_STATE_SESSION_ESTABLISHING,
  SCREEN_UI_STATE_STREAMING,
  SCREEN_UI_STATE_RECOVERING,
  SCREEN_UI_STATE_FAULT,
} screen_ui_state_t;

typedef struct {
  char title[SCREEN_UI_TEXT_MAX];
  char artist[SCREEN_UI_TEXT_MAX];
  uint32_t position_secs;
  uint32_t duration_secs;
  bool has_progress;
} screen_ui_metadata_t;

typedef enum {
  SCREEN_UI_VOICE_OFF = 0,
  SCREEN_UI_VOICE_CONNECTING,
  SCREEN_UI_VOICE_LISTENING,
  SCREEN_UI_VOICE_SENDING,
  SCREEN_UI_VOICE_THINKING,
  SCREEN_UI_VOICE_SPEAKING,
  SCREEN_UI_VOICE_ERROR,
  SCREEN_UI_VOICE_STANDBY,
} screen_ui_voice_state_t;

esp_err_t screen_ui_init(void);
void screen_ui_deinit(void);
void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming);
void screen_ui_set_metadata(const screen_ui_metadata_t *metadata);
void screen_ui_set_playing(bool playing);
void screen_ui_set_voice_state(screen_ui_voice_state_t state, const char *user_text,
                               const char *assistant_text, const char *error_text);

/** Optional: while voice overlay is visible, any screen tap calls this (push-to-talk assist). */
void screen_ui_set_voice_ptt_callback(void (*callback)(void));

/**
 * Throttle LVGL animation timer while voice stack runs heavy TLS/WebSocket work
 * (frees CPU1 for IDLE task WDT and reduces contention for internal DMA heap).
 */
void screen_ui_set_voice_network_busy(bool busy);
