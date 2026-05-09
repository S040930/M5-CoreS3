#include "voice_frontend_v2.h"
#include "voice_internal_types.h"

#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "audio/audio_output.h"
#include "voice_common.h"

#include <string.h>

#define TAG "voice_fe_v2"

#define VOICE_FE_TASK_STACK 8192
#define VOICE_FE_TASK_PRIO 5
#if CONFIG_FREERTOS_UNICORE
#define VOICE_FE_TASK_CORE 0
#else
#define VOICE_FE_TASK_CORE 0
#endif

#define SIMPLE_VAD_THRESHOLD 200

#ifndef CONFIG_VOICE_MIC_IN_GAIN_DB
#define CONFIG_VOICE_MIC_IN_GAIN_DB 37
#endif

static bool s_initialized = false;
static bool s_running = false;
static bool s_paused = false;
static char s_pause_reason[64] = {0};

static esp_codec_dev_handle_t s_mic_dev = NULL;
static voice_frontend_event_cb_t s_event_cb = NULL;
static void* s_event_cb_user_data = NULL;

static TaskHandle_t s_fe_task = NULL;
static StaticTask_t s_fe_task_buf;
static StackType_t* s_fe_task_stack = NULL;

static int16_t* s_mono_buf = NULL;
static size_t s_mono_cap = 0;

static bool simple_vad(const int16_t* samples, size_t count) {
    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += labs(samples[i]);
    }
    int64_t avg = sum / count;
    return avg > SIMPLE_VAD_THRESHOLD;
}

static void log_fe_stack_watermark(void) {
    UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
    if (wm < 300) {
        ESP_LOGW(TAG, "stack critical: free_words=%lu", (unsigned long)wm);
    } else if (wm < 800) {
        ESP_LOGI(TAG, "stack low: free_words=%lu", (unsigned long)wm);
    }
}

