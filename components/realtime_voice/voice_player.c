#include "voice_player.h"
#include "voice_internal_types.h"

#include "audio/audio_output.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "audio_ringbuf.h"
#include "voice_dsp.h"
#include "voice_playout.h"
#include "esp_heap_caps.h"

#include <string.h>

#define TAG "voice_player_v2"
#define VOICE_PLAYER_RING_MS 5000U
#define VOICE_PLAYER_PREFILL_MS 500U
#define SILENCE_TIMEOUT_MS 5000

static bool s_initialized = false;
static bool s_playing = false;
static bool s_prefilled = false;

static voice_player_event_cb_t s_event_cb = NULL;
static void* s_event_cb_user_data = NULL;

static audio_ringbuf_t* s_ringbuf = NULL;
static int16_t* s_hw_buf = NULL;
static int16_t* s_stereo_buf = NULL;
static int16_t* s_rs_out_buf = NULL;
static size_t s_hw_cap = 0;
static size_t s_stereo_cap = 0;
static size_t s_rs_out_cap = 0;

static uint32_t s_current_rate = 24000;
static uint32_t s_prefill_ms = VOICE_PLAYER_PREFILL_MS;

static TaskHandle_t s_play_task = NULL;
static StaticTask_t s_play_task_buf;
static StackType_t* s_play_task_stack = NULL;

static uint64_t s_last_audio_time_ms = 0;

#define DC_CORRECTION_ALPHA 0.995f
static float s_dc_offset = 0.0f;

#define VOICE_PLAYER_GAIN 1.5f

static void* playout_alloc(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return p;
}

static void playout_free(void* p) {
    if (p != NULL) {
        heap_caps_free(p);
    }
}

