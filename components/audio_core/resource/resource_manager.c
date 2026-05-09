#include "resource_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

static const char *TAG = "resource_mgr";

static resource_owner_t s_owner = RESOURCE_OWNER_NONE;
static resource_event_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t resource_manager_init(void) {
  portENTER_CRITICAL(&s_mux);
  s_owner = RESOURCE_OWNER_NONE;
  portEXIT_CRITICAL(&s_mux);
  ESP_LOGI(TAG, "initialized");
  return ESP_OK;
}

resource_owner_t resource_manager_get_owner(void) {
  portENTER_CRITICAL(&s_mux);
  resource_owner_t owner = s_owner;
  portEXIT_CRITICAL(&s_mux);
  return owner;
}

esp_err_t resource_manager_acquire(resource_owner_t owner) {
  portENTER_CRITICAL(&s_mux);
  if (s_owner == owner) {
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
  }
  if (s_owner != RESOURCE_OWNER_NONE) {
    portEXIT_CRITICAL(&s_mux);
    return ESP_ERR_INVALID_STATE;
  }
  s_owner = owner;
  portEXIT_CRITICAL(&s_mux);

  if (s_cb) {
    if (owner == RESOURCE_OWNER_VOICE) {
      s_cb(RESOURCE_EVENT_VOICE_STARTED, s_cb_ctx);
    } else if (owner == RESOURCE_OWNER_AIRPLAY) {
      s_cb(RESOURCE_EVENT_AIRPLAY_STARTED, s_cb_ctx);
    }
  }
  return ESP_OK;
}

esp_err_t resource_manager_release(resource_owner_t owner) {
  portENTER_CRITICAL(&s_mux);
  if (s_owner != owner) {
    portEXIT_CRITICAL(&s_mux);
    return ESP_ERR_INVALID_STATE;
  }
  resource_owner_t prev = s_owner;
  s_owner = RESOURCE_OWNER_NONE;
  portEXIT_CRITICAL(&s_mux);

  if (s_cb) {
    if (prev == RESOURCE_OWNER_VOICE) {
      s_cb(RESOURCE_EVENT_VOICE_STOPPED, s_cb_ctx);
    } else if (prev == RESOURCE_OWNER_AIRPLAY) {
      s_cb(RESOURCE_EVENT_AIRPLAY_STOPPED, s_cb_ctx);
    }
  }
  return ESP_OK;
}

void resource_manager_register_callback(resource_event_cb_t cb, void *ctx) {
  s_cb = cb;
  s_cb_ctx = ctx;
}

bool resource_manager_is_airplay_active(void) {
  portENTER_CRITICAL(&s_mux);
  bool active = (s_owner == RESOURCE_OWNER_AIRPLAY);
  portEXIT_CRITICAL(&s_mux);
  return active;
}
