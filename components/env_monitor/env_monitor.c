#include "env_monitor.h"

#include "audio/audio_output.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_board.h"
#include "realtime_voice.h"
#include "sdkconfig.h"

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_ENV_MONITOR_ENABLE
#define CONFIG_ENV_MONITOR_ENABLE 0
#endif

#ifndef CONFIG_ENV_MONITOR_DEBUG_LOG
#define CONFIG_ENV_MONITOR_DEBUG_LOG 1
#endif

#ifndef CONFIG_ENV_MONITOR_POLL_INTERVAL_SEC
#define CONFIG_ENV_MONITOR_POLL_INTERVAL_SEC 30
#endif

#ifndef CONFIG_ENV_MONITOR_COOLDOWN_SEC
#define CONFIG_ENV_MONITOR_COOLDOWN_SEC 900
#endif

#ifndef CONFIG_ENV_TEMP_HYSTERESIS_C
#define CONFIG_ENV_TEMP_HYSTERESIS_C 1
#endif

#ifndef CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT
#define CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT 3
#endif

#ifndef CONFIG_ENV_PRESSURE_HYSTERESIS_KPA
#define CONFIG_ENV_PRESSURE_HYSTERESIS_KPA 0.5f
#endif

#ifndef CONFIG_ENV_SUMMER_TEMP_MIN_C
#define CONFIG_ENV_SUMMER_TEMP_MIN_C 23
#endif

#ifndef CONFIG_ENV_SUMMER_TEMP_MAX_C
#define CONFIG_ENV_SUMMER_TEMP_MAX_C 28
#endif

#ifndef CONFIG_ENV_SUMMER_HUMIDITY_MIN_PCT
#define CONFIG_ENV_SUMMER_HUMIDITY_MIN_PCT 40
#endif

#ifndef CONFIG_ENV_SUMMER_HUMIDITY_MAX_PCT
#define CONFIG_ENV_SUMMER_HUMIDITY_MAX_PCT 80
#endif

#ifndef CONFIG_ENV_WINTER_TEMP_MIN_C
#define CONFIG_ENV_WINTER_TEMP_MIN_C 20
#endif

#ifndef CONFIG_ENV_WINTER_TEMP_MAX_C
#define CONFIG_ENV_WINTER_TEMP_MAX_C 25
#endif

#ifndef CONFIG_ENV_WINTER_HUMIDITY_MIN_PCT
#define CONFIG_ENV_WINTER_HUMIDITY_MIN_PCT 30
#endif

#ifndef CONFIG_ENV_WINTER_HUMIDITY_MAX_PCT
#define CONFIG_ENV_WINTER_HUMIDITY_MAX_PCT 60
#endif

#define TAG "env_monitor"
#define ENV_MONITOR_TASK_STACK_BYTES 16384U
#define ENV_SHT30_ADDR 0x44
#define ENV_QMP6988_ADDR_PRIMARY 0x70
#define ENV_QMP6988_ADDR_FALLBACK 0x56
#define ENV_QMP6988_CHIP_ID 0x5C
#define ENV_QMP6988_CHIP_ID_REG 0xD1
#define ENV_QMP6988_RESET_REG 0xE0
#define ENV_QMP6988_PRESSURE_MSB_REG 0xF7
#define ENV_QMP6988_TEMPERATURE_MSB_REG 0xFA
#define ENV_QMP6988_CALIBRATION_START 0xA0
#define ENV_QMP6988_CALIBRATION_LEN 25
#define ENV_QMP6988_SUBTRACTOR 8388608
#define ENV_PRESSURE_NOMINAL_KPA 101.3f
#define ENV_PRESSURE_BAND_KPA 5.0f
#define ENV_MONITOR_MISSING_RETRY_MS 30000U
#if CONFIG_FREERTOS_UNICORE
#define ENV_MONITOR_TASK_CORE 0
#else
#define ENV_MONITOR_TASK_CORE 0
#endif

typedef struct {
  int32_t COE_a0;
  int32_t COE_b00;
  int16_t COE_a1;
  int16_t COE_a2;
  int16_t COE_bt1;
  int16_t COE_bt2;
  int16_t COE_bp1;
  int16_t COE_b11;
  int16_t COE_bp2;
  int16_t COE_b12;
  int16_t COE_b21;
  int16_t COE_bp3;
  int32_t a0;
  int32_t b00;
  int32_t a1;
  int32_t a2;
  int64_t bt1;
  int64_t bt2;
  int64_t bp1;
  int64_t b11;
  int64_t bp2;
  int64_t b12;
  int64_t b21;
  int64_t bp3;
} env_qmp6988_cal_t;

typedef struct {
  bool valid;
  i2c_master_bus_handle_t bus;
  i2c_master_dev_handle_t sht30;
  i2c_master_dev_handle_t qmp6988;
  uint8_t qmp_addr;
  env_qmp6988_cal_t qmp_cal;
} env_hw_t;

typedef enum {
  ENV_SEASON_WINTER = 0,
  ENV_SEASON_SUMMER = 1,
} env_season_t;

typedef struct {
  float temp_min_c;
  float temp_max_c;
  float humidity_min_pct;
  float humidity_max_pct;
} env_thresholds_t;

typedef struct {
  bool active;
  uint64_t last_remind_ms;
} env_trigger_t;

typedef struct {
  bool pending;
  uint8_t pending_mask;
  char pending_text[160];
} env_pending_t;

static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
static StackType_t *s_task_stack;
static volatile bool s_running;
static env_hw_t s_hw;
static env_trigger_t s_temp_trigger;
static env_trigger_t s_humidity_trigger;
static env_trigger_t s_cold_trigger;
static env_trigger_t s_humid_trigger;
static env_trigger_t s_pressure_trigger;
static env_pending_t s_pending;
static bool s_time_wait_logged;
static env_season_t s_last_season = ENV_SEASON_WINTER;
static float s_latest_temp_c = 0.0f;
static float s_latest_humidity_pct = 0.0f;
static float s_latest_pressure_kpa = 0.0f;
static bool s_i2c_probe_logged = false;
static uint8_t s_last_absent_mask = 0;
static uint64_t s_last_absent_log_ms = 0;

