
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char api_key[256];
    char model[64];
    char voice[32];
    char instructions[512];
} omni_client_config_t;

typedef struct {
    void *user_data;
    void (*on_text_delta)(const char *text, size_t len, void *user_data);
    void (*on_audio_delta)(const int16_t *pcm, size_t frames, uint32_t sample_rate, void *user_data);
    void (*on_response_done)(void *user_data);
    void (*on_error)(esp_err_t err, const char *message, void *user_data);
} omni_client_callbacks_t;

esp_err_t omni_client_init(const omni_client_config_t *config,
                           const omni_client_callbacks_t *callbacks);
void omni_client_deinit(void);

esp_err_t omni_client_send_audio(const int16_t *pcm_data, size_t pcm_frames,
                                  uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
