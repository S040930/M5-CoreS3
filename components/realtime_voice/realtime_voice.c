#include "realtime_voice.h"
#include "voice_controller.h"
#include "voice_frontend_v2.h"
#include "audio/audio_output.h"
#include "esp_log.h"
#include "esp_codec_dev.h"

#include <string.h>

#define TAG "voice_api_v2"

// Static state
static bool s_initialized = false;
static realtime_voice_config_t s_config = {0};

// Callback storage
static voice_ui_state_cb_t s_ui_state_cb = NULL;
static voice_ui_network_busy_cb_t s_ui_network_busy_cb = NULL;
static voice_airplay_query_cb_t s_airplay_query_cb = NULL;
static voice_airplay_refresh_cb_t s_airplay_refresh_cb = NULL;
static voice_network_query_cb_t s_network_query_cb = NULL;

// Expose callbacks to controller
voice_ui_state_cb_t realtime_voice_get_ui_state_cb(void) { return s_ui_state_cb; }
voice_ui_network_busy_cb_t realtime_voice_get_ui_network_busy_cb(void) { return s_ui_network_busy_cb; }
voice_airplay_query_cb_t realtime_voice_get_airplay_query_cb(void) { return s_airplay_query_cb; }
voice_airplay_refresh_cb_t realtime_voice_get_airplay_refresh_cb(void) { return s_airplay_refresh_cb; }
voice_network_query_cb_t realtime_voice_get_network_query_cb(void) { return s_network_query_cb; }

// Public API functions
void realtime_voice_set_ui_state_cb(voice_ui_state_cb_t cb) {
    s_ui_state_cb = cb;
}

void realtime_voice_set_ui_network_busy_cb(voice_ui_network_busy_cb_t cb) {
    s_ui_network_busy_cb = cb;
}

void realtime_voice_set_airplay_query_cb(voice_airplay_query_cb_t cb) {
    s_airplay_query_cb = cb;
}

void realtime_voice_set_airplay_refresh_cb(voice_airplay_refresh_cb_t cb) {
    s_airplay_refresh_cb = cb;
}

void realtime_voice_set_network_query_cb(voice_network_query_cb_t cb) {
    s_network_query_cb = cb;
}

esp_err_t realtime_voice_start(void) {
    if (!s_initialized) {
        void *mic_handle = NULL;
        void *spk_handle = NULL;
        if (audio_output_get_mic_handle(&mic_handle) != ESP_OK ||
            audio_output_get_spk_handle(&spk_handle) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t err = voice_controller_init(&s_config, (esp_codec_dev_handle_t)mic_handle, (esp_codec_dev_handle_t)spk_handle);
        if (err != ESP_OK) {
            return err;
        }
        s_initialized = true;
    }
    return voice_controller_start();
}

void realtime_voice_stop(void) {
    voice_controller_stop();
}

void realtime_voice_set_enabled(bool enabled) {
    voice_controller_set_enabled(enabled);
}

bool realtime_voice_is_enabled(void) {
    return voice_controller_is_enabled();
}

void realtime_voice_on_airplay_state_changed(bool active) {
    voice_controller_on_airplay_active(active);
}

device_mode_t realtime_voice_get_mode(void) {
    return voice_controller_get_mode();
}

void realtime_voice_notify_user_speech_start(void) {
    voice_controller_notify_user_speech_start();
}

void realtime_voice_interrupt_response(void) {
    voice_controller_interrupt_response();
}

void realtime_voice_reset_session(void) {
    voice_controller_reset_session();
}

void realtime_voice_set_config(const realtime_voice_config_t* config) {
    if (config != NULL) {
        memcpy(&s_config, config, sizeof(s_config));
    }
}

bool realtime_voice_config_ready(void) {
    return s_config.api_key[0] != '\0';
}

esp_err_t realtime_voice_speak_text(const char* text) {
    if (!s_initialized) {
        void *mic_handle = NULL;
        void *spk_handle = NULL;
        if (audio_output_get_mic_handle(&mic_handle) != ESP_OK ||
            audio_output_get_spk_handle(&spk_handle) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t err = voice_controller_init(&s_config, (esp_codec_dev_handle_t)mic_handle, (esp_codec_dev_handle_t)spk_handle);
        if (err != ESP_OK) {
            return err;
        }
        s_initialized = true;
    }
    return voice_controller_speak_text(text);
}

void realtime_voice_notify_network_busy(bool busy) {
    if (s_ui_network_busy_cb) {
        s_ui_network_busy_cb(busy);
    }
}

void realtime_voice_notify_ui_state(int state, const char* user, const char* assistant, const char* error) {
    if (s_ui_state_cb) {
        s_ui_state_cb(state, user, assistant, error);
    }
}

bool realtime_voice_is_activation_armed(void) {
    return voice_controller_is_activation_armed();
}

bool realtime_voice_is_response_active(void) {
    return voice_controller_is_response_active();
}
