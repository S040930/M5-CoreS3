#include "voice_frontend_v2.h"
#include "voice_internal_types.h"

#include "esp_log.h"
#include "esp_afe_sr_iface.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "audio/audio_output.h"
#include "voice_common.h"
#include "afe_bridge.h"

#include <string.h>

#define TAG "voice_frontend_v2"

#define VOICE_FE_TASK_STACK 8192
#define VOICE_FE_TASK_PRIO 5
#if CONFIG_FREERTOS_UNICORE
#define VOICE_FE_TASK_CORE 0
#else
#define VOICE_FE_TASK_CORE 1
#endif

#define VOICE_ONESHOT_SAMPLE_RATE 16000
#define VOICE_FE_MAX_PCM_FRAMES 320

#define VOICE_HW_CHANNELS 2
#define VOICE_HW_CHANNEL_MASK 0x03
#ifndef CONFIG_VOICE_MIC_IN_GAIN_DB
#define CONFIG_VOICE_MIC_IN_GAIN_DB 33
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

#define VOICE_WAKE_MODEL_NAME "wn9_hiesp"

static void voice_frontend_v2_task(void* arg) {
    (void)arg;

    ESP_LOGI(TAG, "frontend task started");

    int feed_ch = afe_bridge_get_feed_chunksize();
    int fetch_ch = afe_bridge_get_fetch_chunksize();
    int sample_rate = afe_bridge_get_sample_rate();

    ESP_LOGI(TAG, "AFE feed=%d fetch=%d rate=%d", feed_ch, fetch_ch, sample_rate);

    int16_t* feed_buf = (int16_t*)malloc(feed_ch * 2 * sizeof(int16_t));

    if (feed_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate feed buffer");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    const uint32_t hw_hz = voice_hw_codec_rate_hz();
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
        free(feed_buf);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    audio_output_notify_i2s_rx(true);
    (void)esp_codec_dev_set_in_gain(s_mic_dev, (float)CONFIG_VOICE_MIC_IN_GAIN_DB);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "persistent mic ready: rate=%lu channels=%d bits=%d",
             (unsigned long)mic_cfg.sample_rate, mic_cfg.channel, mic_cfg.bits_per_sample);

    while (s_running) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int want = (int)(feed_ch * VOICE_HW_CHANNELS * sizeof(int16_t));
        int ret = esp_codec_dev_read(s_mic_dev, feed_buf, want);
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t got_samples = ((size_t)want / sizeof(int16_t)) / VOICE_HW_CHANNELS;
        if (got_samples == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Convert from stereo to mono by averaging left and right channels
        for (size_t i = 0; i < got_samples; ++i) {
            int32_t l = feed_buf[i * VOICE_HW_CHANNELS];
            int32_t r = feed_buf[i * VOICE_HW_CHANNELS + 1];
            feed_buf[i * 2 + 0] = (int16_t)((l + r) / 2);
            feed_buf[i * 2 + 1] = 0; // Reference channel is silence
        }

        afe_bridge_feed(feed_buf, got_samples);

        afe_fetch_result_t* res = afe_bridge_fetch(0);
        if (res != NULL && res->data_size > 0) {
            voice_frontend_event_t event = {0};

            if (res->wakeup_state == WAKENET_DETECTED) {
                event.type = VOICE_FRONTEND_EVENT_WAKE;
                ESP_LOGI(TAG, "wake word detected");
            } else {
                event.type = VOICE_FRONTEND_EVENT_AUDIO;
            }

            event.pcm_data = res->data;
            event.pcm_frames = res->data_size;
            event.vad_active = res->vad_state == VAD_SPEECH;

            if (s_event_cb) {
                s_event_cb(&event, s_event_cb_user_data);
            }
        }

        // Cooperative yield: always yield at least 1 OS tick.
        vTaskDelay(1);
    }

    esp_codec_dev_close(s_mic_dev);
    free(feed_buf);

    ESP_LOGI(TAG, "frontend task stopped");
    vTaskDelete(NULL);
}

esp_err_t voice_frontend_v2_init(esp_codec_dev_handle_t mic_dev, voice_frontend_event_cb_t event_cb, void* user_data) {
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initializing");

    s_mic_dev = mic_dev;
    s_event_cb = event_cb;
    s_event_cb_user_data = user_data;

    esp_err_t err = afe_bridge_init(VOICE_WAKE_MODEL_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "afe_bridge_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "initialized");

    return ESP_OK;
}

void voice_frontend_v2_deinit(void) {
    if (!s_initialized) {
        return;
    }

    voice_frontend_v2_stop();
    afe_bridge_deinit();

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
        s_fe_task_stack = heap_caps_malloc(VOICE_FE_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

    afe_bridge_reset();

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

    afe_bridge_reset();

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
