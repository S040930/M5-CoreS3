#include "structured_trace.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "structured_trace";

static bool s_enabled = false;

static const char *status_to_str(esp_err_t status) {
  if (status == ESP_OK) {
    return "ok";
  }
  if (status == ESP_ERR_NOT_SUPPORTED) {
    return "not_supported";
  }
  if (status == ESP_ERR_INVALID_ARG) {
    return "invalid_arg";
  }
  if (status == ESP_ERR_INVALID_STATE) {
    return "invalid_state";
  }
  if (status == ESP_ERR_NO_MEM) {
    return "no_mem";
  }
  return "error";
}

esp_err_t structured_trace_init(void) {
#if CONFIG_STRUCTURED_TRACE_ENABLE
  s_enabled = true;
  ESP_LOGI(TAG, "Structured trace enabled");
#else
  s_enabled = false;
#endif
  return ESP_OK;
}

esp_err_t structured_trace_emit(const char *domain, const char *event,
                                esp_err_t status, uint64_t subject_id,
                                uint64_t peer_id, int cseq,
                                const char *stage, cJSON *details) {
  if (!s_enabled || !domain || !event) {
    return ESP_OK;
  }

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddNumberToObject(root, "ts_ms",
                          (double)(esp_timer_get_time() / 1000ULL));
  cJSON_AddStringToObject(root, "domain", domain);
  cJSON_AddStringToObject(root, "event", event);
  cJSON_AddStringToObject(root, "status", status_to_str(status));
  cJSON_AddNumberToObject(root, "status_code", (double)status);

  if (subject_id != 0) {
    char subject_buf[32];
    snprintf(subject_buf, sizeof(subject_buf), "0x%016" PRIx64, subject_id);
    cJSON_AddStringToObject(root, "subject_id", subject_buf);
  }

  if (peer_id != 0) {
    char peer_buf[32];
    snprintf(peer_buf, sizeof(peer_buf), "0x%016" PRIx64, peer_id);
    cJSON_AddStringToObject(root, "peer_id", peer_buf);
  }

  if (cseq > 0) {
    cJSON_AddNumberToObject(root, "cseq", cseq);
  }
  if (stage && stage[0] != '\0') {
    cJSON_AddStringToObject(root, "stage", stage);
  }
  if (details) {
    cJSON *copy = cJSON_Duplicate(details, 1);
    if (copy) {
      cJSON_AddItemToObject(root, "details", copy);
    }
  }

  char *json = cJSON_PrintUnformatted(root);
  if (json) {
    fputs(json, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(json);
  }
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t structured_trace_emit_simple(const char *domain, const char *event,
                                      esp_err_t status) {
  return structured_trace_emit(domain, event, status, 0, 0, 0, NULL, NULL);
}