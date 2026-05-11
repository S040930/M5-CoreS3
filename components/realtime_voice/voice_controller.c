#include "voice_controller.h"
#include "voice_frontend_v2.h"
#include "omni_client.h"
#include "voice_player.h"
#include "voice_internal_types.h"
#include "voice_common.h"
#include "voice_dsp.h"
#include "voice_playout.h"
#include "voice_speaker.h"
#include "voice_events.h"
#include "audio/audio_output.h"
#include "audio/audio_output_common.h"
#include "audio/audio_volume.h"
#include "resource/resource_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>

#define TAG "voice_ctrl"

#define CONTROLLER_TASK_STACK 16384
#define CONTROLLER_TASK_PRIO 4
#define VOICE_ONESHOT_RECORD_MAX_MS 6000
#define VOICE_RECORD_SILENCE_TIMEOUT_MS 2500
#define VOICE_SAMPLE_RATE 44100
#define VAD_CONSEC_FRAMES 1

#define STATE_TIMEOUT_REQUESTING_MS 30000
#define STATE_TIMEOUT_PLAYING_MS     60000
#define AIRPLAY_POLL_INTERVAL_MS     200

/* Buffer full Omni TTS clip (mono @ ~24kHz) then play once — avoids stream gaps. */
#define OMNI_TTS_MAX_ACCUM_FRAMES (24000u * 120u)
#define OMNI_TTS_RING_MARGIN_FRAMES 48000u

static int16_t *s_tts_accum;
static size_t s_tts_accum_frames;
static size_t s_tts_accum_cap;
static uint32_t s_tts_accum_rate_hz;

static void tts_accum_reset(void) {
    heap_caps_free(s_tts_accum);
    s_tts_accum = NULL;
    s_tts_accum_frames = 0;
    s_tts_accum_cap = 0;
    s_tts_accum_rate_hz = 24000;
}

static void tts_accum_append(const int16_t *pcm, size_t frames, uint32_t sample_rate) {
    if (pcm == NULL || frames == 0) {
        return;
    }
    if (s_tts_accum_frames == 0) {
        s_tts_accum_rate_hz = sample_rate;
    } else if (sample_rate != s_tts_accum_rate_hz) {
        ESP_LOGW(TAG, "Omni TTS sample rate changed (%lu vs %lu), drop chunk",
                 (unsigned long)sample_rate, (unsigned long)s_tts_accum_rate_hz);
        return;
    }
    size_t next_total = s_tts_accum_frames + frames;
    if (next_total > OMNI_TTS_MAX_ACCUM_FRAMES) {
        ESP_LOGW(TAG, "Omni TTS buffer cap (~120s @24k), truncating");
        frames = (s_tts_accum_frames < OMNI_TTS_MAX_ACCUM_FRAMES)
                     ? (OMNI_TTS_MAX_ACCUM_FRAMES - s_tts_accum_frames)
                     : 0;
        next_total = s_tts_accum_frames + frames;
        if (frames == 0) {
            return;
        }
    }
    if (next_total > s_tts_accum_cap) {
        size_t new_cap = s_tts_accum_cap ? s_tts_accum_cap : 8192;
        while (new_cap < next_total) {
            new_cap *= 2;
        }
        if (new_cap > OMNI_TTS_MAX_ACCUM_FRAMES) {
            new_cap = OMNI_TTS_MAX_ACCUM_FRAMES;
        }
        int16_t *nb =
            (int16_t *)heap_caps_realloc(s_tts_accum, new_cap * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            nb = (int16_t *)realloc(s_tts_accum, new_cap * sizeof(int16_t));
        }
        if (nb == NULL) {
            ESP_LOGE(TAG, "tts accum realloc failed");
            return;
        }
        s_tts_accum = nb;
        s_tts_accum_cap = new_cap;
    }
    memcpy(s_tts_accum + s_tts_accum_frames, pcm, frames * sizeof(int16_t));
    s_tts_accum_frames += frames;
}

