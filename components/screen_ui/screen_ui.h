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

esp_err_t screen_ui_init(void);
void screen_ui_deinit(void);
void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming);
void screen_ui_set_metadata(const screen_ui_metadata_t *metadata);
