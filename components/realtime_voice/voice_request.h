#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_REQUEST_TEXT_MAX 512

typedef struct {
  const char *url;
  const char *api_key;
  const char *model;
} voice_request_config_t;

typedef struct {
  esp_err_t err;
  int status_code;
  uint64_t latency_ms;
  char text[VOICE_REQUEST_TEXT_MAX];
} voice_request_result_t;

esp_err_t voice_request_send_audio(const voice_request_config_t *cfg, const int16_t *pcm,
                                   size_t frames, uint32_t sample_rate,
                                   voice_request_result_t *out);

#ifdef __cplusplus
}
#endif
