#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t audio_domain_init(void);

esp_err_t audio_domain_decode(const uint8_t *input, size_t len,
                              int16_t **output, size_t *frames);

esp_err_t audio_domain_encode(const int16_t *input, size_t frames,
                              uint8_t **output, size_t *len);

esp_err_t audio_domain_output_play(const int16_t *samples, size_t frames);

esp_err_t audio_domain_output_stop(void);

esp_err_t audio_domain_output_pause(void);

esp_err_t audio_domain_output_resume(void);

esp_err_t audio_domain_set_volume(float volume);

float audio_domain_get_volume(void);

esp_err_t audio_domain_acquire_speaker(const char *owner);

esp_err_t audio_domain_release_speaker(const char *owner);

bool audio_domain_is_speaker_available(const char *owner);
