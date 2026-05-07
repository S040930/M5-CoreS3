
#pragma once

#include "voice_internal_types.h"
#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_player_init(esp_codec_dev_handle_t spk_dev, voice_player_event_cb_t event_cb, void* user_data);
void voice_player_deinit(void);

esp_err_t voice_player_start(void);
esp_err_t voice_player_stop(void);

esp_err_t voice_player_feed(const int16_t* pcm_data, size_t frames, uint32_t sample_rate);

bool voice_player_is_active(void);
bool voice_player_is_buffering(void);
size_t voice_player_available_frames(void);

void voice_player_set_prebuffer_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
