#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  RESOURCE_OWNER_NONE = 0,
  RESOURCE_OWNER_AIRPLAY,
  RESOURCE_OWNER_VOICE,
} resource_owner_t;

typedef enum {
  RESOURCE_EVENT_AIRPLAY_STARTED,
  RESOURCE_EVENT_AIRPLAY_STOPPED,
  RESOURCE_EVENT_VOICE_STARTED,
  RESOURCE_EVENT_VOICE_STOPPED,
} resource_event_t;

typedef void (*resource_event_cb_t)(resource_event_t event, void *ctx);

esp_err_t resource_manager_init(void);

resource_owner_t resource_manager_get_owner(void);

esp_err_t resource_manager_acquire(resource_owner_t owner);
esp_err_t resource_manager_release(resource_owner_t owner);

void resource_manager_register_callback(resource_event_cb_t cb, void *ctx);

bool resource_manager_is_airplay_active(void);

#ifdef __cplusplus
}
#endif