static const uint8_t k_env_diag_focus_addrs[] = {
    0x21,
    0x23, /* LTR553 */
    0x34, /* ES7210 */
    0x36, /* AW88298 / amp control candidate */
    0x38,
    0x40,
    0x44, /* SHT30 */
    0x51,
    0x56, /* QMP6988 fallback */
    0x58,
    0x69,
    0x70, /* QMP6988 primary */
};

static esp_err_t env_sht30_sample(env_hw_t *hw, float *temp_c, float *humidity_pct);

static uint64_t now_ms(void) {
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t poll_ms(void) {
  return (uint32_t)CONFIG_ENV_MONITOR_POLL_INTERVAL_SEC * 1000U;
}

static uint32_t cooldown_ms(void) {
  return (uint32_t)CONFIG_ENV_MONITOR_COOLDOWN_SEC * 1000U;
}

static bool time_ready(void) {
  time_t now = time(NULL);
  if (now < 1704067200L) {
    return false;
  }
  struct tm tm_local = {0};
  return localtime_r(&now, &tm_local) != NULL && (tm_local.tm_year + 1900) >= 2024;
}

static env_season_t current_season(void) {
  time_t now = time(NULL);
  struct tm tm_local = {0};
  if (localtime_r(&now, &tm_local) == NULL) {
    return ENV_SEASON_WINTER;
  }
  int month = tm_local.tm_mon + 1;
  return (month >= 4 && month <= 9) ? ENV_SEASON_SUMMER : ENV_SEASON_WINTER;
}

static const char *season_name(env_season_t season) {
  return season == ENV_SEASON_SUMMER ? "summer" : "winter";
}

static env_thresholds_t thresholds_for(env_season_t season) {
  if (season == ENV_SEASON_SUMMER) {
    return (env_thresholds_t){
        .temp_min_c = (float)CONFIG_ENV_SUMMER_TEMP_MIN_C,
        .temp_max_c = (float)CONFIG_ENV_SUMMER_TEMP_MAX_C,
        .humidity_min_pct = (float)CONFIG_ENV_SUMMER_HUMIDITY_MIN_PCT,
        .humidity_max_pct = (float)CONFIG_ENV_SUMMER_HUMIDITY_MAX_PCT,
    };
  }
  return (env_thresholds_t){
      .temp_min_c = (float)CONFIG_ENV_WINTER_TEMP_MIN_C,
      .temp_max_c = (float)CONFIG_ENV_WINTER_TEMP_MAX_C,
      .humidity_min_pct = (float)CONFIG_ENV_WINTER_HUMIDITY_MIN_PCT,
      .humidity_max_pct = (float)CONFIG_ENV_WINTER_HUMIDITY_MAX_PCT,
  };
}

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data,
                              size_t len) {
  if (dev == NULL || data == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_master_transmit_receive(dev, &reg, 1, data, len, 200);
}

static esp_err_t i2c_write_reg8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  return i2c_master_transmit(dev, buf, sizeof(buf), 200);
}

static bool env_diag_addr_acked(i2c_master_bus_handle_t bus, uint8_t addr) {
  if (bus == NULL) {
    return false;
  }
  return (i2c_master_probe(bus, addr, 20) == ESP_OK);
}

static void env_log_i2c_discovery(i2c_master_bus_handle_t bus) {
  if (bus == NULL || s_i2c_probe_logged) {
    return;
  }

  char found[192];
  size_t used = 0;
  found[0] = '\0';

  for (size_t i = 0; i < sizeof(k_env_diag_focus_addrs); i++) {
    uint8_t addr = k_env_diag_focus_addrs[i];
    if (env_diag_addr_acked(bus, addr)) {
      int written = snprintf(found + used, sizeof(found) - used,
                             "%s0x%02X", used == 0 ? "" : " ", addr);
      if (written > 0 && (size_t)written < (sizeof(found) - used)) {
        used += (size_t)written;
      } else {
        break;
      }
    }
  }

  ESP_LOGI(TAG, "i2c diag: shared bus with board codec + auto_brightness; detected_addrs=%s",
           used > 0 ? found : "(none)");

  char focus[160];
  size_t focus_used = 0;
  focus[0] = '\0';
  for (size_t i = 0; i < sizeof(k_env_diag_focus_addrs); i++) {
    uint8_t addr = k_env_diag_focus_addrs[i];
    bool ack = env_diag_addr_acked(bus, addr);
    int written = snprintf(focus + focus_used, sizeof(focus) - focus_used,
                           "%s0x%02X=%s", focus_used == 0 ? "" : " ", addr,
                           ack ? "ack" : "noack");
    if (written > 0 && (size_t)written < (sizeof(focus) - focus_used)) {
      focus_used += (size_t)written;
    } else {
      break;
    }
  }
  ESP_LOGI(TAG, "i2c diag focus: %s", focus);
  s_i2c_probe_logged = true;
}

static void env_log_hw_absent(uint8_t missing_mask) {
  uint64_t now = now_ms();
  if (missing_mask == s_last_absent_mask &&
      (now - s_last_absent_log_ms) < 60000ULL) {
    return;
  }
  bool sht_missing = (missing_mask & 0x01) != 0;
  bool qmp_missing = (missing_mask & 0x02) != 0;
  if (sht_missing && qmp_missing) {
    ESP_LOGW(TAG, "env probe failed: no SHT30/QMP6988 ack on shared bus");
  } else if (sht_missing) {
    ESP_LOGW(TAG, "env probe failed: no SHT30 ack on shared bus");
  } else if (qmp_missing) {
    ESP_LOGW(TAG, "env probe failed: no QMP6988 ack on shared bus");
  }
  s_last_absent_mask = missing_mask;
  s_last_absent_log_ms = now;
}

static uint16_t be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int32_t qmp_temperature256(const env_qmp6988_cal_t *cal, int32_t dt) {
  int64_t wk1 = (int64_t)cal->a1 * (int64_t)dt;
  int64_t wk2 = ((int64_t)cal->a2 * (int64_t)dt) >> 14;
  wk2 = (wk2 * (int64_t)dt) >> 10;
  wk2 = ((wk1 + wk2) / 32767) >> 19;
  return (int32_t)((cal->a0 + (int32_t)wk2) >> 4);
}

