
#pragma once

#include "voice_internal_types.h"
#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_frontend_v2_init(esp_codec_dev_handle_t mic_dev, voice_frontend_event_cb_t event_cb, void* user_data);
void voice_frontend_v2_deinit(void);

esp_err_t voice_frontend_v2_start(void);
esp_err_t voice_frontend_v2_stop(void);
esp_err_t voice_frontend_v2_pause(const char* reason);
esp_err_t voice_frontend_v2_resume(void);

bool voice_frontend_v2_is_paused(void);
bool voice_frontend_v2_is_running(void);

const char* voice_frontend_v2_last_pause_reason(void);

#ifdef __cplusplus
}
#endif