extern voice_ui_state_cb_t realtime_voice_get_ui_state_cb(void);
extern voice_ui_network_busy_cb_t realtime_voice_get_ui_network_busy_cb(void);
extern voice_airplay_refresh_cb_t realtime_voice_get_airplay_refresh_cb(void);
extern voice_network_query_cb_t realtime_voice_get_network_query_cb(void);

typedef enum {
    CTRL_STATE_IDLE = 0,
    CTRL_STATE_RECORDING,
    CTRL_STATE_REQUESTING,
    CTRL_STATE_PLAYING,
} ctrl_state_t;

static struct {
    bool initialized;
    bool running;
    bool enabled;
    bool armed;
    bool interrupt_requested;

    ctrl_state_t state;
    device_mode_t mode;

    realtime_voice_config_t config;

    esp_codec_dev_handle_t mic_dev;
    esp_codec_dev_handle_t spk_dev;

    TaskHandle_t task;
    StaticTask_t task_buf;
    StackType_t* task_stack;

    int16_t* record_buf;
    size_t record_cap;
    size_t record_pos;
    bool recording;
    uint64_t record_start_ms;
    uint32_t vad_hit_count;
    bool vad_active_last;

    char last_user[64];
    char last_assistant[512];

    bool user_speech_notified;
    uint64_t state_enter_ms;
    SemaphoreHandle_t state_lock;
} s = {0};

static void set_ui_state(int state, const char* user, const char* assistant, const char* error) {
    voice_ui_state_cb_t cb = realtime_voice_get_ui_state_cb();
    if (cb) {
        cb(state, user, assistant, error);
    }
}

static void notify_network_busy(bool busy) {
    voice_ui_network_busy_cb_t cb = realtime_voice_get_ui_network_busy_cb();
    if (cb) {
        cb(busy);
    }
}

static bool is_network_ready(void) {
    voice_network_query_cb_t cb = realtime_voice_get_network_query_cb();
    if (cb == NULL) {
        return false;
    }
    voice_network_snapshot_t snap = {0};
    cb(&snap);
    return snap.network_ready || snap.discoverable;
}

static bool is_airplay_active_now(void) {
    return resource_manager_is_airplay_active();
}

static void frontend_event_cb(const voice_frontend_event_t* event, void* user_data) {
    (void)user_data;

    if (s.state_lock) xSemaphoreTake(s.state_lock, portMAX_DELAY);
    bool airplay_active = is_airplay_active_now();
    bool recording = s.recording;
    if (s.state_lock) xSemaphoreGive(s.state_lock);

    if (airplay_active) {
        return;
    }

    if (event->type == VOICE_FRONTEND_EVENT_WAKE) {
        if (s.state_lock) xSemaphoreTake(s.state_lock, portMAX_DELAY);
        if (!s.armed) {
            s.armed = true;
            s.recording = true;
            s.record_pos = 0;
            s.record_start_ms = esp_timer_get_time() / 1000;
            s.vad_hit_count = 0;
            if (s.state_lock) xSemaphoreGive(s.state_lock);

            snprintf(s.last_user, sizeof(s.last_user), "%s", "[recording]");
            set_ui_state(REALTIME_VOICE_STATE_LISTENING, s.last_user, NULL, NULL);
            voice_publish_wakeword_detected();
            ESP_LOGI(TAG, "Wake detected, started recording");
        } else {
            if (s.state_lock) xSemaphoreGive(s.state_lock);
        }
    } else if (event->type == VOICE_FRONTEND_EVENT_AUDIO) {
        if (recording && s.record_pos < s.record_cap) {
            size_t copy_frames = event->pcm_frames;
            if (s.record_pos + copy_frames > s.record_cap) {
                copy_frames = s.record_cap - s.record_pos;
            }
            memcpy(s.record_buf + s.record_pos, event->pcm_data, copy_frames * sizeof(int16_t));
            s.record_pos += copy_frames;

            if (event->vad_active) {
                s.vad_hit_count++;
            } else {
                s.vad_hit_count = 0;
            }
        }
    }
}