static int32_t qmp_pressure16(const env_qmp6988_cal_t *cal, int32_t dp, int32_t tx) {
  int64_t wk1, wk2, wk3;
  wk1 = ((int64_t)cal->bt1 * (int64_t)tx);
  wk2 = ((int64_t)cal->bp1 * (int64_t)dp) >> 5;
  wk1 += wk2;
  wk2 = ((int64_t)cal->bt2 * (int64_t)tx) >> 1;
  wk2 = (wk2 * (int64_t)tx) >> 8;
  wk3 = wk2;
  wk2 = ((int64_t)cal->b11 * (int64_t)tx) >> 4;
  wk2 = (wk2 * (int64_t)dp) >> 1;
  wk3 += wk2;
  wk2 = ((int64_t)cal->bp2 * (int64_t)dp) >> 13;
  wk2 = (wk2 * (int64_t)dp) >> 1;
  wk3 += wk2;
  wk1 += wk3 >> 14;
  wk2 = ((int64_t)cal->b12 * (int64_t)tx);
  wk2 = (wk2 * (int64_t)tx) >> 22;
  wk2 = (wk2 * (int64_t)dp) >> 1;
  wk3 = wk2;
  wk2 = ((int64_t)cal->b21 * (int64_t)tx) >> 6;
  wk2 = (wk2 * (int64_t)dp) >> 23;
  wk2 = (wk2 * (int64_t)dp) >> 1;
  wk3 += wk2;
  wk2 = ((int64_t)cal->bp3 * (int64_t)dp) >> 12;
  wk2 = (wk2 * (int64_t)dp) >> 23;
  wk2 = (wk2 * (int64_t)dp);
  wk3 += wk2;
  wk1 += wk3 >> 15;
  wk1 /= 32767L;
  wk1 >>= 11;
  wk1 += cal->b00;
  return (int32_t)wk1;
}

static esp_err_t env_qmp_finish_init(env_hw_t *hw) {
  if (hw == NULL || hw->qmp6988 == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err = i2c_write_reg8(hw->qmp6988, ENV_QMP6988_RESET_REG, 0xE6);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 reset assert failed: addr=0x%02x reg=0x%02X err=%s",
             hw->qmp_addr, ENV_QMP6988_RESET_REG, esp_err_to_name(err));
    return err;
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  err = i2c_write_reg8(hw->qmp6988, ENV_QMP6988_RESET_REG, 0x00);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 reset release failed: addr=0x%02x reg=0x%02X err=%s",
             hw->qmp_addr, ENV_QMP6988_RESET_REG, esp_err_to_name(err));
    return err;
  }

  uint8_t cal_raw[ENV_QMP6988_CALIBRATION_LEN] = {0};
  err = i2c_read_reg(hw->qmp6988, ENV_QMP6988_CALIBRATION_START, cal_raw,
                     sizeof(cal_raw));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 calibration read failed: addr=0x%02x reg=0x%02X len=%u err=%s",
             hw->qmp_addr, ENV_QMP6988_CALIBRATION_START, ENV_QMP6988_CALIBRATION_LEN,
             esp_err_to_name(err));
    return err;
  }

  hw->qmp_cal.COE_a0 =
      (int32_t)(((cal_raw[18] << 12) | (cal_raw[19] << 4) | (cal_raw[24] & 0x0f)) << 12) >> 12;
  hw->qmp_cal.COE_a1 = (int16_t)(((cal_raw[20]) << 8) | cal_raw[21]);
  hw->qmp_cal.COE_a2 = (int16_t)(((cal_raw[22]) << 8) | cal_raw[23]);
  hw->qmp_cal.COE_b00 =
      (int32_t)(((cal_raw[0] << 12) | (cal_raw[1] << 4) | ((cal_raw[24] & 0xf0) >> 4)) << 12) >> 12;
  hw->qmp_cal.COE_bt1 = (int16_t)(((cal_raw[2]) << 8) | cal_raw[3]);
  hw->qmp_cal.COE_bt2 = (int16_t)(((cal_raw[4]) << 8) | cal_raw[5]);
  hw->qmp_cal.COE_bp1 = (int16_t)(((cal_raw[6]) << 8) | cal_raw[7]);
  hw->qmp_cal.COE_b11 = (int16_t)(((cal_raw[8]) << 8) | cal_raw[9]);
  hw->qmp_cal.COE_bp2 = (int16_t)(((cal_raw[10]) << 8) | cal_raw[11]);
  hw->qmp_cal.COE_b12 = (int16_t)(((cal_raw[12]) << 8) | cal_raw[13]);
  hw->qmp_cal.COE_b21 = (int16_t)(((cal_raw[14]) << 8) | cal_raw[15]);
  hw->qmp_cal.COE_bp3 = (int16_t)(((cal_raw[16]) << 8) | cal_raw[17]);

  hw->qmp_cal.a0 = hw->qmp_cal.COE_a0;
  hw->qmp_cal.b00 = hw->qmp_cal.COE_b00;
  hw->qmp_cal.a1 = 3608L * (int32_t)hw->qmp_cal.COE_a1 - 1731677965L;
  hw->qmp_cal.a2 = 16889L * (int32_t)hw->qmp_cal.COE_a2 - 87619360L;
  hw->qmp_cal.bt1 = 2982L * (int64_t)hw->qmp_cal.COE_bt1 + 107370906L;
  hw->qmp_cal.bt2 = 329854L * (int64_t)hw->qmp_cal.COE_bt2 + 108083093L;
  hw->qmp_cal.bp1 = 19923L * (int64_t)hw->qmp_cal.COE_bp1 + 1133836764L;
  hw->qmp_cal.b11 = 2406L * (int64_t)hw->qmp_cal.COE_b11 + 118215883L;
  hw->qmp_cal.bp2 = 3079L * (int64_t)hw->qmp_cal.COE_bp2 - 181579595L;
  hw->qmp_cal.b12 = 6846L * (int64_t)hw->qmp_cal.COE_b12 + 85590281L;
  hw->qmp_cal.b21 = 13836L * (int64_t)hw->qmp_cal.COE_b21 + 79333336L;
  hw->qmp_cal.bp3 = 2915L * (int64_t)hw->qmp_cal.COE_bp3 + 157155561L;

  uint8_t ctrl = 0;
  err = i2c_read_reg(hw->qmp6988, 0xF4, &ctrl, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 ctrl read failed: addr=0x%02x reg=0xF4 err=%s",
             hw->qmp_addr, esp_err_to_name(err));
    return err;
  }
  ctrl = (ctrl & 0xFC) | 0x03;
  err = i2c_write_reg8(hw->qmp6988, 0xF4, ctrl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 mode write failed: addr=0x%02x reg=0xF4 val=0x%02X err=%s",
             hw->qmp_addr, ctrl, esp_err_to_name(err));
    return err;
  }
  err = i2c_write_reg8(hw->qmp6988, 0xF1, 0x02);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 filter write failed: addr=0x%02x reg=0xF1 val=0x02 err=%s",
             hw->qmp_addr, esp_err_to_name(err));
    return err;
  }
  err = i2c_read_reg(hw->qmp6988, 0xF4, &ctrl, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 osrs_t read failed: addr=0x%02x reg=0xF4 err=%s",
             hw->qmp_addr, esp_err_to_name(err));
    return err;
  }
  ctrl = (ctrl & 0xE3) | (0x04 << 2);
  err = i2c_write_reg8(hw->qmp6988, 0xF4, ctrl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 osrs_t write failed: addr=0x%02x reg=0xF4 val=0x%02X err=%s",
             hw->qmp_addr, ctrl, esp_err_to_name(err));
    return err;
  }
  err = i2c_read_reg(hw->qmp6988, 0xF4, &ctrl, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 osrs_p read failed: addr=0x%02x reg=0xF4 err=%s",
             hw->qmp_addr, esp_err_to_name(err));
    return err;
  }
  ctrl = (ctrl & 0x1F) | (0x01 << 5);
  err = i2c_write_reg8(hw->qmp6988, 0xF4, ctrl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 osrs_p write failed: addr=0x%02x reg=0xF4 val=0x%02X err=%s",
             hw->qmp_addr, ctrl, esp_err_to_name(err));
    return err;
  }
  return ESP_OK;
}

