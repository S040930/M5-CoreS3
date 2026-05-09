#include "env_interface.h"
#include "env_monitor.h"
#include "esp_log.h"

static const char *TAG = "env_domain";

esp_err_t env_domain_init(void) {
    ESP_LOGI(TAG, "ENV domain initialized");
    return ESP_OK;
}

esp_err_t env_domain_start(void) {
    return env_monitor_start();
}

esp_err_t env_domain_stop(void) {
    env_monitor_stop();
    return ESP_OK;
}

esp_err_t env_domain_get_reading(float *temperature, float *humidity, float *pressure) {
    if (temperature == NULL || humidity == NULL || pressure == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!env_monitor_get_latest(temperature, humidity, pressure)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

bool env_domain_is_available(void) {
    float temp, hum, press;
    return env_monitor_get_latest(&temp, &hum, &press);
}
