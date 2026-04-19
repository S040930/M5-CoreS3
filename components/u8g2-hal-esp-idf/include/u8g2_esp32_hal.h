#ifndef U8G2_ESP32_HAL_H
#define U8G2_ESP32_HAL_H

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "u8g2.h"

#define U8G2_ESP32_HAL_UNDEFINED GPIO_NUM_NC

#if SOC_I2C_NUM > 1
#define I2C_MASTER_NUM I2C_NUM_1
#else
#define I2C_MASTER_NUM I2C_NUM_0
#endif

#define I2C_MASTER_FREQ_HZ 50000

typedef struct {
  union {
    struct {
      gpio_num_t clk;
      gpio_num_t mosi;
      gpio_num_t cs;
    } spi;
    struct {
      gpio_num_t sda;
      gpio_num_t scl;
    } i2c;
  } bus;
  gpio_num_t reset;
  gpio_num_t dc;
} u8g2_esp32_hal_t;

#define U8G2_ESP32_HAL_DEFAULT                       \
  {.bus = {.spi = {.clk = U8G2_ESP32_HAL_UNDEFINED,  \
                   .mosi = U8G2_ESP32_HAL_UNDEFINED, \
                   .cs = U8G2_ESP32_HAL_UNDEFINED}}, \
   .reset = U8G2_ESP32_HAL_UNDEFINED,                \
   .dc = U8G2_ESP32_HAL_UNDEFINED}

void u8g2_esp32_hal_init(u8g2_esp32_hal_t u8g2_esp32_hal_param);
void u8g2_esp32_hal_set_i2c_bus(i2c_master_bus_handle_t bus);
void u8g2_esp32_hal_set_spi_host(spi_host_device_t host);

uint8_t u8g2_esp32_spi_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                               void *arg_ptr);
uint8_t u8g2_esp32_i2c_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                               void *arg_ptr);
uint8_t u8g2_esp32_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                                     void *arg_ptr);

#endif
