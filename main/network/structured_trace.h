#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct cJSON cJSON;

esp_err_t structured_trace_init(void);

esp_err_t structured_trace_emit(const char *domain, const char *event,
                                esp_err_t status, uint64_t subject_id,
                                uint64_t peer_id, int cseq,
                                const char *stage, cJSON *details);

esp_err_t structured_trace_emit_simple(const char *domain, const char *event,
                                      esp_err_t status);