static void player_event_cb(voice_player_event_type_t event, void* user_data) {
    (void)user_data;

    if (event == VOICE_PLAYER_EVENT_STOPPED) {
        ESP_LOGI(TAG, "Playback stopped");
        voice_publish_playback_end();
        voice_airplay_refresh_cb_t refresh_cb = realtime_voice_get_airplay_refresh_cb();
        if (refresh_cb) {
            refresh_cb();
        }
        if (s.state_lock) xSemaphoreTake(s.state_lock, portMAX_DELAY);
        s.state = CTRL_STATE_IDLE;
        s.state_enter_ms = esp_timer_get_time() / 1000;
        if (s.state_lock) xSemaphoreGive(s.state_lock);
        set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
        audio_output_common_apply_context_fidelity(
            resource_manager_is_airplay_active(), false);
    }
}

static void omni_on_audio_delta(const int16_t *pcm, size_t frames, uint32_t sample_rate, void *user_data) {
    (void)user_data;
    tts_accum_append(pcm, frames, sample_rate);
}

static void handle_volume_command(const char *text) {
    if (text == NULL || text[0] == '\0') return;

    float current_vol = 0.0f;
    audio_volume_get(&current_vol);
    bool adjusted = false;

    /* Detect volume up intent (Chinese patterns) */
    if (strstr(text, "调大") || strstr(text, "大声") ||
        strstr(text, "音量高") || strstr(text, "音量提高") ||
        strstr(text, "声音大")) {
        float new_vol = current_vol + 3.0f;
        if (new_vol > 0.0f) new_vol = 0.0f;
        audio_volume_save(new_vol);
        ESP_LOGI(TAG, "volume up via voice: %.1f -> %.1f dB", current_vol, new_vol);
        adjusted = true;
    }
    /* Detect volume down intent (Chinese patterns) */
    else if (strstr(text, "调小") || strstr(text, "小声") ||
             strstr(text, "音量低") || strstr(text, "音量降低") ||
             strstr(text, "声音小")) {
        float new_vol = current_vol - 3.0f;
        if (new_vol < -30.0f) new_vol = -30.0f;
        audio_volume_save(new_vol);
        ESP_LOGI(TAG, "volume down via voice: %.1f -> %.1f dB", current_vol, new_vol);
        adjusted = true;
    }
    /* Detect mute intent */
    else if (strstr(text, "静音") || strstr(text, "关闭声音") ||
             strstr(text, "mute") || strstr(text, "Mute")) {
        audio_volume_save(-30.0f);
        ESP_LOGI(TAG, "volume muted via voice: %.1f -> -30.0 dB", current_vol);
        adjusted = true;
    }

    if (adjusted) {
        /* Ensure the audio pipeline is adjusted for the new volume immediately */
        audio_output_set_target_volume_db(current_vol > 0.0f ? 0.0f :
                                           current_vol < -30.0f ? -30.0f : current_vol);
    }
}

static void omni_on_text_delta(const char *text, size_t len, void *user_data) {
    (void)user_data;
    if (len > 0 && text != NULL) {
        /* Check and handle volume commands embedded in the AI text response */
        handle_volume_command(text);

        /* SSE sends incremental string deltas; must append, not overwrite. */
        size_t cur = strnlen(s.last_assistant, sizeof(s.last_assistant));
        size_t room = (cur < sizeof(s.last_assistant) - 1) ? (sizeof(s.last_assistant) - 1 - cur) : 0;
        if (room > 0) {
            size_t copy = len < room ? len : room;
            memcpy(s.last_assistant + cur, text, copy);
            s.last_assistant[cur + copy] = '\0';
        }
        ESP_LOGD(TAG, "text delta: %.*s", (int)len, text);
        if (s.state == CTRL_STATE_REQUESTING) {
            set_ui_state(REALTIME_VOICE_STATE_THINKING, s.last_user, s.last_assistant, NULL);
        } else if (s.state == CTRL_STATE_PLAYING) {
            set_ui_state(REALTIME_VOICE_STATE_SPEAKING, s.last_user, s.last_assistant, NULL);
        }
    }
}