static esp_err_t env_qmp_try_addr(env_hw_t *hw, uint8_t addr) {
  if (hw == NULL || hw->bus == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  i2c_device_config_t qmp_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = addr,
      .scl_speed_hz = 100000,
  };
  i2c_master_dev_handle_t dev = NULL;
  esp_err_t err = i2c_master_bus_add_device(hw->bus, &qmp_cfg, &dev);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 add-device failed: addr=0x%02X err=%s",
             addr, esp_err_to_name(err));
    return err;
  }

  uint8_t chip_id = 0;
  err = i2c_read_reg(dev, ENV_QMP6988_CHIP_ID_REG, &chip_id, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 chip_id read failed: addr=0x%02X reg=0x%02X err=%s",
             addr, ENV_QMP6988_CHIP_ID_REG, esp_err_to_name(err));
    i2c_master_bus_rm_device(dev);
    return err;
  }
  if (chip_id != ENV_QMP6988_CHIP_ID) {
    ESP_LOGW(TAG, "QMP6988 chip_id mismatch: addr=0x%02X reg=0x%02X got=0x%02X expected=0x%02X",
             addr, ENV_QMP6988_CHIP_ID_REG, chip_id, ENV_QMP6988_CHIP_ID);
    i2c_master_bus_rm_device(dev);
    return ESP_ERR_NOT_FOUND;
  }

  hw->qmp6988 = dev;
  hw->qmp_addr = addr;
  err = env_qmp_finish_init(hw);
  if (err != ESP_OK) {
    i2c_master_bus_rm_device(dev);
    hw->qmp6988 = NULL;
    hw->qmp_addr = 0;
    return err;
  }
  return ESP_OK;
}

static esp_err_t env_hw_init(env_hw_t *hw) {
  if (!iot_board_is_init()) {
    return ESP_ERR_INVALID_STATE;
  }
  if (hw->valid) {
    return ESP_OK;
  }

  esp_err_t err = ESP_OK;
  if (hw->bus == NULL) {
    err = i2c_master_get_bus_handle(CONFIG_BSP_I2C_NUM, &hw->bus);
  }
  if (err != ESP_OK || hw->bus == NULL) {
    ESP_LOGW(TAG, "I2C bus unavailable: port=%d err=%s",
             CONFIG_BSP_I2C_NUM, esp_err_to_name(err != ESP_OK ? err : ESP_ERR_INVALID_STATE));
    return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
  }
  if (CONFIG_ENV_MONITOR_DEBUG_LOG) {
    env_log_i2c_discovery(hw->bus);
  }

  bool sht_ack = env_diag_addr_acked(hw->bus, ENV_SHT30_ADDR);
  bool qmp_primary_ack = env_diag_addr_acked(hw->bus, ENV_QMP6988_ADDR_PRIMARY);
  bool qmp_fallback_ack = env_diag_addr_acked(hw->bus, ENV_QMP6988_ADDR_FALLBACK);
  bool qmp_any_ack = qmp_primary_ack || qmp_fallback_ack;
  uint8_t missing_mask = 0;
  if (!sht_ack) {
    missing_mask |= 0x01;
  }
  if (!qmp_any_ack) {
    missing_mask |= 0x02;
  }
  if (missing_mask != 0) {
    env_log_hw_absent(missing_mask);
    if (!sht_ack) {
      ESP_LOGW(TAG, "SHT30 probe failed: addr=0x%02X no ACK on shared bus",
               ENV_SHT30_ADDR);
    }
    if (!qmp_any_ack) {
      ESP_LOGW(TAG, "QMP6988 probe failed: addrs=0x%02X/0x%02X no ACK on shared bus",
               ENV_QMP6988_ADDR_PRIMARY, ENV_QMP6988_ADDR_FALLBACK);
    }
    return ESP_ERR_NOT_FOUND;
  }

  if (hw->sht30 == NULL) {
    i2c_device_config_t sht_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ENV_SHT30_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(hw->bus, &sht_cfg, &hw->sht30);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "SHT30 add-device failed: addr=0x%02X err=%s",
               ENV_SHT30_ADDR, esp_err_to_name(err));
      return err;
    }
  }
  {
    float dummy_temp = 0.0f;
    float dummy_hum = 0.0f;
    err = env_sht30_sample(hw, &dummy_temp, &dummy_hum);
    if (err != ESP_OK) {
      return err;
    }
  }

  if (hw->qmp6988 == NULL) {
    if (qmp_primary_ack) {
      ESP_LOGI(TAG, "QMP6988 probe: trying primary addr=0x%02X", ENV_QMP6988_ADDR_PRIMARY);
      err = env_qmp_try_addr(hw, ENV_QMP6988_ADDR_PRIMARY);
    } else {
      err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK && qmp_fallback_ack) {
      ESP_LOGI(TAG, "QMP6988 fallback probe: trying addr=0x%02X", ENV_QMP6988_ADDR_FALLBACK);
      err = env_qmp_try_addr(hw, ENV_QMP6988_ADDR_FALLBACK);
    }
    if (err != ESP_OK) {
      return err;
    }
  }

  hw->valid = true;
  return ESP_OK;
}

