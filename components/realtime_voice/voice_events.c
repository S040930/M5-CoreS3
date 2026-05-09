#include "voice_events.h"
#include "event_bus.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "voice_events";

void voice_publish_wakeword_detected(void) {
    event_bus_publish(EVENT_VOICE_WAKEWORD_DETECTED, NULL, 0);
    ESP_LOGI(TAG, "Published: VOICE_WAKEWORD_DETECTED");
}

void voice_publish_speech_start(void) {
    event_bus_publish(EVENT_VOICE_SPEECH_START, NULL, 0);
    ESP_LOGI(TAG, "Published: VOICE_SPEECH_START");
}

void voice_publish_speech_end(void) {
    event_bus_publish(EVENT_VOICE_SPEECH_END, NULL, 0);
    ESP_LOGI(TAG, "Published: VOICE_SPEECH_END");
}

void voice_publish_request_send(const char *request_id) {
    if (request_id) {
        event_bus_publish(EVENT_VOICE_REQUEST_SEND, (void *)request_id, strlen(request_id) + 1);
    } else {
        event_bus_publish(EVENT_VOICE_REQUEST_SEND, NULL, 0);
    }
    ESP_LOGI(TAG, "Published: VOICE_REQUEST_SEND");
}

void voice_publish_response_audio(void) {
    event_bus_publish(EVENT_VOICE_RESPONSE_AUDIO, NULL, 0);
    ESP_LOGI(TAG, "Published: VOICE_RESPONSE_AUDIO");
}

void voice_publish_response_text(const char *text) {
    ESP_LOGI(TAG, "Published: VOICE_RESPONSE_TEXT: %s", text ? text : "(null)");
}

void voice_publish_playback_start(void) {
    event_bus_publish(EVENT_VOICE_PLAYBACK_START, NULL, 0);
    ESP_LOGI(TAG, "Published: VOICE_PLAYBACK_START");
}

void voice_publish_playback_end(void) {
    event_bus_publish(EVENT_VOICE_PLAYBACK_END, NULL, 0);
    ESP_LOGI(TAG, "Published: VOICE_PLAYBACK_END");
}

void voice_publish_error(const char *error_msg) {
    ESP_LOGI(TAG, "Published: VOICE_ERROR: %s", error_msg ? error_msg : "(null)");
}