static void omni_on_response_done(void *user_data) {
    (void)user_data;
    ESP_LOGI(TAG, "Omni response done (assistant text): %s",
             s.last_assistant[0] ? s.last_assistant : "(none)");

    if (s_tts_accum_frames > 0 && s_tts_accum != NULL) {
        ESP_LOGI(TAG, "TTS buffered playback: %u frames @ %lu Hz",
                 (unsigned)s_tts_accum_frames, (unsigned long)s_tts_accum_rate_hz);
        esp_err_t res =
            voice_player_reserve_pcm_capacity(s_tts_accum_frames + OMNI_TTS_RING_MARGIN_FRAMES);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "voice_player_reserve_pcm_capacity failed: %s", esp_err_to_name(res));
            tts_accum_reset();
            s.state = CTRL_STATE_IDLE;
            s.state_enter_ms = esp_timer_get_time() / 1000;
            set_ui_state(REALTIME_VOICE_STATE_ERROR, NULL, NULL, "Playback buffer");
            return;
        }
        esp_err_t err = voice_player_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "voice_player_start failed: %s", esp_err_to_name(err));
            tts_accum_reset();
            s.state = CTRL_STATE_IDLE;
            s.state_enter_ms = esp_timer_get_time() / 1000;
            set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
            return;
        }
        voice_player_feed_blocking(s_tts_accum, s_tts_accum_frames, s_tts_accum_rate_hz);
        voice_player_mark_upstream_complete();
        s.state = CTRL_STATE_PLAYING;
        s.state_enter_ms = esp_timer_get_time() / 1000;
        set_ui_state(REALTIME_VOICE_STATE_SPEAKING, s.last_user, s.last_assistant, NULL);
        tts_accum_reset();
    } else {
        s.state = CTRL_STATE_IDLE;
        s.state_enter_ms = esp_timer_get_time() / 1000;
        set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
    }
}

static void omni_on_error(esp_err_t err, const char *message, void *user_data) {
    (void)user_data;
    ESP_LOGE(TAG, "Omni error: %s (%s)", message ? message : "unknown", esp_err_to_name(err));
    tts_accum_reset();
}

static void log_stack_watermark(const char* reason) {
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    if (watermark < 500) {
        ESP_LOGW(TAG, "stack low[%s]: free_words=%lu", reason, (unsigned long)watermark);
    } else {
        ESP_LOGD(TAG, "stack watermark[%s]: free_words=%lu", reason, (unsigned long)watermark);
    }
}

