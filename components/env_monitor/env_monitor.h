#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENV_TRIG_TEMP_HIGH  0x01
#define ENV_TRIG_HUMID_LOW  0x02
#define ENV_TRIG_TEMP_LOW   0x04
#define ENV_TRIG_HUMID_HIGH 0x08
#define ENV_TRIG_PRESSURE   0x10

typedef struct {
  bool comfortable;
  float temp_c;
  float humidity_pct;
  float pressure_kpa;
  int season;           /* 0 = winter, 1 = summer */
  uint8_t issue_mask;
  char issue_names[6][24];
  uint8_t issue_count;
  char advice[256];
} env_comfort_t;

esp_err_t env_monitor_start(void);
void env_monitor_stop(void);
bool env_monitor_get_latest(float *temp_c, float *humidity_pct, float *pressure_kpa);
bool env_monitor_get_comfort(env_comfort_t *out);

#ifdef __cplusplus
}
#endif
