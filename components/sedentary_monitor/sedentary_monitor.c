#include "sedentary_monitor.h"

#include "iot_board.h"
#include "realtime_voice.h"
#include "receiver_state.h"
#include "sedentary_alert.h"
#include "sedentary_camera.h"
#include "sedentary_local_detect.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_SEDENTARY_ENABLE
#define CONFIG_SEDENTARY_ENABLE 0
#endif

static bool s_user_enabled = true;
#if CONFIG_FREERTOS_UNICORE
#define SEDENTARY_TASK_CORE 0
#else
#define SEDENTARY_TASK_CORE 0
#endif

#if CONFIG_SEDENTARY_ENABLE

static const char *TAG = "sedentary";

typedef enum {
  ST_IDLE = 0,
  ST_ABSENT,
  ST_PRESENT_TRACKING,
  ST_OVERDUE_REMINDING,
  ST_SUSPENDED,
} sedentary_state_t;

static TaskHandle_t s_task;
static volatile bool s_running;
static sedentary_state_t s_state;
static uint64_t s_present_anchor_ms;
static uint64_t s_absent_streak_ms;
static uint64_t s_last_remind_ms;
static uint32_t s_fail_count;
static uint64_t s_suspended_at_ms;
static bool s_pending_reminder;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

static uint32_t cfg_cap_ms(void) { return (uint32_t)CONFIG_SEDENTARY_CAPTURE_INTERVAL_SEC * 1000U; }
static uint32_t cfg_remind_ms(void) { return (uint32_t)CONFIG_SEDENTARY_REMIND_AFTER_SEC * 1000U; }
static uint32_t cfg_absence_ms(void) { return (uint32_t)CONFIG_SEDENTARY_ABSENCE_RESET_SEC * 1000U; }
static uint32_t cfg_repeat_ms(void) { return (uint32_t)CONFIG_SEDENTARY_REPEAT_INTERVAL_SEC * 1000U; }
static uint32_t cfg_suspend_cool_ms(void) {
  return (uint32_t)CONFIG_SEDENTARY_SUSPEND_COOLDOWN_SEC * 1000U;
}

static const char *pick_phrase(void) {
  const char *p = CONFIG_SEDENTARY_VOICE_PROMPT;
  if (p != NULL && p[0] != '\0') {
    return p;
  }
  return "stand up";
}

static bool airplay_blocks(void) {
  receiver_state_snapshot_t snap = {0};
  receiver_state_get_snapshot(&snap);
  return snap.streaming || snap.session_establishing;
}

static void try_play_pending(void) {
  if (!s_pending_reminder) {
    return;
  }
  if (airplay_blocks() || realtime_voice_is_response_active()) {
    return;
  }
  s_pending_reminder = false;
  (void)sedentary_alert_play(pick_phrase());
}

static void schedule_reminder(void) {
  if (airplay_blocks() || realtime_voice_is_response_active()) {
    s_pending_reminder = true;
    ESP_LOGI(TAG, "reminder deferred (voice/airplay busy)");
    return;
  }
  (void)sedentary_alert_play(pick_phrase());
}

static void on_detect_ok(sedentary_detect_result_t *r) {
  s_fail_count = 0;
  uint64_t t = now_ms();
  const uint32_t cap = cfg_cap_ms();

  if (!r->valid || r->clazz == SEDENTARY_DETECT_UNKNOWN) {
    return;
  }

  if (r->clazz == SEDENTARY_DETECT_PRESENT) {
    s_absent_streak_ms = 0;
    if (s_state == ST_IDLE || s_state == ST_ABSENT) {
      s_state = ST_PRESENT_TRACKING;
      s_present_anchor_ms = t;
      s_last_remind_ms = 0;
      ESP_LOGI(TAG, "state -> PRESENT_TRACKING");
      return;
    }
    if (s_state == ST_SUSPENDED) {
      return;
    }
    if (s_state == ST_PRESENT_TRACKING || s_state == ST_OVERDUE_REMINDING) {
      uint64_t seated = t - s_present_anchor_ms;
      if (seated < (uint64_t)cfg_remind_ms()) {
        return;
      }
      if (s_state == ST_PRESENT_TRACKING) {
        s_state = ST_OVERDUE_REMINDING;
        s_last_remind_ms = t;
        schedule_reminder();
        ESP_LOGI(TAG, "state -> OVERDUE_REMINDING (first remind)");
        return;
      }
      if (s_last_remind_ms == 0 || (t - s_last_remind_ms) >= (uint64_t)cfg_repeat_ms()) {
        s_last_remind_ms = t;
        schedule_reminder();
        ESP_LOGI(TAG, "repeat remind");
      }
    }
    return;
  }

  if (r->clazz == SEDENTARY_DETECT_ABSENT) {
    s_absent_streak_ms += cap;
    if (s_absent_streak_ms >= (uint64_t)cfg_absence_ms()) {
      ESP_LOGI(TAG, "state -> ABSENT (absent streak %" PRIu32 " ms)", (uint32_t)s_absent_streak_ms);
      s_state = ST_ABSENT;
      s_absent_streak_ms = 0;
      s_present_anchor_ms = 0;
      s_last_remind_ms = 0;
      s_pending_reminder = false;
    }
  }
}

