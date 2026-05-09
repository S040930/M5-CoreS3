#include "event_bus.h"
#include "esp_log.h"

ESP_EVENT_DEFINE_BASE(EVENT_BUS_BASE);

static const char *TAG = "event_bus";

const char *event_id_to_string(event_id_t id) {
    static const char *names[EVENT_ID_COUNT] = {
        [EVENT_VOICE_WAKEWORD_DETECTED] = "VOICE_WAKEWORD_DETECTED",
        [EVENT_VOICE_SPEECH_START] = "VOICE_SPEECH_START",
        [EVENT_VOICE_SPEECH_END] = "VOICE_SPEECH_END",
        [EVENT_VOICE_REQUEST_SEND] = "VOICE_REQUEST_SEND",
        [EVENT_VOICE_RESPONSE_AUDIO] = "VOICE_RESPONSE_AUDIO",
        [EVENT_VOICE_RESPONSE_TEXT] = "VOICE_RESPONSE_TEXT",
        [EVENT_VOICE_PLAYBACK_START] = "VOICE_PLAYBACK_START",
        [EVENT_VOICE_PLAYBACK_END] = "VOICE_PLAYBACK_END",
        [EVENT_VOICE_ERROR] = "VOICE_ERROR",

        [EVENT_AIRPLAY_CLIENT_CONNECTED] = "AIRPLAY_CLIENT_CONNECTED",
        [EVENT_AIRPLAY_CLIENT_DISCONNECTED] = "AIRPLAY_CLIENT_DISCONNECTED",
        [EVENT_AIRPLAY_PLAYBACK_START] = "AIRPLAY_PLAYBACK_START",
        [EVENT_AIRPLAY_PLAYBACK_PAUSE] = "AIRPLAY_PLAYBACK_PAUSE",
        [EVENT_AIRPLAY_PLAYBACK_STOP] = "AIRPLAY_PLAYBACK_STOP",
        [EVENT_AIRPLAY_STREAM_START] = "AIRPLAY_STREAM_START",
        [EVENT_AIRPLAY_STREAM_STOP] = "AIRPLAY_STREAM_STOP",

        [EVENT_AUDIO_DECODE_REQUEST] = "AUDIO_DECODE_REQUEST",
        [EVENT_AUDIO_DECODE_DONE] = "AUDIO_DECODE_DONE",
        [EVENT_AUDIO_ENCODE_REQUEST] = "AUDIO_ENCODE_REQUEST",
        [EVENT_AUDIO_ENCODE_DONE] = "AUDIO_ENCODE_DONE",
        [EVENT_AUDIO_OUTPUT_UNDERFLOW] = "AUDIO_OUTPUT_UNDERFLOW",
        [EVENT_AUDIO_OUTPUT_OVERFLOW] = "AUDIO_OUTPUT_OVERFLOW",
        [EVENT_AUDIO_OUTPUT_STARTED] = "AUDIO_OUTPUT_STARTED",
        [EVENT_AUDIO_OUTPUT_STOPPED] = "AUDIO_OUTPUT_STOPPED",

        [EVENT_ENV_DATA_READY] = "ENV_DATA_READY",
        [EVENT_ENV_THRESHOLD_EXCEEDED] = "ENV_THRESHOLD_EXCEEDED",

        [EVENT_SYSTEM_WIFI_CONNECTED] = "SYSTEM_WIFI_CONNECTED",
        [EVENT_SYSTEM_WIFI_DISCONNECTED] = "SYSTEM_WIFI_DISCONNECTED",
        [EVENT_SYSTEM_RESOURCE_LOW] = "SYSTEM_RESOURCE_LOW",
        [EVENT_SYSTEM_BOOT] = "SYSTEM_BOOT",
    };

    if (id < 0 || id >= EVENT_ID_COUNT) {
        return "UNKNOWN";
    }
    return names[id] ? names[id] : "UNKNOWN";
}

esp_err_t event_bus_init(void) {
    ESP_LOGI(TAG, "Event bus initialized");
    return ESP_OK;
}

esp_err_t event_bus_publish(event_id_t id, void *data, size_t data_size) {
    esp_err_t err = esp_event_post(EVENT_BUS_BASE, id, data, data_size, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post event %s: %s", event_id_to_string(id), esp_err_to_name(err));
    }
    return err;
}

esp_err_t event_bus_subscribe(event_id_t id, esp_event_handler_t handler, void *handler_args) {
    return esp_event_handler_register(EVENT_BUS_BASE, id, handler, handler_args);
}

esp_err_t event_bus_unsubscribe(event_id_t id, esp_event_handler_t handler) {
    return esp_event_handler_unregister(EVENT_BUS_BASE, id, handler);
}