static esp_err_t env_sht30_sample(env_hw_t *hw, float *temp_c, float *humidity_pct) {
  if (hw == NULL || hw->sht30 == NULL || temp_c == NULL || humidity_pct == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  const uint8_t cmd[2] = {0x24, 0x00};
  esp_err_t err = i2c_master_transmit(hw->sht30, cmd, sizeof(cmd), 200);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SHT30 measure command failed: addr=0x%02X cmd=0x2400 err=%s",
             ENV_SHT30_ADDR, esp_err_to_name(err));
    return err;
  }
  vTaskDelay(pdMS_TO_TICKS(15));
  uint8_t data[6] = {0};
  err = i2c_master_receive(hw->sht30, data, sizeof(data), 200);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SHT30 sample read failed: addr=0x%02X len=%u err=%s",
             ENV_SHT30_ADDR, (unsigned)sizeof(data), esp_err_to_name(err));
    return err;
  }
  uint16_t raw_temp = be16(&data[0]);
  uint16_t raw_hum = be16(&data[3]);
  *temp_c = -45.0f + ((175.0f * (float)raw_temp) / 65535.0f);
  *humidity_pct = (100.0f * (float)raw_hum) / 65536.0f;
  return ESP_OK;
}

static esp_err_t env_qmp6988_sample(env_hw_t *hw, float *pressure_kpa) {
  if (hw == NULL || hw->qmp6988 == NULL || pressure_kpa == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t raw[6] = {0};
  esp_err_t err = i2c_read_reg(hw->qmp6988, ENV_QMP6988_PRESSURE_MSB_REG, raw, sizeof(raw));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "QMP6988 sample read failed: addr=0x%02x reg=0x%02X len=%u err=%s",
             hw->qmp_addr, ENV_QMP6988_PRESSURE_MSB_REG, (unsigned)sizeof(raw),
             esp_err_to_name(err));
    return err;
  }
  uint32_t p_read = (((uint32_t)raw[0]) << 16) | (((uint32_t)raw[1]) << 8) | raw[2];
  uint32_t t_read = (((uint32_t)raw[3]) << 16) | (((uint32_t)raw[4]) << 8) | raw[5];
  int32_t p_raw = (int32_t)(p_read - ENV_QMP6988_SUBTRACTOR);
  int32_t t_raw = (int32_t)(t_read - ENV_QMP6988_SUBTRACTOR);
  int32_t t256 = qmp_temperature256(&hw->qmp_cal, t_raw);
  int32_t p16 = qmp_pressure16(&hw->qmp_cal, p_raw, t256);
  *pressure_kpa = (float)p16 / 16000.0f;
  return ESP_OK;
}

static bool env_sample(env_hw_t *hw, float *temp_c, float *humidity_pct, float *pressure_kpa) {
  if (!hw->valid) {
    return false;
  }
  if (env_sht30_sample(hw, temp_c, humidity_pct) != ESP_OK) {
    return false;
  }
  if (env_qmp6988_sample(hw, pressure_kpa) != ESP_OK) {
    return false;
  }
  s_latest_temp_c = *temp_c;
  s_latest_humidity_pct = *humidity_pct;
  s_latest_pressure_kpa = *pressure_kpa;
  return true;
}

static void build_reminder_text(uint8_t active_mask, char *dst, size_t dst_len) {
  if (dst == NULL || dst_len == 0) {
    return;
  }
  if (active_mask == 0) {
    dst[0] = '\0';
    return;
  }

  /* describe each issue concisely for the AI model to speak naturally */
  const char *issues[5];
  uint8_t count = 0;
  if ((active_mask & ENV_TRIG_TEMP_HIGH) && count < 5)  issues[count++] = "temperature is too high";
  if ((active_mask & ENV_TRIG_HUMID_LOW) && count < 5)  issues[count++] = "air is too dry";
  if ((active_mask & ENV_TRIG_TEMP_LOW) && count < 5)   issues[count++] = "temperature is too low";
  if ((active_mask & ENV_TRIG_HUMID_HIGH) && count < 5) issues[count++] = "air is too humid";
  if ((active_mask & ENV_TRIG_PRESSURE) && count < 5)   issues[count++] = "pressure is abnormal";

  if (count == 0) {
    dst[0] = '\0';
    return;
  }

  size_t used = (size_t)snprintf(dst, dst_len, "Please remind the user that ");
  for (uint8_t i = 0; i < count && used < dst_len; i++) {
    used += (size_t)snprintf(dst + used, dst_len - used, "%s%s",
                             i > 0 ? ", " : "", issues[i]);
  }
  snprintf(dst + used, dst_len - used, ". Please suggest steps to improve indoor comfort.");
}

static bool reminder_busy(void) {
  return audio_output_is_active() || realtime_voice_is_response_active();
}

static void update_pending_message(uint8_t active_mask) {
  build_reminder_text(active_mask, s_pending.pending_text, sizeof(s_pending.pending_text));
  s_pending.pending_mask = active_mask;
  s_pending.pending = active_mask != 0;
}

static void clear_pending(void) {
  s_pending.pending = false;
  s_pending.pending_mask = 0;
  s_pending.pending_text[0] = '\0';
}

