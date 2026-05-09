#include "voice_interface.h"
#include "realtime_voice.h"
#include "esp_log.h"

static const char *TAG = "voice_domain";
static bool s_enabled = true;

esp_err_t voice_domain_init(void) {
    ESP_LOGI(TAG, "Voice domain initialized");
    return ESP_OK;
}

esp_err_t voice_domain_start(void) {
    return realtime_voice_start();
}

esp_err_t voice_domain_stop(void) {
    realtime_voice_stop();
    return ESP_OK;
}

esp_err_t voice_domain_set_enabled(bool enabled) {
    s_enabled = enabled;
    realtime_voice_set_enabled(enabled);
    return ESP_OK;
}

bool voice_domain_is_enabled(void) {
    return s_enabled;
}

esp_err_t voice_domain_set_config(const realtime_voice_config_t *config) {
    realtime_voice_set_config(config);
    return ESP_OK;
}

esp_err_t voice_domain_start_recording(void) {
    realtime_voice_notify_user_speech_start();
    return ESP_OK;
}

esp_err_t voice_domain_stop_recording(void) {
    return ESP_OK;
}

esp_err_t voice_domain_cancel(void) {
    realtime_voice_interrupt_response();
    return ESP_OK;
}

esp_err_t voice_domain_speak_text(const char *text) {
    return realtime_voice_speak_text(text);
}

esp_err_t voice_domain_on_airplay_state_changed(bool active) {
    realtime_voice_on_airplay_state_changed(active);
    return ESP_OK;
}

void voice_domain_set_ui_state_cb(voice_ui_state_cb_t cb) {
    realtime_voice_set_ui_state_cb(cb);
}

void voice_domain_set_ui_network_busy_cb(voice_ui_network_busy_cb_t cb) {
    realtime_voice_set_ui_network_busy_cb(cb);
}
