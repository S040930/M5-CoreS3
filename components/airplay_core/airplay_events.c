#include "airplay_events.h"
#include "event_bus.h"
#include "esp_log.h"

static const char *TAG = "airplay_events";

void airplay_publish_client_connected(const char *device_name, const char *peer_address) {
    event_airplay_client_t data = {
        .id = EVENT_AIRPLAY_CLIENT_CONNECTED,
        .device_name = device_name,
        .peer_address = peer_address
    };
    event_bus_publish(EVENT_AIRPLAY_CLIENT_CONNECTED, &data, sizeof(data));
    ESP_LOGI(TAG, "Published: AIRPLAY_CLIENT_CONNECTED");
}

void airplay_publish_client_disconnected(void) {
    event_bus_publish(EVENT_AIRPLAY_CLIENT_DISCONNECTED, NULL, 0);
    ESP_LOGI(TAG, "Published: AIRPLAY_CLIENT_DISCONNECTED");
}

void airplay_publish_playback_start(void) {
    event_bus_publish(EVENT_AIRPLAY_PLAYBACK_START, NULL, 0);
    ESP_LOGI(TAG, "Published: AIRPLAY_PLAYBACK_START");
}

void airplay_publish_playback_pause(void) {
    event_bus_publish(EVENT_AIRPLAY_PLAYBACK_PAUSE, NULL, 0);
    ESP_LOGI(TAG, "Published: AIRPLAY_PLAYBACK_PAUSE");
}

void airplay_publish_playback_stop(void) {
    event_bus_publish(EVENT_AIRPLAY_PLAYBACK_STOP, NULL, 0);
    ESP_LOGI(TAG, "Published: AIRPLAY_PLAYBACK_STOP");
}

void airplay_publish_stream_start(void) {
    event_bus_publish(EVENT_AIRPLAY_STREAM_START, NULL, 0);
    ESP_LOGI(TAG, "Published: AIRPLAY_STREAM_START");
}

void airplay_publish_stream_stop(void) {
    event_bus_publish(EVENT_AIRPLAY_STREAM_STOP, NULL, 0);
    ESP_LOGI(TAG, "Published: AIRPLAY_STREAM_STOP");
}
