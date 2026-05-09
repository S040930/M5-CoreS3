#include "airplay_interface.h"
#include "airplay_service.h"
#include "esp_log.h"

static const char *TAG = "airplay_domain";
static airplay_active_callback_t s_active_cb;

esp_err_t airplay_domain_init(void) {
    ESP_LOGI(TAG, "AirPlay domain initialized");
    return ESP_OK;
}

esp_err_t airplay_domain_start(const char *device_name) {
    return airplay_service_start(device_name);
}

esp_err_t airplay_domain_stop(void) {
    airplay_service_stop();
    return ESP_OK;
}

esp_err_t airplay_domain_refresh_playback(void) {
    airplay_service_refresh_playback();
    return ESP_OK;
}

bool airplay_domain_is_active(void) {
    return airplay_service_is_active();
}

esp_err_t airplay_domain_set_active_callback(airplay_active_callback_t callback) {
    s_active_cb = callback;
    airplay_service_set_active_callback((airplay_service_active_cb_t)callback);
    return ESP_OK;
}