static bool reminder_cooldown_ready(uint8_t active_mask, uint64_t now) {
  bool ready = true;
  if ((active_mask & ENV_TRIG_TEMP_HIGH) != 0) {
    if (s_temp_trigger.last_remind_ms != 0 &&
        (now - s_temp_trigger.last_remind_ms) < cooldown_ms())
      ready = false;
  }
  if ((active_mask & ENV_TRIG_HUMID_LOW) != 0) {
    if (s_humidity_trigger.last_remind_ms != 0 &&
        (now - s_humidity_trigger.last_remind_ms) < cooldown_ms())
      ready = false;
  }
  if ((active_mask & ENV_TRIG_TEMP_LOW) != 0) {
    if (s_cold_trigger.last_remind_ms != 0 &&
        (now - s_cold_trigger.last_remind_ms) < cooldown_ms())
      ready = false;
  }
  if ((active_mask & ENV_TRIG_HUMID_HIGH) != 0) {
    if (s_humid_trigger.last_remind_ms != 0 &&
        (now - s_humid_trigger.last_remind_ms) < cooldown_ms())
      ready = false;
  }
  if ((active_mask & ENV_TRIG_PRESSURE) != 0) {
    if (s_pressure_trigger.last_remind_ms != 0 &&
        (now - s_pressure_trigger.last_remind_ms) < cooldown_ms())
      ready = false;
  }
  return ready;
}

static void record_remind_time(uint8_t mask, uint64_t t) {
  if ((mask & ENV_TRIG_TEMP_HIGH) != 0)  s_temp_trigger.last_remind_ms = t;
  if ((mask & ENV_TRIG_HUMID_LOW) != 0)  s_humidity_trigger.last_remind_ms = t;
  if ((mask & ENV_TRIG_TEMP_LOW) != 0)   s_cold_trigger.last_remind_ms = t;
  if ((mask & ENV_TRIG_HUMID_HIGH) != 0) s_humid_trigger.last_remind_ms = t;
  if ((mask & ENV_TRIG_PRESSURE) != 0)   s_pressure_trigger.last_remind_ms = t;
}

static void try_send_pending(void) {
  if (!s_pending.pending || s_pending.pending_mask == 0 || s_pending.pending_text[0] == '\0') {
    return;
  }
  if (reminder_busy()) {
    return;
  }
  esp_err_t err = realtime_voice_speak_text(s_pending.pending_text);
  if (err == ESP_OK) {
    record_remind_time(s_pending.pending_mask, now_ms());
    clear_pending();
  } else if (err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "pending reminder speak failed: %s", esp_err_to_name(err));
  }
}

static void maybe_schedule_reminder(uint8_t active_mask, uint64_t now, bool allow_repeat) {
  if (active_mask == 0) {
    return;
  }
  (void)allow_repeat;
  if (s_pending.pending) {
    update_pending_message(active_mask);
    return;
  }
  if (!reminder_cooldown_ready(active_mask, now)) {
    return;
  }

  build_reminder_text(active_mask, s_pending.pending_text, sizeof(s_pending.pending_text));
  if (s_pending.pending_text[0] == '\0') {
    return;
  }
  if (reminder_busy()) {
    s_pending.pending = true;
    s_pending.pending_mask = active_mask;
    record_remind_time(active_mask, now);
    ESP_LOGI(TAG, "reminder deferred: busy=%d mask=0x%02x",
             reminder_busy() ? 1 : 0, active_mask);
    return;
  }

  esp_err_t err = realtime_voice_speak_text(s_pending.pending_text);
  if (err == ESP_OK) {
    record_remind_time(active_mask, now);
    clear_pending();
    ESP_LOGI(TAG, "reminder spoken: mask=0x%02x", active_mask);
  } else if (err == ESP_ERR_INVALID_STATE) {
    s_pending.pending = true;
    s_pending.pending_mask = active_mask;
    record_remind_time(active_mask, now);
    ESP_LOGI(TAG, "reminder deferred after race: mask=0x%02x", active_mask);
  } else {
    ESP_LOGW(TAG, "reminder speak failed: %s", esp_err_to_name(err));
  }
}