static void voice_player_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "Play task started");

    uint32_t hw_rate = voice_hw_codec_rate_hz();
    size_t chunk_frames = hw_rate * 20 / 1000;

    double rs_ratio = (s_current_rate > 0 && s_current_rate != hw_rate)
                          ? (double)hw_rate / (double)s_current_rate
                          : 1.0;
    size_t max_rs_frames = voice_rs_output_cap(chunk_frames, rs_ratio);

    if (s_hw_cap < chunk_frames) {
        if (s_hw_buf) playout_free(s_hw_buf);
        s_hw_buf = (int16_t*)playout_alloc(chunk_frames * sizeof(int16_t));
        s_hw_cap = chunk_frames;
    }

    if (s_rs_out_cap < max_rs_frames) {
        if (s_rs_out_buf) playout_free(s_rs_out_buf);
        s_rs_out_buf = (int16_t*)playout_alloc(max_rs_frames * sizeof(int16_t));
        s_rs_out_cap = max_rs_frames;
    }

    size_t stereo_need = max_rs_frames * VOICE_HW_CHANNELS;
    if (s_stereo_cap < stereo_need) {
        if (s_stereo_buf) playout_free(s_stereo_buf);
        s_stereo_buf = (int16_t*)playout_alloc(stereo_need * sizeof(int16_t));
        s_stereo_cap = stereo_need;
    }

    if (s_hw_buf == NULL || s_stereo_buf == NULL || s_rs_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate work buffers");
        s_playing = false;
        if (s_event_cb) {
            s_event_cb(VOICE_PLAYER_EVENT_ERROR, s_event_cb_user_data);
        }
        vTaskDelete(NULL);
        return;
    }

    voice_play_rs_reset();

    while (s_playing && audio_ringbuf_avail(s_ringbuf) < (s_prefill_ms * s_current_rate / 1000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_playing) {
        vTaskDelete(NULL);
        return;
    }

    s_prefilled = true;
    ESP_LOGI(TAG, "Prefill done, entering play loop: rate=%u hw_rate=%u buf_avail=%u",
             (unsigned)s_current_rate, (unsigned)hw_rate, (unsigned)audio_ringbuf_avail(s_ringbuf));
    if (s_event_cb) {
        s_event_cb(VOICE_PLAYER_EVENT_STARTED, s_event_cb_user_data);
    }

    while (s_playing) {
        size_t avail = audio_ringbuf_avail(s_ringbuf);
        uint64_t now = esp_timer_get_time() / 1000;

        if (avail == 0) {
            // Check silence timeout
            if (s_last_audio_time_ms > 0 && (now - s_last_audio_time_ms) > SILENCE_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Silence timeout, stopping playback");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t to_read = (avail < chunk_frames) ? avail : chunk_frames;
        size_t read = audio_ringbuf_pop(s_ringbuf, s_hw_buf, to_read);

        if (read == 0) {
            continue;
        }

        // Resample if needed
        int16_t* out_samples = s_hw_buf;
        size_t out_frames = read;

        if (s_current_rate != hw_rate) {
            if (voice_rs_play_rs() == NULL) {
                voice_pcm_format_t fmt = {
                    .sample_rate_hz = s_current_rate,
                    .channels = 1,
                    .bits_per_sample = 16
                };
                voice_playout_set_stream_format(&fmt);
                voice_rs_play_ensure();
            }
            out_frames = voice_rs_process_mono(voice_rs_play_rs(), voice_rs_play_ratio(),
                                               s_hw_buf, read, s_rs_out_buf, s_rs_out_cap);
            for (size_t i = 0; i < out_frames; i++) {
                s_stereo_buf[i * 2] = s_rs_out_buf[i];
                s_stereo_buf[i * 2 + 1] = s_rs_out_buf[i];
            }
            out_samples = s_stereo_buf;
        } else {
            for (size_t i = 0; i < read; i++) {
                s_stereo_buf[i * 2] = s_hw_buf[i];
                s_stereo_buf[i * 2 + 1] = s_hw_buf[i];
            }
            out_samples = s_stereo_buf;
            out_frames = read;
        }

        // Apply DC offset correction to prevent humming/noise
        for (size_t i = 0; i < out_frames; i++) {
            int16_t left = out_samples[i * 2];
            int16_t right = out_samples[i * 2 + 1];

            // Track DC offset using low-pass filter
            s_dc_offset = DC_CORRECTION_ALPHA * s_dc_offset + (1.0f - DC_CORRECTION_ALPHA) * (float)left;

            // Remove DC offset
            float left_f = (float)left - s_dc_offset;
            float right_f = (float)right - s_dc_offset;

            // Clamp to prevent clipping
            if (left_f > 32767.0f) left_f = 32767.0f;
            if (left_f < -32768.0f) left_f = -32768.0f;
            if (right_f > 32767.0f) right_f = 32767.0f;
            if (right_f < -32768.0f) right_f = -32768.0f;

            out_samples[i * 2] = (int16_t)left_f;
            out_samples[i * 2 + 1] = (int16_t)right_f;
        }

        for (size_t i = 0; i < out_frames; i++) {
            float left = (float)out_samples[i * 2] * VOICE_PLAYER_GAIN;
            float right = (float)out_samples[i * 2 + 1] * VOICE_PLAYER_GAIN;
            
            if (left > 32767.0f) left = 32767.0f;
            if (left < -32768.0f) left = -32768.0f;
            if (right > 32767.0f) right = 32767.0f;
            if (right < -32768.0f) right = -32768.0f;
            
            out_samples[i * 2] = (int16_t)left;
            out_samples[i * 2 + 1] = (int16_t)right;
        }

        // Play audio
        size_t write_bytes = out_frames * VOICE_HW_CHANNELS * sizeof(int16_t);
        esp_err_t write_ret = audio_output_external_write(out_samples, write_bytes, pdMS_TO_TICKS(40));
        if (write_ret != ESP_OK) {
            ESP_LOGW(TAG, "Write failed: %s frames=%u", esp_err_to_name(write_ret), (unsigned)out_frames);
        } else {
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "Play task stopping");

    // Cleanup and notify
    audio_output_external_close();
    audio_output_release_external("voice_v2", true);

    audio_ringbuf_reset(s_ringbuf);
    s_prefilled = false;
    s_playing = false;

    if (s_event_cb) {
        s_event_cb(VOICE_PLAYER_EVENT_STOPPED, s_event_cb_user_data);
    }

    ESP_LOGI(TAG, "Play task stopped");
    vTaskDelete(NULL);
}

esp_err_t voice_player_init(esp_codec_dev_handle_t spk_dev, voice_player_event_cb_t event_cb, void* user_data) {
    if (s_initialized) {
        return ESP_OK;
    }

    (void)spk_dev;
    s_event_cb = event_cb;
    s_event_cb_user_data = user_data;

    // Initialize ring buffer
    uint32_t sample_rate = CONFIG_VOICE_OUTPUT_SAMPLE_RATE;
    size_t samples = sample_rate * VOICE_PLAYER_RING_MS / 1000;
    if (samples < 1024) samples = 1024;

    s_ringbuf = audio_ringbuf_init_named(samples, "voice_play_v2");
    if (s_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to init ring buffer");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

void voice_player_deinit(void) {
    if (!s_initialized) {
        return;
    }

    voice_player_stop();

    s_dc_offset = 0.0f;

    if (s_ringbuf) {
        audio_ringbuf_deinit(s_ringbuf);
        s_ringbuf = NULL;
    }

    if (s_hw_buf) {
        playout_free(s_hw_buf);
        s_hw_buf = NULL;
    }

    if (s_stereo_buf) {
        playout_free(s_stereo_buf);
        s_stereo_buf = NULL;
    }

    if (s_rs_out_buf) {
        playout_free(s_rs_out_buf);
        s_rs_out_buf = NULL;
    }

    if (s_play_task_stack) {
        playout_free(s_play_task_stack);
        s_play_task_stack = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}

esp_err_t voice_player_start(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_playing) {
        return ESP_OK;
    }

    // Acquire speaker
    esp_err_t err = audio_output_acquire_external("voice_v2", true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire speaker: %s", esp_err_to_name(err));
        return err;
    }

    // Open audio output
    err = audio_output_external_open(voice_hw_codec_rate_hz());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open audio output: %s", esp_err_to_name(err));
        audio_output_release_external("voice_v2", true);
        return err;
    }

    // Allocate task stack
    if (s_play_task_stack == NULL) {
        s_play_task_stack = heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_play_task_stack == NULL) {
        s_play_task_stack = heap_caps_malloc(8192, MALLOC_CAP_8BIT);
            if (s_play_task_stack == NULL) {
                ESP_LOGE(TAG, "Failed to allocate task stack");
                audio_output_external_close();
                audio_output_release_external("voice_v2", true);
                return ESP_ERR_NO_MEM;
            }
        }
    }

    s_playing = true;
    s_prefilled = false;
    s_last_audio_time_ms = 0;

    /* CPU 0: keep CPU 1 for taskLVGL (BSP pins LVGL to core 1). Same-core
     * contention starves LVGL and breaks TWDT resets in anim_timer_cb. */
    s_play_task = xTaskCreateStaticPinnedToCore(
        voice_player_task, "voice_play_v2",
        8192 / sizeof(StackType_t), NULL,
        5, s_play_task_stack, &s_play_task_buf, 0);

    if (s_play_task == NULL) {
        ESP_LOGE(TAG, "Failed to create play task");
        s_playing = false;
        audio_output_external_close();
        audio_output_release_external("voice_v2", true);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Started");
    return ESP_OK;
}

esp_err_t voice_player_stop(void) {
    if (!s_playing) {
        return ESP_OK;
    }

    s_playing = false;
    s_dc_offset = 0.0f;

    if (s_play_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_play_task = NULL;
    }

    // Note: audio_output cleanup and event notification happens in the play task

    ESP_LOGI(TAG, "Stop requested");
    return ESP_OK;
}

esp_err_t voice_player_feed(const int16_t* pcm_data, size_t frames, uint32_t sample_rate) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pcm_data == NULL || frames == 0) {
        return ESP_OK;
    }

    s_current_rate = sample_rate;
    s_last_audio_time_ms = esp_timer_get_time() / 1000;

    size_t pushed = audio_ringbuf_push(s_ringbuf, pcm_data, frames);
    if (pushed < frames) {
        ESP_LOGW(TAG, "Ring buffer full: pushed %u/%u", (unsigned)pushed, (unsigned)frames);
    }

    return ESP_OK;
}

bool voice_player_is_active(void) {
    return s_playing;
}

bool voice_player_is_buffering(void) {
    if (!s_playing || s_prefilled) {
        return false;
    }
    return audio_ringbuf_avail(s_ringbuf) < (s_prefill_ms * s_current_rate / 1000);
}

size_t voice_player_available_frames(void) {
    if (!s_initialized || s_ringbuf == NULL) {
        return 0;
    }
    return audio_ringbuf_avail(s_ringbuf);
}

void voice_player_set_prebuffer_ms(uint32_t ms) {
    s_prefill_ms = ms;
}