static void controller_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "Controller task started");

    s.record_cap = (VOICE_SAMPLE_RATE * VOICE_ONESHOT_RECORD_MAX_MS) / 1000;
    s.record_buf = heap_caps_malloc(s.record_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s.record_buf == NULL) {
        s.record_buf = heap_caps_malloc(s.record_cap * sizeof(int16_t), MALLOC_CAP_8BIT);
        if (s.record_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate record buffer");
            s.running = false;
            vTaskDelete(NULL);
            return;
        }
    }

    voice_frontend_v2_init(s.mic_dev, frontend_event_cb, NULL);
    voice_player_init(s.spk_dev, player_event_cb, NULL);

    omni_client_config_t omni_cfg = {0};
    strncpy(omni_cfg.api_key, s.config.api_key, sizeof(omni_cfg.api_key) - 1);
    strncpy(omni_cfg.model, s.config.model, sizeof(omni_cfg.model) - 1);
    strncpy(omni_cfg.voice, s.config.voice, sizeof(omni_cfg.voice) - 1);
    strncpy(omni_cfg.instructions, s.config.instructions, sizeof(omni_cfg.instructions) - 1);

    omni_client_callbacks_t omni_cbs = {
        .user_data = NULL,
        .on_text_delta = omni_on_text_delta,
        .on_audio_delta = omni_on_audio_delta,
        .on_response_done = omni_on_response_done,
        .on_error = omni_on_error,
    };
    omni_client_init(&omni_cfg, &omni_cbs);

    s.state = CTRL_STATE_IDLE;
    s.state_enter_ms = esp_timer_get_time() / 1000;
    set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);

    while (s.running) {
        if (!is_network_ready()) {
            if (voice_frontend_v2_is_running()) {
                voice_frontend_v2_pause("no_network");
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        bool airplay_active = is_airplay_active_now();
        if (airplay_active) {
            s.mode = DEVICE_MODE_AIRPLAY;
            if (voice_frontend_v2_is_running()) {
                voice_frontend_v2_pause("airplay_active");
            }
            if (voice_player_is_active()) {
                voice_player_stop();
                voice_airplay_refresh_cb_t refresh_cb = realtime_voice_get_airplay_refresh_cb();
                if (refresh_cb) {
                    refresh_cb();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(AIRPLAY_POLL_INTERVAL_MS));
            continue;
        }

        s.mode = DEVICE_MODE_VOICE;

        if (!voice_frontend_v2_is_running()) {
            voice_frontend_v2_start();
        } else if (voice_frontend_v2_is_paused()) {
            voice_frontend_v2_resume();
        }

        /* PTT-only mode: recording is triggered by screen tap
           (voice_controller_notify_user_speech_start) which sets
           s.user_speech_notified = true, handled below. */

        if (s.user_speech_notified) {
            s.user_speech_notified = false;
            s.vad_hit_count = VAD_CONSEC_FRAMES + 1;
            voice_publish_speech_start();
        }

        if (s.recording) {
            uint64_t now = esp_timer_get_time() / 1000;
            uint64_t elapsed = now - s.record_start_ms;

            bool should_end = (elapsed > VOICE_ONESHOT_RECORD_MAX_MS) ||
                            (s.vad_hit_count == 0 && elapsed > VOICE_RECORD_SILENCE_TIMEOUT_MS);

            if (should_end && s.record_pos > 0) {
                ESP_LOGI(TAG, "Recording ended, %u frames", s.record_pos);
                voice_publish_speech_end();

                s.recording = false;
                s.armed = false;

                audio_output_common_apply_context_fidelity(
                    resource_manager_is_airplay_active(), true);

                voice_frontend_v2_pause("uploading");

                s.state = CTRL_STATE_REQUESTING;
                s.state_enter_ms = esp_timer_get_time() / 1000;
                set_ui_state(REALTIME_VOICE_STATE_THINKING, s.last_user, NULL, NULL);
                notify_network_busy(true);

                s.last_assistant[0] = '\0';
                tts_accum_reset();

                voice_audio_request_t req = {
                    .pcm_data = s.record_buf,
                    .pcm_frames = s.record_pos,
                    .sample_rate = VOICE_SAMPLE_RATE
                };

                esp_err_t err = omni_client_send_audio(
                    req.pcm_data, req.pcm_frames, req.sample_rate);

                voice_publish_request_send(NULL);
                notify_network_busy(false);

                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Omni request failed: %s", esp_err_to_name(err));
                    tts_accum_reset();
                    set_ui_state(REALTIME_VOICE_STATE_ERROR, NULL, NULL, "API request failed");
                    voice_player_stop();
                    s.state = CTRL_STATE_IDLE;
                    s.state_enter_ms = esp_timer_get_time() / 1000;
                }

                s.record_pos = 0;
                s.vad_hit_count = 0;

                if (s.state == CTRL_STATE_PLAYING) {
                    bool player_active = voice_player_is_active();
                    if (!player_active) {
                        ESP_LOGI(TAG, "Player not active after request, going idle");
                        s.state = CTRL_STATE_IDLE;
                        s.state_enter_ms = esp_timer_get_time() / 1000;
                        set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
                    }
                }

                if (!voice_frontend_v2_is_running()) {
                    voice_frontend_v2_start();
                }
            }
        }

        if (s.interrupt_requested) {
            s.interrupt_requested = false;
            if (s.state == CTRL_STATE_PLAYING || s.state == CTRL_STATE_REQUESTING) {
                ESP_LOGI(TAG, "Interrupting response");
                tts_accum_reset();
                voice_player_stop();
                s.state = CTRL_STATE_IDLE;
                s.state_enter_ms = esp_timer_get_time() / 1000;
                set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
            }
        }

        uint64_t now_ms = esp_timer_get_time() / 1000;
        uint64_t state_elapsed = now_ms - s.state_enter_ms;
        if (s.state == CTRL_STATE_REQUESTING && state_elapsed > STATE_TIMEOUT_REQUESTING_MS) {
            ESP_LOGE(TAG, "REQUESTING state timeout (%llums > %dms), aborting",
                     (unsigned long long)state_elapsed, STATE_TIMEOUT_REQUESTING_MS);
            omni_client_deinit();
            tts_accum_reset();
            voice_player_stop();
            s.state = CTRL_STATE_IDLE;
            s.state_enter_ms = now_ms;
            s.record_pos = 0;
            s.vad_hit_count = 0;
            set_ui_state(REALTIME_VOICE_STATE_ERROR, NULL, NULL, "API timeout");
            notify_network_busy(false);
        } else if (s.state == CTRL_STATE_PLAYING && state_elapsed > STATE_TIMEOUT_PLAYING_MS) {
            ESP_LOGW(TAG, "PLAYING state timeout (%llums > %dms), stopping",
                     (unsigned long long)state_elapsed, STATE_TIMEOUT_PLAYING_MS);
            voice_player_stop();
            s.state = CTRL_STATE_IDLE;
            s.state_enter_ms = now_ms;
            set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
        }

        static uint32_t stack_check_counter = 0;
        if (++stack_check_counter >= 500) {
            stack_check_counter = 0;
            log_stack_watermark("ctrl");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    voice_frontend_v2_stop();
    voice_frontend_v2_deinit();
    tts_accum_reset();
    voice_player_stop();
    voice_player_deinit();
    omni_client_deinit();

    if (s.record_buf) {
        heap_caps_free(s.record_buf);
        s.record_buf = NULL;
    }

    s.task = NULL;
    s.state = CTRL_STATE_IDLE;
    ESP_LOGI(TAG, "Controller task stopped");
    vTaskDelete(NULL);
}

esp_err_t voice_controller_init(const realtime_voice_config_t* config,
                                esp_codec_dev_handle_t mic_dev,
                                esp_codec_dev_handle_t spk_dev) {
    if (s.initialized) {
        return ESP_OK;
    }

    if (config != NULL) {
        memcpy(&s.config, config, sizeof(s.config));
    }

    s.mic_dev = mic_dev;
    s.spk_dev = spk_dev;

    esp_codec_set_disable_when_closed(s.mic_dev, false);

    voice_speaker_set_handles(s.mic_dev, s.spk_dev);

    voice_playout_init(0);

    s.state_lock = xSemaphoreCreateMutex();
    if (s.state_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create state lock");
        return ESP_ERR_NO_MEM;
    }

    if (s.config.url[0] == '\0' && strlen(CONFIG_VOICE_REALTIME_URL) > 0) {
        strncpy(s.config.url, CONFIG_VOICE_REALTIME_URL, sizeof(s.config.url) - 1);
    }
    if (s.config.api_key[0] == '\0' && strlen(CONFIG_VOICE_API_KEY) > 0) {
        strncpy(s.config.api_key, CONFIG_VOICE_API_KEY, sizeof(s.config.api_key) - 1);
    }
    if (s.config.model[0] == '\0' && strlen(CONFIG_VOICE_MODEL) > 0) {
        strncpy(s.config.model, CONFIG_VOICE_MODEL, sizeof(s.config.model) - 1);
    }

    // omni_client_init is called in controller_task with proper callbacks

    s.initialized = true;
    s.enabled = true;
    s.mode = DEVICE_MODE_AIRPLAY;

    ESP_LOGI(TAG, "Controller initialized");
    return ESP_OK;
}

void voice_controller_deinit(void) {
    if (!s.initialized) {
        return;
    }

    voice_controller_stop();

    if (s.task_stack) {
        heap_caps_free(s.task_stack);
        s.task_stack = NULL;
    }

    if (s.state_lock) {
        vSemaphoreDelete(s.state_lock);
        s.state_lock = NULL;
    }

    s.initialized = false;
    ESP_LOGI(TAG, "Controller deinitialized");
}

esp_err_t voice_controller_start(void) {
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s.running) {
        return ESP_OK;
    }

    if (s.task_stack == NULL) {
        s.task_stack = heap_caps_malloc(CONTROLLER_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s.task_stack == NULL) {
            s.task_stack = heap_caps_malloc(CONTROLLER_TASK_STACK, MALLOC_CAP_8BIT);
            if (s.task_stack == NULL) {
                ESP_LOGE(TAG, "Failed to allocate task stack");
                return ESP_ERR_NO_MEM;
            }
        }
    }

    s.running = true;
    s.state = CTRL_STATE_IDLE;
    s.armed = false;
    s.recording = false;
    s.record_pos = 0;
    s.vad_hit_count = 0;

    s.task = xTaskCreateStaticPinnedToCore(
        controller_task, "voice_ctrl",
        CONTROLLER_TASK_STACK / sizeof(StackType_t), NULL,
        CONTROLLER_TASK_PRIO, s.task_stack, &s.task_buf, 0);

    if (s.task == NULL) {
        ESP_LOGE(TAG, "Failed to create controller task");
        s.running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Controller started");
    return ESP_OK;
}

void voice_controller_stop(void) {
    if (!s.running) {
        return;
    }

    s.running = false;

    if (s.task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s.task = NULL;
    }

    ESP_LOGI(TAG, "Controller stopped");
}

void voice_controller_set_enabled(bool enabled) {
    s.enabled = enabled;
}

bool voice_controller_is_enabled(void) {
    return s.enabled;
}

void voice_controller_on_airplay_active(bool active) {
    (void)active;
}

device_mode_t voice_controller_get_mode(void) {
    return s.mode;
}

void voice_controller_notify_user_speech_start(void) {
    if (is_airplay_active_now()) {
        ESP_LOGW(TAG, "PTT ignored: airplay active");
        return;
    }
    if (s.running && !s.recording) {
        s.armed = true;
        s.recording = true;
        s.record_pos = 0;
        s.record_start_ms = esp_timer_get_time() / 1000;
        s.vad_hit_count = 0;
        snprintf(s.last_user, sizeof(s.last_user), "%s", "[recording]");
        set_ui_state(REALTIME_VOICE_STATE_LISTENING, s.last_user, NULL, NULL);
        ESP_LOGI(TAG, "PTT start: recording forced");
    }
    s.user_speech_notified = true;
}

void voice_controller_interrupt_response(void) {
    s.interrupt_requested = true;
}

void voice_controller_reset_session(void) {
    s.armed = false;
    s.recording = false;
    s.record_pos = 0;
    s.vad_hit_count = 0;
    s.last_user[0] = '\0';
    s.last_assistant[0] = '\0';
    tts_accum_reset();
}

esp_err_t voice_controller_speak_text(const char* text) {
    if (!s.initialized || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_airplay_active_now()) {
        ESP_LOGW(TAG, "Speak text blocked: airplay active");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Speak text: %s (not supported in omni mode)", text);
    return ESP_ERR_NOT_SUPPORTED;
}

bool voice_controller_is_activation_armed(void) {
    return s.armed;
}

bool voice_controller_is_response_active(void) {
    return s.state == CTRL_STATE_PLAYING || s.state == CTRL_STATE_REQUESTING;
}
