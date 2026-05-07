
#pragma once

#include "voice_internal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_api_client_init(const voice_api_config_t* config);
void voice_api_client_deinit(void);

esp_err_t voice_api_client_send_audio(const voice_audio_request_t* request,
                                       voice_response_audio_cb_t audio_cb,
                                       void* audio_cb_user_data,
                                       char* out_text,
                                       size_t out_text_len);

esp_err_t voice_api_client_send_text(const char* text,
                                     voice_response_audio_cb_t audio_cb,
                                     void* audio_cb_user_data,
                                     char* out_text,
                                     size_t out_text_len);

#ifdef __cplusplus
}
#endif