static void env_monitor_task(void *arg) {
  (void)arg;
  bool hw_ready = false;
  uint64_t last_time_warn_ms = 0;
  uint64_t last_sample_log_ms = 0;
  float temp_c = 0.0f;
  float humidity_pct = 0.0f;
  float pressure_kpa = 0.0f;

  while (s_running) {
    uint64_t loop_ms = now_ms();
    if (!hw_ready) {
      esp_err_t init_err = env_hw_init(&s_hw);
      if (init_err == ESP_OK) {
        hw_ready = true;
        ESP_LOGI(TAG, "ENV.3 sensor ready (SHT30 + QMP6988 addr=0x%02x)", s_hw.qmp_addr);
      } else {
        if ((loop_ms - last_time_warn_ms) >= 10000ULL) {
          ESP_LOGW(TAG, "sensor init retry pending: err=%s next_retry_ms=%u",
                   esp_err_to_name(init_err),
                   (unsigned)(init_err == ESP_ERR_NOT_FOUND ? ENV_MONITOR_MISSING_RETRY_MS : 10000U));
          last_time_warn_ms = loop_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(init_err == ESP_ERR_NOT_FOUND ? ENV_MONITOR_MISSING_RETRY_MS : 10000U));
        continue;
      }
    }

    try_send_pending();

    if (!env_sample(&s_hw, &temp_c, &humidity_pct, &pressure_kpa)) {
      if ((loop_ms - last_time_warn_ms) >= 10000ULL) {
        ESP_LOGW(TAG, "sensor sample failed");
        last_time_warn_ms = loop_ms;
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    float pressure_nominal_low = ENV_PRESSURE_NOMINAL_KPA - ENV_PRESSURE_BAND_KPA;
    float pressure_nominal_high = ENV_PRESSURE_NOMINAL_KPA + ENV_PRESSURE_BAND_KPA;
    if (pressure_kpa < pressure_nominal_low || pressure_kpa > pressure_nominal_high) {
      ESP_LOGW(TAG, "pressure diag out of band: pressure=%.1f kPa nominal=%.1f±%.1f",
               pressure_kpa, ENV_PRESSURE_NOMINAL_KPA, ENV_PRESSURE_BAND_KPA);
    }

    bool ready = time_ready();
    if (!ready) {
      if (!s_time_wait_logged || (loop_ms - last_time_warn_ms) >= 60000ULL) {
        ESP_LOGW(TAG, "waiting for valid time before seasonal thresholds");
        s_time_wait_logged = true;
        last_time_warn_ms = loop_ms;
      }
      if (CONFIG_ENV_MONITOR_DEBUG_LOG) {
        ESP_LOGI(TAG, "sample: time_pending temp=%.1fC humidity=%.1f%% pressure=%.1fkPa",
                 temp_c, humidity_pct, pressure_kpa);
      }
      vTaskDelay(pdMS_TO_TICKS(poll_ms()));
      continue;
    }

    s_time_wait_logged = false;
    env_season_t season = current_season();
    if (season != s_last_season) {
      s_last_season = season;
      ESP_LOGI(TAG, "season -> %s", season_name(season));
    }
    env_thresholds_t th = thresholds_for(season);

    /* ---- trigger hysteresis ---- */
    uint8_t next_temp_high_mask  = (temp_c >= th.temp_max_c) ? ENV_TRIG_TEMP_HIGH : 0x00;
    uint8_t next_temp_low_mask   = (temp_c <= th.temp_min_c) ? ENV_TRIG_TEMP_LOW : 0x00;
    uint8_t next_humid_low_mask  = (humidity_pct <= th.humidity_min_pct) ? ENV_TRIG_HUMID_LOW : 0x00;
    uint8_t next_humid_high_mask = (humidity_pct >= th.humidity_max_pct) ? ENV_TRIG_HUMID_HIGH : 0x00;
    uint8_t next_pressure_mask   = (pressure_kpa < pressure_nominal_low ||
                                    pressure_kpa > pressure_nominal_high) ? ENV_TRIG_PRESSURE : 0x00;

    s_temp_trigger.active =
        (s_temp_trigger.active &&
         temp_c > (th.temp_max_c - (float)CONFIG_ENV_TEMP_HYSTERESIS_C)) ||
        next_temp_high_mask != 0;
    s_humidity_trigger.active =
        (s_humidity_trigger.active &&
         humidity_pct < (th.humidity_min_pct + (float)CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT)) ||
        next_humid_low_mask != 0;
    s_cold_trigger.active =
        (s_cold_trigger.active &&
         temp_c < (th.temp_min_c + (float)CONFIG_ENV_TEMP_HYSTERESIS_C)) ||
        next_temp_low_mask != 0;
    s_humid_trigger.active =
        (s_humid_trigger.active &&
         humidity_pct > (th.humidity_max_pct - (float)CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT)) ||
        next_humid_high_mask != 0;
    s_pressure_trigger.active =
        (s_pressure_trigger.active &&
         pressure_kpa > (pressure_nominal_low + (float)CONFIG_ENV_PRESSURE_HYSTERESIS_KPA) &&
         pressure_kpa < (pressure_nominal_high - (float)CONFIG_ENV_PRESSURE_HYSTERESIS_KPA))
             ? false
             : next_pressure_mask != 0;

    uint8_t active_mask = 0;
    if (temp_c >= th.temp_max_c ||
        (s_temp_trigger.active && temp_c > (th.temp_max_c - (float)CONFIG_ENV_TEMP_HYSTERESIS_C)))
      active_mask |= ENV_TRIG_TEMP_HIGH;
    if (humidity_pct <= th.humidity_min_pct ||
        (s_humidity_trigger.active &&
         humidity_pct < (th.humidity_min_pct + (float)CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT)))
      active_mask |= ENV_TRIG_HUMID_LOW;
    if (temp_c <= th.temp_min_c ||
        (s_cold_trigger.active && temp_c < (th.temp_min_c + (float)CONFIG_ENV_TEMP_HYSTERESIS_C)))
      active_mask |= ENV_TRIG_TEMP_LOW;
    if (humidity_pct >= th.humidity_max_pct ||
        (s_humid_trigger.active &&
         humidity_pct > (th.humidity_max_pct - (float)CONFIG_ENV_HUMIDITY_HYSTERESIS_PCT)))
      active_mask |= ENV_TRIG_HUMID_HIGH;
    if (pressure_kpa < pressure_nominal_low || pressure_kpa > pressure_nominal_high ||
        (s_pressure_trigger.active &&
         pressure_kpa > (pressure_nominal_low + (float)CONFIG_ENV_PRESSURE_HYSTERESIS_KPA) &&
         pressure_kpa < (pressure_nominal_high - (float)CONFIG_ENV_PRESSURE_HYSTERESIS_KPA)))
      active_mask |= ENV_TRIG_PRESSURE;

    if (CONFIG_ENV_MONITOR_DEBUG_LOG &&
        (loop_ms - last_sample_log_ms) >= (uint64_t)poll_ms()) {
      ESP_LOGI(TAG,
               "sample: season=%s temp=%.1fC humidity=%.1f%% pressure=%.1fkPa "
               "th=[%.1f..%.1fC %.1f..%.1f%%] active=0x%02x pending=%d",
               season_name(season), temp_c, humidity_pct, pressure_kpa, th.temp_min_c,
               th.temp_max_c, th.humidity_min_pct, th.humidity_max_pct, active_mask,
               s_pending.pending ? 1 : 0);
      last_sample_log_ms = loop_ms;
    }

    if (active_mask != 0) {
      maybe_schedule_reminder(active_mask, loop_ms, true);
    } else {
      if (!s_pending.pending) {
        s_temp_trigger.active = false;
        s_humidity_trigger.active = false;
        s_cold_trigger.active = false;
        s_humid_trigger.active = false;
        s_pressure_trigger.active = false;
      }
    }

    try_send_pending();
    vTaskDelay(pdMS_TO_TICKS(poll_ms()));
  }

  s_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t env_monitor_start(void) {
#if !CONFIG_ENV_MONITOR_ENABLE
  return ESP_OK;
#else
  if (s_task != NULL) {
    return ESP_OK;
  }
  if (!iot_board_is_init()) {
    ESP_LOGW(TAG, "board not ready");
    return ESP_ERR_INVALID_STATE;
  }
  s_running = true;
  memset(&s_hw, 0, sizeof(s_hw));
  memset(&s_temp_trigger, 0, sizeof(s_temp_trigger));
  memset(&s_humidity_trigger, 0, sizeof(s_humidity_trigger));
  memset(&s_cold_trigger, 0, sizeof(s_cold_trigger));
  memset(&s_humid_trigger, 0, sizeof(s_humid_trigger));
  memset(&s_pressure_trigger, 0, sizeof(s_pressure_trigger));
  memset(&s_pending, 0, sizeof(s_pending));
  s_last_season = ENV_SEASON_WINTER;
  s_time_wait_logged = false;

  if (s_task_stack == NULL) {
    s_task_stack =
        (StackType_t *)heap_caps_malloc(ENV_MONITOR_TASK_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_task_stack == NULL) {
      s_task_stack =
          (StackType_t *)heap_caps_malloc(ENV_MONITOR_TASK_STACK_BYTES, MALLOC_CAP_8BIT);
    }
  }
  if (s_task_stack == NULL) {
    ESP_LOGE(TAG, "Failed to allocate env monitor stack");
    s_running = false;
    return ESP_ERR_NO_MEM;
  }

  s_task = xTaskCreateStaticPinnedToCore(
      env_monitor_task, "env_monitor",
      ENV_MONITOR_TASK_STACK_BYTES / sizeof(StackType_t), NULL, 3,
      s_task_stack, &s_task_tcb, ENV_MONITOR_TASK_CORE);
  if (s_task == NULL) {
    ESP_LOGE(TAG, "Failed to create env monitor task");
    s_running = false;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "env monitor started");
  return ESP_OK;
#endif
}

void env_monitor_stop(void) {
#if !CONFIG_ENV_MONITOR_ENABLE
  return;
#else
  s_running = false;
  for (int i = 0; i < 80 && s_task != NULL; ++i) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  s_task = NULL;
#endif
}

bool env_monitor_get_latest(float *temp_c, float *humidity_pct, float *pressure_kpa) {
  if (!s_hw.valid) {
    return false;
  }
  if (temp_c != NULL) {
    *temp_c = s_latest_temp_c;
  }
  if (humidity_pct != NULL) {
    *humidity_pct = s_latest_humidity_pct;
  }
  if (pressure_kpa != NULL) {
    *pressure_kpa = s_latest_pressure_kpa;
  }
  return true;
}

bool env_monitor_get_comfort(env_comfort_t *out) {
  if (out == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  float temp_c = 0.0f;
  float humidity_pct = 0.0f;
  float pressure_kpa = 0.0f;
  if (!env_monitor_get_latest(&temp_c, &humidity_pct, &pressure_kpa)) {
    return false;
  }
  out->temp_c = temp_c;
  out->humidity_pct = humidity_pct;
  out->pressure_kpa = pressure_kpa;

  env_season_t season = ENV_SEASON_WINTER;
  bool time_ok = time_ready();
  if (time_ok) {
    season = current_season();
  }
  out->season = (int)season;
  env_thresholds_t th = thresholds_for(season);

  uint8_t mask = 0;
  if (temp_c >= th.temp_max_c)                           mask |= ENV_TRIG_TEMP_HIGH;
  else if (temp_c <= th.temp_min_c)                      mask |= ENV_TRIG_TEMP_LOW;
  if (humidity_pct <= th.humidity_min_pct)                mask |= ENV_TRIG_HUMID_LOW;
  else if (humidity_pct >= th.humidity_max_pct)           mask |= ENV_TRIG_HUMID_HIGH;

  float pressure_nominal_low  = ENV_PRESSURE_NOMINAL_KPA - ENV_PRESSURE_BAND_KPA;
  float pressure_nominal_high = ENV_PRESSURE_NOMINAL_KPA + ENV_PRESSURE_BAND_KPA;
  if (pressure_kpa < pressure_nominal_low || pressure_kpa > pressure_nominal_high)
    mask |= ENV_TRIG_PRESSURE;

  out->issue_mask = mask;
  out->comfortable = (mask == 0);

  /* build issue name list */
  uint8_t idx = 0;
  if ((mask & ENV_TRIG_TEMP_HIGH)  && idx < 6)  snprintf(out->issue_names[idx++], 24, "temperature_high");
  if ((mask & ENV_TRIG_HUMID_LOW)  && idx < 6)  snprintf(out->issue_names[idx++], 24, "humidity_low");
  if ((mask & ENV_TRIG_TEMP_LOW)   && idx < 6)  snprintf(out->issue_names[idx++], 24, "temperature_low");
  if ((mask & ENV_TRIG_HUMID_HIGH) && idx < 6)  snprintf(out->issue_names[idx++], 24, "humidity_high");
  if ((mask & ENV_TRIG_PRESSURE)   && idx < 6)  snprintf(out->issue_names[idx++], 24, "pressure_abnormal");
  out->issue_count = idx;

  /* build concise advice for the AI tool response */
  {
    const char *actions[5];
    uint8_t ac = 0;
    if ((mask & ENV_TRIG_TEMP_HIGH)  && ac < 5) actions[ac++] = "开空调";
    if ((mask & ENV_TRIG_HUMID_LOW)  && ac < 5) actions[ac++] = "开加湿器";
    if ((mask & ENV_TRIG_TEMP_LOW)   && ac < 5) actions[ac++] = "开暖气";
    if ((mask & ENV_TRIG_HUMID_HIGH) && ac < 5) actions[ac++] = "开除湿机";
    if ((mask & ENV_TRIG_PRESSURE)   && ac < 5) actions[ac++] = "注意天气变化";

    size_t au = 0;
    if (ac > 0) {
      au += (size_t)snprintf(out->advice + au, sizeof(out->advice) - au, "建议");
      for (uint8_t i = 0; i < ac && au < sizeof(out->advice); i++) {
        au += (size_t)snprintf(out->advice + au, sizeof(out->advice) - au,
                               "%s%s", i > 0 ? "、" : "", actions[i]);
      }
      snprintf(out->advice + au, sizeof(out->advice) - au, "。");
    }
  }

  return true;
}
