#include "audio_interface.h"
#include "audio/audio_output.h"
#include "audio_pipeline.h"
#include "esp_log.h"
#include "event_bus.h"

static const char *TAG = "audio_domain";
static float s_volume = 1.0f;

esp_err_t audio_domain_init(void) {
    ESP_LOGI(TAG, "Audio domain initialized");
    return ESP_OK;
}

esp_err_t audio_domain_decode(const uint8_t *input, size_t len,
                              int16_t **output, size_t *frames) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_domain_encode(const int16_t *input, size_t frames,
                              uint8_t **output, size_t *len) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_domain_output_play(const int16_t *samples, size_t frames) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_domain_output_stop(void) {
    audio_pipeline_stop();
    event_bus_publish(EVENT_AUDIO_OUTPUT_STOPPED, NULL, 0);
    return ESP_OK;
}

esp_err_t audio_domain_output_pause(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_domain_output_resume(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_domain_set_volume(float volume) {
    s_volume = volume;
    audio_output_set_target_volume_db(volume);
    return ESP_OK;
}

float audio_domain_get_volume(void) {
    return s_volume;
}

esp_err_t audio_domain_acquire_speaker(const char *owner) {
    return audio_output_acquire_external(owner, true);
}

esp_err_t audio_domain_release_speaker(const char *owner) {
    return audio_output_release_external(owner, true);
}

bool audio_domain_is_speaker_available(const char *owner) {
    char current_owner[64] = {0};
    bool owned = false;
    esp_err_t err = audio_output_get_external_owner(current_owner, sizeof(current_owner), &owned);
    if (err != ESP_OK) return false;
    if (!owned) return true;
    return strcmp(current_owner, owner) == 0;
}
