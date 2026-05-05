#include "voice_timers.h"

#include "screen_ui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

#define TAG "voice_timers"
#define SLOT_COUNT 4

typedef struct {
  esp_timer_handle_t handle;
  bool armed;
  uint32_t public_id;
  char label[32];
} slot_t;

static slot_t s_slots[SLOT_COUNT];
static SemaphoreHandle_t s_mux;
static uint32_t s_next_public_id = 1;
static bool s_inited;

static void timer_fired(void *arg) {
  intptr_t idx = (intptr_t)arg;
  if (idx < 0 || idx >= SLOT_COUNT) {
    return;
  }
  char msg[SCREEN_UI_TEXT_MAX];
  msg[0] = '\0';

  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
    ESP_LOGW(TAG, "timer fired: mutex timeout slot=%d", (int)idx);
    return;
  }

  slot_t *sl = &s_slots[idx];
  if (!sl->armed) {
    xSemaphoreGive(s_mux);
    return;
  }
  sl->armed = false;
  if (sl->label[0] != '\0') {
    snprintf(msg, sizeof(msg), "Timer: %s", sl->label);
  } else {
    snprintf(msg, sizeof(msg), "Timer #%lu done", (unsigned long)sl->public_id);
  }
  uint32_t pid = sl->public_id;
  xSemaphoreGive(s_mux);

  ESP_LOGI(TAG, "timer id=%lu fired (%s)", (unsigned long)pid, msg);
  screen_ui_set_voice_state(SCREEN_UI_VOICE_LISTENING, NULL, msg, NULL);
}

void voice_timers_init(void) {
  if (s_inited) {
    return;
  }
  s_mux = xSemaphoreCreateMutex();
  if (s_mux == NULL) {
    return;
  }
  static const char *const slot_names[] = {"vt0", "vt1", "vt2", "vt3"};
  for (int i = 0; i < SLOT_COUNT; ++i) {
    slot_t *sl = &s_slots[i];
    memset(sl, 0, sizeof(*sl));
    const esp_timer_create_args_t args = {
        .callback = &timer_fired,
        .arg = (void *)(intptr_t)i,
        .dispatch_method = ESP_TIMER_TASK,
        .name = slot_names[i],
    };
    if (esp_timer_create(&args, &sl->handle) != ESP_OK) {
      sl->handle = NULL;
    }
  }
  s_inited = true;
}

void voice_timers_deinit(void) {
  if (!s_inited) {
    return;
  }
  if (s_mux != NULL && xSemaphoreTake(s_mux, pdMS_TO_TICKS(500)) == pdTRUE) {
    for (int i = 0; i < SLOT_COUNT; ++i) {
      slot_t *sl = &s_slots[i];
      if (sl->handle != NULL) {
        esp_timer_stop(sl->handle);
        esp_timer_delete(sl->handle);
        sl->handle = NULL;
      }
      sl->armed = false;
    }
    xSemaphoreGive(s_mux);
  }
  if (s_mux != NULL) {
    vSemaphoreDelete(s_mux);
    s_mux = NULL;
  }
  s_inited = false;
}

uint32_t voice_timers_set(uint32_t duration_sec, const char *label) {
  if (!s_inited || duration_sec == 0 || s_mux == NULL) {
    return 0;
  }
  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
    return 0;
  }
  int free_i = -1;
  for (int i = 0; i < SLOT_COUNT; ++i) {
    if (s_slots[i].handle != NULL && !s_slots[i].armed) {
      free_i = i;
      break;
    }
  }
  if (free_i < 0) {
    xSemaphoreGive(s_mux);
    return 0;
  }
  slot_t *sl = &s_slots[free_i];
  sl->label[0] = '\0';
  if (label != NULL && label[0] != '\0') {
    snprintf(sl->label, sizeof(sl->label), "%s", label);
  }
  uint32_t id = s_next_public_id++;
  if (s_next_public_id == 0) {
    s_next_public_id = 1;
  }
  sl->public_id = id;
  sl->armed = true;
  uint64_t us = (uint64_t)duration_sec * 1000000ULL;
  esp_err_t err = esp_timer_start_once(sl->handle, us);
  if (err != ESP_OK) {
    sl->armed = false;
    xSemaphoreGive(s_mux);
    ESP_LOGW(TAG, "start_once failed: %s", esp_err_to_name(err));
    return 0;
  }
  xSemaphoreGive(s_mux);
  ESP_LOGI(TAG, "started timer id=%lu dur=%lus", (unsigned long)id, (unsigned long)duration_sec);
  return id;
}

bool voice_timers_cancel(uint32_t timer_id) {
  if (!s_inited || timer_id == 0 || s_mux == NULL) {
    return false;
  }
  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
    return false;
  }
  bool ok = false;
  for (int i = 0; i < SLOT_COUNT; ++i) {
    slot_t *sl = &s_slots[i];
    if (sl->armed && sl->public_id == timer_id) {
      (void)esp_timer_stop(sl->handle);
      sl->armed = false;
      ok = true;
      break;
    }
  }
  xSemaphoreGive(s_mux);
  return ok;
}

int voice_timers_active_count(void) {
  if (!s_inited || s_mux == NULL) {
    return 0;
  }
  if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }
  int n = 0;
  for (int i = 0; i < SLOT_COUNT; ++i) {
    if (s_slots[i].armed) {
      n++;
    }
  }
  xSemaphoreGive(s_mux);
  return n;
}
