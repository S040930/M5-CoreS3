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
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"

// CoreS3 speaker-path pins exposed by the BSP implementation.
#define CORES3_I2S_MCLK GPIO_NUM_0
#define CORES3_I2S_SCLK GPIO_NUM_34
#define CORES3_I2S_LCLK GPIO_NUM_33
#define CORES3_I2S_DOUT GPIO_NUM_13
#define CORES3_I2S_DSIN GPIO_NUM_14

// Minimal BSP surface retained by the refactor.
esp_err_t bsp_i2c_init(void);
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

static const char TAG[] = "CoreS3";

static bool s_board_initialized = false;
static esp_codec_dev_handle_t s_speaker_handle = NULL;

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

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100);
  clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

  const i2s_std_config_t audio_cfg = {
      .clk_cfg = clk_cfg,
      .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = CORES3_I2S_MCLK,
              .bclk = CORES3_I2S_SCLK,
              .ws = CORES3_I2S_LCLK,
              .dout = CORES3_I2S_DOUT,
              .din = CORES3_I2S_DSIN,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  ESP_RETURN_ON_ERROR(bsp_audio_init(&audio_cfg), TAG,
                      "CoreS3 audio bus init failed");
  ESP_LOGI(TAG, "CoreS3 BSP audio init: 44100 Hz stereo mclk=384x");

  s_speaker_handle = bsp_audio_codec_speaker_init();
  if (s_speaker_handle == NULL) {
    ESP_LOGE(TAG, "Failed to initialize CoreS3 speaker codec");
    return ESP_FAIL;
  }

  s_board_initialized = true;
  ESP_LOGI(TAG, "CoreS3 board initialized");
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

  s_board_initialized = false;
  return ESP_OK;
}
