#include "ltr553.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define TAG "ltr553"

#define LTR553_REG_ALS_CONTR     0x80
#define LTR553_REG_PS_CONTR      0x81
#define LTR553_REG_PS_LED        0x82
#define LTR553_REG_PS_N_PULSES   0x83
#define LTR553_REG_PS_MEAS_RATE  0x84
#define LTR553_REG_ALS_MEAS_RATE 0x85
#define LTR553_REG_PART_ID       0x86
#define LTR553_REG_ALS_DATA_CH1_0 0x88
#define LTR553_REG_ALS_DATA_CH1_1 0x89
#define LTR553_REG_ALS_DATA_CH0_0 0x8A
#define LTR553_REG_ALS_DATA_CH0_1 0x8B
#define LTR553_REG_ALS_PS_STATUS 0x8C

#define LTR553_ALS_CONTR_ACTIVE  0x01
#define LTR553_ALS_GAIN_1X       (0x00 << 2)
#define LTR553_ALS_GAIN_2X       (0x01 << 2)
#define LTR553_ALS_GAIN_4X       (0x02 << 2)
#define LTR553_ALS_GAIN_8X       (0x03 << 2)
#define LTR553_ALS_GAIN_48X      (0x04 << 2)
#define LTR553_ALS_GAIN_96X      (0x05 << 2)
#define LTR553_ALS_IT_50MS       (0x01 << 4)
#define LTR553_ALS_IT_100MS      (0x00 << 4)
#define LTR553_ALS_IT_200MS      (0x04 << 4)
#define LTR553_ALS_IT_400MS      (0x08 << 4)
#define LTR553_ALS_IT_800MS      (0x0C << 4)

#define LTR553_PART_ID_EXPECTED  0x92

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t ltr553_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static esp_err_t ltr553_read_reg(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t ltr553_read_reg16(uint8_t reg_low, uint16_t *val) {
    uint8_t lo = 0, hi = 0;
    esp_err_t err = ltr553_read_reg(reg_low, &lo);
    if (err != ESP_OK) return err;
    err = ltr553_read_reg(reg_low + 1, &hi);
    if (err != ESP_OK) return err;
    *val = (uint16_t)((hi << 8) | lo);
    return ESP_OK;
}

esp_err_t ltr553_init(i2c_master_bus_handle_t bus) {
    if (bus == NULL) return ESP_ERR_INVALID_ARG;
    if (s_dev != NULL) return ESP_OK;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LTR553_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t part_id = 0;
    err = ltr553_read_reg(LTR553_REG_PART_ID, &part_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read part_id failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    if (part_id != LTR553_PART_ID_EXPECTED) {
        ESP_LOGW(TAG, "unexpected part_id: 0x%02X (expected 0x%02X)", part_id, LTR553_PART_ID_EXPECTED);
    } else {
        ESP_LOGI(TAG, "LTR-553 detected, part_id=0x%02X", part_id);
    }

    err = ltr553_als_enable(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ALS enable failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t ltr553_deinit(void) {
    if (s_dev == NULL) return ESP_OK;
    ltr553_als_enable(false);
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ESP_OK;
}

esp_err_t ltr553_als_enable(bool enable) {
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t ctrl = LTR553_ALS_GAIN_1X | LTR553_ALS_IT_100MS;
    if (enable) ctrl |= LTR553_ALS_CONTR_ACTIVE;
    return ltr553_write_reg(LTR553_REG_ALS_CONTR, ctrl);
}

esp_err_t ltr553_read_als(uint16_t *ch0, uint16_t *ch1) {
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (ch0 == NULL || ch1 == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t status = 0;
    esp_err_t err = ltr553_read_reg(LTR553_REG_ALS_PS_STATUS, &status);
    if (err != ESP_OK) return err;

    bool als_data_valid = (status & 0x80) != 0;
    if (!als_data_valid) {
        return ESP_ERR_NOT_FOUND;
    }

    err = ltr553_read_reg16(LTR553_REG_ALS_DATA_CH1_0, ch1);
    if (err != ESP_OK) return err;
    err = ltr553_read_reg16(LTR553_REG_ALS_DATA_CH0_0, ch0);
    if (err != ESP_OK) return err;

    return ESP_OK;
}
