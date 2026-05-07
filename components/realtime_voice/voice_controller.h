
#pragma once

#include "voice_internal_types.h"
#include "realtime_voice.h"
#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_controller_init(const realtime_voice_config_t* config,
                                esp_codec_dev_handle_t mic_dev,
                                esp_codec_dev_handle_t spk_dev);

void voice_controller_deinit(void);

esp_err_t voice_controller_start(void);
void voice_controller_stop(void);

void voice_controller_set_enabled(bool enabled);
bool voice_controller_is_enabled(void);

void voice_controller_on_airplay_active(bool active);

device_mode_t voice_controller_get_mode(void);

void voice_controller_notify_user_speech_start(void);
void voice_controller_interrupt_response(void);
void voice_controller_reset_session(void);

esp_err_t voice_controller_speak_text(const char* text);

bool voice_controller_is_activation_armed(void);
bool voice_controller_is_response_active(void);

#ifdef __cplusplus
}
#endif
