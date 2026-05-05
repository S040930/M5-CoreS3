#include "sedentary_alert.h"

#include "audio_output.h"
#include "audio_output_common.h"
#include "iot_board.h"
#include "realtime_voice.h"
#include "receiver_state.h"
#include "screen_ui.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <math.h>
#include <string.h>

static const char *TAG = "sed_alert";
#define OWNER_TAG "sedentary_monitor"

static int mclk_multiple(uint32_t rate) {
  switch (rate) {
  case 44100:
  case 88200:
  case 176400:
    return I2S_MCLK_MULTIPLE_384;
  default:
    return 0;
  }
}

esp_err_t sedentary_alert_play(const char *utf8_line) {
  if (utf8_line == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  receiver_state_snapshot_t snap = {0};
  receiver_state_get_snapshot(&snap);
  if (snap.session_establishing || snap.streaming) {
    ESP_LOGW(TAG, "sedentary speaker acquire skipped: airplay active");
    return ESP_ERR_INVALID_STATE;
  }
  if (realtime_voice_is_response_active()) {
    ESP_LOGW(TAG, "sedentary speaker acquire skipped: voice response active");
    return ESP_ERR_INVALID_STATE;
  }
  screen_ui_set_voice_state(SCREEN_UI_VOICE_LISTENING, NULL, utf8_line, NULL);

  esp_err_t a = audio_output_acquire_external(OWNER_TAG, true);
  if (a != ESP_OK) {
    ESP_LOGW(TAG, "speaker busy; alert UI only");
    return a;
  }

  esp_codec_dev_handle_t spk = (esp_codec_dev_handle_t)iot_board_get_handle(BOARD_AUDIO_SPK_ID);
  if (spk == NULL) {
    (void)audio_output_release_external(OWNER_TAG, true);
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t hz = (uint32_t)AO_OUTPUT_RATE;
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = 2,
      .channel_mask = 0x03,
      .sample_rate = hz,
      .mclk_multiple = mclk_multiple(hz),
  };
  if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
    (void)audio_output_release_external(OWNER_TAG, true);
    return ESP_FAIL;
  }

  const unsigned nframes = hz / 5;
  const double freq = 880.0;
  int16_t *pcm = (int16_t *)malloc(nframes * 2 * sizeof(int16_t));
  if (pcm == NULL) {
    esp_codec_dev_close(spk);
    (void)audio_output_release_external(OWNER_TAG, true);
    return ESP_ERR_NO_MEM;
  }
  for (unsigned i = 0; i < nframes; ++i) {
    double t = (double)i / (double)hz;
    int16_t s = (int16_t)(sin(2.0 * M_PI * freq * t) * 8000.0);
    pcm[i * 2] = s;
    pcm[i * 2 + 1] = s;
  }
  int wb = (int)(nframes * 2 * sizeof(int16_t));
  int wr = esp_codec_dev_write(spk, pcm, wb);
  free(pcm);
  esp_codec_dev_close(spk);
  (void)audio_output_release_external(OWNER_TAG, true);
  if (wr != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "codec write failed");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "sedentary alert played");
  return ESP_OK;
}
