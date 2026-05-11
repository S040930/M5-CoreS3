#include "auto_brightness.h"
#include "ltr553.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define TAG "auto_bright"

#define POLL_INTERVAL_MS       10000
#define OVERRIDE_PAUSE_MS      30000
#define BRIGHTNESS_HYSTERESIS  2
#define SMOOTH_FACTOR_NUM      1
#define SMOOTH_FACTOR_DEN      2

typedef struct {
    uint16_t als_max;
    int brightness_min;
    int brightness_max;
} lux_segment_t;

static const lux_segment_t s_segments[] = {
    {5,    5, 20},
    {30,   20, 40},
    {100,  40, 60},
    {300,  60, 80},
    {800,  80, 95},
    {0xFFFF, 95, 100},
};
#define SEGMENT_COUNT (sizeof(s_segments) / sizeof(s_segments[0]))

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static auto_brightness_set_fn_t s_set_brightness = NULL;
static TaskHandle_t s_task = NULL;
static bool s_running = false;
static int s_current_brightness = -1;
static int64_t s_override_until_ms = 0;
static bool s_ltr553_ok = false;

static int64_t now_ms(void) { return (int64_t)pdTICKS_TO_MS(xTaskGetTickCount()); }

static int als_to_brightness(uint16_t ch0) {
    for (int i = 0; i < (int)SEGMENT_COUNT; i++) {
        if (ch0 <= s_segments[i].als_max || s_segments[i].als_max == 0xFFFF) {
            uint16_t seg_max = s_segments[i].als_max;
            int bmin = s_segments[i].brightness_min;
            int bmax = s_segments[i].brightness_max;
            if (i == 0) {
                return bmin;
            }
            uint16_t seg_min = s_segments[i - 1].als_max;
            if (ch0 <= seg_min) return bmin;
            if (seg_max == seg_min) return bmin;
            int range = bmax - bmin;
            uint32_t num = (uint32_t)(ch0 - seg_min) * (uint32_t)range;
            uint32_t den = (uint32_t)(seg_max - seg_min);
            return bmin + (int)(num / den);
        }
    }
    return 100;
}

static void auto_brightness_task(void *arg) {
    ESP_LOGI(TAG, "task started, poll=%dms", POLL_INTERVAL_MS);

    while (s_running) {
        int64_t t = now_ms();
        if (s_override_until_ms > 0 && t < s_override_until_ms) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }
        s_override_until_ms = 0;

        uint16_t ch0 = 0, ch1 = 0;
        esp_err_t err = ltr553_read_als(&ch0, &ch1);
        if (err == ESP_ERR_NOT_FOUND) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ALS read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS * 2));
            continue;
        }

        int target = als_to_brightness(ch0);

        if (s_current_brightness < 0) {
            s_current_brightness = target;
        } else {
            s_current_brightness = (s_current_brightness * SMOOTH_FACTOR_NUM + target) /
                                   (SMOOTH_FACTOR_NUM + 1);
        }

        static int s_last_applied = -1;
        if (s_last_applied < 0 ||
            abs(s_current_brightness - s_last_applied) >= BRIGHTNESS_HYSTERESIS) {
            if (s_set_brightness != NULL) {
                err = s_set_brightness(s_current_brightness);
                if (err == ESP_OK) {
                    s_last_applied = s_current_brightness;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "task stopped");
    vTaskDelete(NULL);
    s_task = NULL;
}

esp_err_t auto_brightness_start(i2c_master_bus_handle_t i2c_bus,
                                 auto_brightness_set_fn_t set_brightness_fn) {
    if (i2c_bus == NULL || set_brightness_fn == NULL) return ESP_ERR_INVALID_ARG;
    if (s_running) return ESP_OK;

    s_i2c_bus = i2c_bus;
    s_set_brightness = set_brightness_fn;
    s_current_brightness = -1;
    s_override_until_ms = 0;

    esp_err_t err = ltr553_init(s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LTR-553 init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ltr553_ok = true;

    s_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(auto_brightness_task, "auto_bright",
                                              3072, NULL, 2, &s_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_running = false;
        ltr553_deinit();
        s_ltr553_ok = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void auto_brightness_stop(void) {
    if (!s_running) return;
    s_running = false;
    if (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS + 100));
    }
    ltr553_deinit();
    s_ltr553_ok = false;
    s_i2c_bus = NULL;
    s_set_brightness = NULL;
}

void auto_brightness_notify_manual_override(void) {
    s_override_until_ms = now_ms() + OVERRIDE_PAUSE_MS;
    ESP_LOGI(TAG, "manual override, pausing for %dms", OVERRIDE_PAUSE_MS);
}
