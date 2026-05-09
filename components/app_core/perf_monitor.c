#include "perf_monitor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#define TAG "perf_monitor"
#define PERF_REPORT_INTERVAL_MS 30000

static bool s_running = false;
static TaskHandle_t s_task = NULL;

static void perf_report_task(void* arg) {
    (void)arg;
    while (s_running) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "memory: internal=%lu largest=%lu spiram=%lu",
                 (unsigned long)internal_free,
                 (unsigned long)internal_largest,
                 (unsigned long)spiram_free);

        vTaskDelay(pdMS_TO_TICKS(PERF_REPORT_INTERVAL_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t perf_monitor_init(void) {
    return ESP_OK;
}

void perf_monitor_start(void) {
    if (s_running) return;
    s_running = true;
    xTaskCreate(perf_report_task, "perf_mon", 4096, NULL, 2, &s_task);
}

void perf_monitor_stop(void) {
    s_running = false;
}
