#include "app_events.h"
#include "app_core.h"
#include "screen_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "event_ids.h"
#include "event_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_events";

static void on_airplay_active(bool active) {
    ESP_LOGI(TAG, "AirPlay active changed: %s", active ? "true" : "false");
}

static void on_wifi_connected(void) {
    ESP_LOGI(TAG, "WiFi connected");
}

static void on_wifi_disconnected(void) {
    ESP_LOGI(TAG, "WiFi disconnected");
}

static void on_voice_state_changed(int state, const char *user, const char *assistant, const char *error) {
    ESP_LOGI(TAG, "Voice state: %d", state);
}

static void on_env_threshold(float temp, float humidity, float pressure) {
    ESP_LOGI(TAG, "ENV threshold: temp=%.1f humidity=%.1f pressure=%.1f",
             temp, humidity, pressure);
}

static void event_handler_airplay(void* handler_args, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data) {
    (void)handler_args;
    (void)event_base;
    if (event_id == EVENT_AIRPLAY_CLIENT_CONNECTED) {
        on_airplay_active(true);
    } else if (event_id == EVENT_AIRPLAY_CLIENT_DISCONNECTED) {
        on_airplay_active(false);
    }
}

static void event_handler_wifi(void* handler_args, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    (void)handler_args;
    (void)event_base;
    if (event_id == EVENT_SYSTEM_WIFI_CONNECTED) {
        on_wifi_connected();
    } else if (event_id == EVENT_SYSTEM_WIFI_DISCONNECTED) {
        on_wifi_disconnected();
    }
}

static void event_handler_env(void* handler_args, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    (void)handler_args;
    (void)event_base;
    if (event_id == EVENT_ENV_THRESHOLD_EXCEEDED && event_data) {
        event_env_data_t *data = (event_env_data_t *)event_data;
        on_env_threshold(data->temperature, data->humidity, data->pressure);
    }
}

void app_events_init(void) {
    ESP_LOGI(TAG, "App events initialized");

    event_bus_subscribe(EVENT_AIRPLAY_CLIENT_CONNECTED, event_handler_airplay, NULL);
    event_bus_subscribe(EVENT_AIRPLAY_CLIENT_DISCONNECTED, event_handler_airplay, NULL);
    event_bus_subscribe(EVENT_SYSTEM_WIFI_CONNECTED, event_handler_wifi, NULL);
    event_bus_subscribe(EVENT_SYSTEM_WIFI_DISCONNECTED, event_handler_wifi, NULL);
    event_bus_subscribe(EVENT_ENV_THRESHOLD_EXCEEDED, event_handler_env, NULL);
}

void app_events_on_airplay_active(bool active) {
    on_airplay_active(active);
}

void app_events_on_wifi_connected(void) {
    on_wifi_connected();
}

void app_events_on_wifi_disconnected(void) {
    on_wifi_disconnected();
}

void app_events_on_voice_state_changed(int state, const char *user, const char *assistant, const char *error) {
    on_voice_state_changed(state, user, assistant, error);
}

void app_events_on_env_threshold(float temp, float humidity, float pressure) {
    on_env_threshold(temp, humidity, pressure);
}
