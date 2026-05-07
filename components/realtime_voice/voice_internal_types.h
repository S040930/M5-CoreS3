
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_LOG_TAG "voice_v2"

#define VOICE_MAX_TEXT_LEN 64
#define VOICE_MAX_URL_LEN 256
#define VOICE_MAX_API_KEY_LEN 256
#define VOICE_MAX_MODEL_LEN 128

typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_RECORDING,
    VOICE_STATE_REQUESTING,
    VOICE_STATE_PLAYING,
    VOICE_STATE_ERROR,
} voice_state_t;

typedef enum {
    VOICE_FRONTEND_EVENT_NONE = 0,
    VOICE_FRONTEND_EVENT_WAKE,
    VOICE_FRONTEND_EVENT_AUDIO,
    VOICE_FRONTEND_EVENT_ERROR,
} voice_frontend_event_type_t;

typedef struct {
    voice_frontend_event_type_t type;
    const int16_t* pcm_data;
    size_t pcm_frames;
    bool vad_active;
} voice_frontend_event_t;

typedef void (*voice_frontend_event_cb_t)(const voice_frontend_event_t* event, void* user_data);

typedef struct {
    int16_t* data;
    size_t capacity_frames;
    size_t write_pos;
    size_t read_pos;
    bool is_full;
} voice_ringbuf_t;

typedef struct {
    char text[VOICE_MAX_TEXT_LEN];
    const int16_t* pcm_data;
    size_t pcm_frames;
    uint32_t sample_rate;
    bool has_audio;
    bool has_text;
} voice_response_t;

typedef void (*voice_response_audio_cb_t)(const int16_t* pcm, size_t frames, uint32_t sample_rate, void* user_data);

typedef struct {
    char url[VOICE_MAX_URL_LEN];
    char api_key[VOICE_MAX_API_KEY_LEN];
    char model[VOICE_MAX_MODEL_LEN];
} voice_api_config_t;

typedef struct {
    const int16_t* pcm_data;
    size_t pcm_frames;
    uint32_t sample_rate;
} voice_audio_request_t;

typedef enum {
    VOICE_PLAYER_EVENT_NONE = 0,
    VOICE_PLAYER_EVENT_STARTED,
    VOICE_PLAYER_EVENT_STOPPED,
    VOICE_PLAYER_EVENT_ERROR,
} voice_player_event_type_t;

typedef void (*voice_player_event_cb_t)(voice_player_event_type_t event, void* user_data);

#ifdef __cplusplus
}
#endif
