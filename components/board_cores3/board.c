/**
 * @file board.c
 * @brief M5Stack CoreS3 board implementation
 *
 * The CoreS3 board support package owns the speaker path, including the
 * onboard AW88298 amplifier and I2S routing. This board layer initializes the
 * BSP once and exposes the speaker handle to the audio output backend.
 */

#include "iot_board.h"

#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "es7210_adc.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "sdkconfig.h"

// CoreS3 speaker-path pins exposed by the BSP implementation.
#define CORES3_I2S_MCLK GPIO_NUM_0
#define CORES3_I2S_SCLK GPIO_NUM_34
#define CORES3_I2S_LCLK GPIO_NUM_33
#define CORES3_I2S_DOUT GPIO_NUM_13
#define CORES3_I2S_DSIN GPIO_NUM_14

// Minimal BSP surface retained by the refactor.
esp_err_t bsp_i2c_init(void);
const audio_codec_data_if_t *bsp_audio_get_codec_itf(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

static const char TAG[] = "CoreS3";

static bool s_board_initialized = false;
static esp_codec_dev_handle_t s_speaker_handle = NULL;
static esp_codec_dev_handle_t s_mic_handle = NULL;

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
  case BOARD_AUDIO_SPK_ID:
    return (board_res_handle_t)s_speaker_handle;
  case BOARD_AUDIO_MIC_ID:
    return (board_res_handle_t)s_mic_handle;
  default:
    return NULL;
  }
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "CoreS3 I2C init failed");

  s_speaker_handle = bsp_audio_codec_speaker_init();
  if (s_speaker_handle == NULL) {
    ESP_LOGE(TAG, "Failed to initialize CoreS3 speaker codec");
    return ESP_FAIL;
  }

  const audio_codec_data_if_t *data_if = bsp_audio_get_codec_itf();
  assert(data_if);

  i2c_master_bus_handle_t i2c_bus = NULL;
  ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(CONFIG_BSP_I2C_NUM, &i2c_bus),
                      TAG, "get I2C bus handle failed");

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = CONFIG_BSP_I2C_NUM,
      .addr = ES7210_CODEC_DEFAULT_ADDR,
      .bus_handle = i2c_bus,
  };
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  if (ctrl_if == NULL) {
    ESP_LOGE(TAG, "Failed to create ES7210 I2C control interface");
    esp_codec_dev_close(s_speaker_handle);
    esp_codec_dev_delete(s_speaker_handle);
    s_speaker_handle = NULL;
    return ESP_FAIL;
  }

  es7210_codec_cfg_t es7210_cfg = {
      .ctrl_if = ctrl_if,
      .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
  };
  const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
  if (es7210_if == NULL) {
    ESP_LOGE(TAG, "Failed to create ES7210 codec interface");
    esp_codec_dev_close(s_speaker_handle);
    esp_codec_dev_delete(s_speaker_handle);
    s_speaker_handle = NULL;
    return ESP_FAIL;
  }

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN,
      .codec_if = es7210_if,
      .data_if = data_if,
  };
  s_mic_handle = esp_codec_dev_new(&dev_cfg);
  if (s_mic_handle == NULL) {
    ESP_LOGE(TAG, "Failed to create mic codec device");
    esp_codec_dev_close(s_speaker_handle);
    esp_codec_dev_delete(s_speaker_handle);
    s_speaker_handle = NULL;
    return ESP_FAIL;
  }

  s_board_initialized = true;
  ESP_LOGI(TAG, "CoreS3 board initialized (speaker=BSP, mic=manual ES7210 MIC1+MIC2)");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  if (!s_board_initialized) {
    return ESP_OK;
  }

  if (s_speaker_handle != NULL) {
    esp_codec_dev_close(s_speaker_handle);
    esp_codec_dev_delete(s_speaker_handle);
    s_speaker_handle = NULL;
  }
  if (s_mic_handle != NULL) {
    esp_codec_dev_close(s_mic_handle);
    esp_codec_dev_delete(s_mic_handle);
    s_mic_handle = NULL;
  }

  s_board_initialized = false;
  return ESP_OK;
}
