#pragma once

#include <stdint.h>
#include <stddef.h>
#include "event_ids.h"

typedef struct {
    event_id_t id;
    void *data;
    size_t data_size;
} event_t;

typedef struct {
    event_id_t id;
    float temperature;
    float humidity;
    float pressure;
} event_env_data_t;

typedef struct {
    event_id_t id;
    const char *device_name;
    const char *peer_address;
} event_airplay_client_t;

typedef struct {
    event_id_t id;
    const int16_t *samples;
    size_t frame_count;
    int sample_rate;
    int channels;
} event_audio_frame_t;

typedef struct {
    event_id_t id;
    const char *text;
} event_text_t;

typedef struct {
    event_id_t id;
    const char *error_msg;
    int error_code;
} event_error_t;
