#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "realtime_voice.h"

esp_err_t voice_domain_init(void);

esp_err_t voice_domain_start(void);

esp_err_t voice_domain_stop(void);

esp_err_t voice_domain_set_enabled(bool enabled);

bool voice_domain_is_enabled(void);

esp_err_t voice_domain_set_config(const realtime_voice_config_t *config);

esp_err_t voice_domain_start_recording(void);

esp_err_t voice_domain_stop_recording(void);

esp_err_t voice_domain_cancel(void);

esp_err_t voice_domain_speak_text(const char *text);

esp_err_t voice_domain_on_airplay_state_changed(bool active);

void voice_domain_set_ui_state_cb(voice_ui_state_cb_t cb);
void voice_domain_set_ui_network_busy_cb(voice_ui_network_busy_cb_t cb);