static void on_detect_fail(void) {
  s_fail_count++;
  ESP_LOGW(TAG, "detect pipeline fail count=%" PRIu32, (uint32_t)s_fail_count);
  if (s_fail_count >= (uint32_t)CONFIG_SEDENTARY_FAIL_SUSPEND_COUNT) {
    s_state = ST_SUSPENDED;
    s_suspended_at_ms = now_ms();
    ESP_LOGW(TAG, "state -> SUSPENDED");
  }
}

static void monitor_task(void *arg) {
  (void)arg;
  s_state = ST_ABSENT;
  while (s_running) {
    vTaskDelay(pdMS_TO_TICKS(cfg_cap_ms()));
    if (!s_user_enabled) {
      s_state = ST_IDLE;
      continue;
    }
    if (s_state == ST_SUSPENDED) {
      if ((now_ms() - s_suspended_at_ms) >= (uint64_t)cfg_suspend_cool_ms()) {
        s_fail_count = 0;
        s_state = ST_ABSENT;
        ESP_LOGI(TAG, "leave SUSPENDED -> ABSENT");
      } else {
        continue;
      }
    }

    try_play_pending();

    sedentary_detect_result_t det = {0};
    esp_err_t lr = sedentary_local_detect_run(&det);
    if (lr == ESP_ERR_INVALID_STATE) {
      ESP_LOGD(TAG, "local detect idle (calibration required)");
      continue;
    }
    if (lr != ESP_OK) {
      on_detect_fail();
      continue;
    }

    on_detect_ok(&det);
    try_play_pending();
  }
  s_task = NULL;
  vTaskDelete(NULL);
}

#endif /* CONFIG_SEDENTARY_ENABLE */

void sedentary_monitor_set_enabled(bool enabled) { s_user_enabled = enabled; }

bool sedentary_monitor_is_enabled(void) { return s_user_enabled; }

esp_err_t sedentary_monitor_start(void) {
#if !CONFIG_SEDENTARY_ENABLE
  return ESP_OK;
#else
  if (s_task != NULL) {
    return ESP_OK;
  }
  if (!iot_board_is_init()) {
    ESP_LOGW(TAG, "board not ready");
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t cam = sedentary_camera_init();
  if (cam != ESP_OK) {
    ESP_LOGE(TAG, "camera init failed");
    return cam;
  }
  sedentary_local_detect_on_boot();
  s_running = true;
  s_state = ST_ABSENT;
  s_absent_streak_ms = 0;
  s_fail_count = 0;
  s_pending_reminder = false;
  if (xTaskCreatePinnedToCore(monitor_task, "sedentary", 8192, NULL, 3, &s_task,
                              SEDENTARY_TASK_CORE) != pdPASS) {
    s_running = false;
    sedentary_camera_deinit();
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "sedentary monitor started");
  return ESP_OK;
#endif
}

void sedentary_monitor_stop(void) {
#if !CONFIG_SEDENTARY_ENABLE
  return;
#else
  s_running = false;
  for (int i = 0; i < 80 && s_task != NULL; ++i) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  sedentary_camera_deinit();
#endif
}
