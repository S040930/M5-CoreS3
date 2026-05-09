
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_MODE_AIRPLAY = 0,
    DEVICE_MODE_VOICE,
} device_mode_t;

typedef enum {
    REALTIME_VOICE_STATE_OFF = 0,
    REALTIME_VOICE_STATE_STANDBY,
    REALTIME_VOICE_STATE_CONNECTING,
    REALTIME_VOICE_STATE_LISTENING,
    REALTIME_VOICE_STATE_SENDING,
    REALTIME_VOICE_STATE_THINKING,
    REALTIME_VOICE_STATE_SPEAKING,
    REALTIME_VOICE_STATE_ERROR,
} realtime_voice_state_t;

typedef struct {
    char url[256];
    char api_key[256];
    char model[128];
    char voice[32];
    char instructions[512];
} realtime_voice_config_t;

typedef void (*voice_ui_state_cb_t)(int state, const char *user, const char *assistant, const char *error);
typedef void (*voice_ui_network_busy_cb_t)(bool busy);

void realtime_voice_set_ui_state_cb(voice_ui_state_cb_t cb);
void realtime_voice_set_ui_network_busy_cb(voice_ui_network_busy_cb_t cb);

typedef bool (*voice_airplay_query_cb_t)(void);
typedef void (*voice_airplay_refresh_cb_t)(void);

void realtime_voice_set_airplay_query_cb(voice_airplay_query_cb_t cb);
void realtime_voice_set_airplay_refresh_cb(voice_airplay_refresh_cb_t cb);

typedef struct {
    bool faulted;
    bool config_required;
    bool recovering;
    bool network_ready;
    bool discoverable;
} voice_network_snapshot_t;

typedef void (*voice_network_query_cb_t)(voice_network_snapshot_t *out);
void realtime_voice_set_network_query_cb(voice_network_query_cb_t cb);

esp_err_t realtime_voice_start(void);
void realtime_voice_stop(void);
void realtime_voice_set_enabled(bool enabled);
bool realtime_voice_is_enabled(void);
void realtime_voice_on_airplay_state_changed(bool active);
device_mode_t realtime_voice_get_mode(void);
void realtime_voice_notify_user_speech_start(void);
void realtime_voice_interrupt_response(void);
void realtime_voice_reset_session(void);
void realtime_voice_set_config(const realtime_voice_config_t *config);
bool realtime_voice_config_ready(void);
esp_err_t realtime_voice_speak_text(const char *text);
void realtime_voice_notify_network_busy(bool busy);
void realtime_voice_notify_ui_state(int state, const char *user, const char *assistant, const char *error);

bool realtime_voice_is_activation_armed(void);
bool realtime_voice_is_response_active(void);

#ifdef __cplusplus
}
#endif