static void voice_frontend_v2_task(void* arg) {
    (void)arg;

    ESP_LOGI(TAG, "frontend task started (no AFE)");

    const uint32_t hw_hz = voice_hw_codec_rate_hz();
    size_t frames_per_read = hw_hz * 20 / 1000;

    if (s_mono_cap < frames_per_read) {
        if (s_mono_buf) {
            heap_caps_free(s_mono_buf);
        }
        s_mono_buf = heap_caps_malloc(frames_per_read * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_mono_buf == NULL) {
            s_mono_buf = heap_caps_malloc(frames_per_read * sizeof(int16_t), MALLOC_CAP_8BIT);
        }
        if (s_mono_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mono buffer");
            s_running = false;
            vTaskDelete(NULL);
            return;
        }
        s_mono_cap = frames_per_read;
    }

    int16_t* stereo_buf = heap_caps_malloc(frames_per_read * VOICE_HW_CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (stereo_buf == NULL) {
        stereo_buf = heap_caps_malloc(frames_per_read * VOICE_HW_CHANNELS * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (stereo_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate stereo buffer");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_sample_info_t mic_cfg = {
        .bits_per_sample = 16,
        .channel = VOICE_HW_CHANNELS,
        .channel_mask = VOICE_HW_CHANNEL_MASK,
        .sample_rate = hw_hz,
        .mclk_multiple = voice_hw_mclk_multiple(hw_hz),
    };

    esp_err_t err = esp_codec_dev_open(s_mic_dev, &mic_cfg);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open mic: %s", esp_err_to_name(err));
        heap_caps_free(stereo_buf);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    audio_output_notify_i2s_rx(true);
    (void)esp_codec_dev_set_in_gain(s_mic_dev, (float)CONFIG_VOICE_MIC_IN_GAIN_DB);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "mic ready: rate=%lu channels=%d bits=%d",
             (unsigned long)mic_cfg.sample_rate, mic_cfg.channel, mic_cfg.bits_per_sample);

    while (s_running) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t want_bytes = frames_per_read * VOICE_HW_CHANNELS * sizeof(int16_t);
        int ret = esp_codec_dev_read(s_mic_dev, stereo_buf, want_bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (size_t i = 0; i < frames_per_read; i++) {
            int32_t l = stereo_buf[i * VOICE_HW_CHANNELS];
            int32_t r = stereo_buf[i * VOICE_HW_CHANNELS + 1];
            s_mono_buf[i] = (int16_t)((l + r) / 2);
        }

        bool vad = simple_vad(s_mono_buf, frames_per_read);

        voice_frontend_event_t event = {0};
        event.type = VOICE_FRONTEND_EVENT_AUDIO;
        event.pcm_data = s_mono_buf;
        event.pcm_frames = frames_per_read;
        event.vad_active = vad;

        if (s_event_cb) {
            s_event_cb(&event, s_event_cb_user_data);
        }

        static int loop_count = 0;
        if (++loop_count >= 500) {
            loop_count = 0;
            log_fe_stack_watermark();
        }

        vTaskDelay(2);
    }

    esp_codec_dev_close(s_mic_dev);
    heap_caps_free(stereo_buf);

    ESP_LOGI(TAG, "frontend task stopped");
    vTaskDelete(NULL);
}

esp_err_t voice_frontend_v2_prepare(void) {
    ESP_LOGI(TAG, "prepare: AFE not used (no-op)");
    return ESP_OK;
}

esp_err_t voice_frontend_v2_init(esp_codec_dev_handle_t mic_dev, voice_frontend_event_cb_t event_cb, void* user_data) {
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initializing (no AFE)");

    s_mic_dev = mic_dev;
    s_event_cb = event_cb;
    s_event_cb_user_data = user_data;

    s_initialized = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

void voice_frontend_v2_deinit(void) {
    if (!s_initialized) {
        return;
    }

    voice_frontend_v2_stop();

    if (s_mono_buf) {
        heap_caps_free(s_mono_buf);
        s_mono_buf = NULL;
        s_mono_cap = 0;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "deinitialized");
}

esp_err_t voice_frontend_v2_start(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        return ESP_OK;
    }

    s_running = true;
    s_paused = false;
    memset(s_pause_reason, 0, sizeof(s_pause_reason));

    if (s_fe_task_stack == NULL) {
        s_fe_task_stack = heap_caps_malloc(VOICE_FE_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_fe_task_stack == NULL) {
            s_fe_task_stack = heap_caps_malloc(VOICE_FE_TASK_STACK, MALLOC_CAP_8BIT);
            if (s_fe_task_stack == NULL) {
                ESP_LOGE(TAG, "Failed to allocate task stack");
                s_running = false;
                return ESP_ERR_NO_MEM;
            }
        }
    }

    s_fe_task = xTaskCreateStaticPinnedToCore(
        voice_frontend_v2_task, "voice_fe_v2",
        VOICE_FE_TASK_STACK / sizeof(StackType_t), NULL,
        VOICE_FE_TASK_PRIO, s_fe_task_stack, &s_fe_task_buf, VOICE_FE_TASK_CORE);

    if (s_fe_task == NULL) {
        ESP_LOGE(TAG, "Failed to create task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "starting task: stack=%d prio=%d core=%d", VOICE_FE_TASK_STACK, VOICE_FE_TASK_PRIO, VOICE_FE_TASK_CORE);
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t voice_frontend_v2_stop(void) {
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;

    if (s_fe_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_fe_task = NULL;
    }

    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

esp_err_t voice_frontend_v2_pause(const char* reason) {
    if (!s_running) {
        return ESP_OK;
    }

    s_paused = true;
    if (reason != NULL) {
        strncpy(s_pause_reason, reason, sizeof(s_pause_reason) - 1);
    }

    ESP_LOGI(TAG, "paused: %s", reason);
    return ESP_OK;
}

esp_err_t voice_frontend_v2_resume(void) {
    if (!s_running) {
        return ESP_OK;
    }

    s_paused = false;
    memset(s_pause_reason, 0, sizeof(s_pause_reason));

    ESP_LOGI(TAG, "resumed");
    return ESP_OK;
}

bool voice_frontend_v2_is_paused(void) {
    return s_paused;
}

bool voice_frontend_v2_is_running(void) {
    return s_running;
}

const char* voice_frontend_v2_last_pause_reason(void) {
    return s_pause_reason;
}
