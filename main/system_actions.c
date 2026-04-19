#include "system_actions.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void deferred_restart_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(750));
  esp_restart();
  vTaskDelete(NULL);
}

esp_err_t system_actions_schedule_restart(void) {
  BaseType_t ok = xTaskCreate(deferred_restart_task, "deferred_restart", 2048,
                              NULL, 4, NULL);
  return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
