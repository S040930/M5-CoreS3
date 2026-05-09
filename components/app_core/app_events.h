#pragma once

#include "esp_event.h"
#include "event_bus.h"

void app_events_init(void);

void app_events_on_airplay_active(bool active);

void app_events_on_wifi_connected(void);

void app_events_on_wifi_disconnected(void);

void app_events_on_voice_state_changed(int state, const char *user, const char *assistant, const char *error);

void app_events_on_env_threshold(float temp, float humidity, float pressure);
