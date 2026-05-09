#pragma once

#include "event_ids.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void voice_publish_wakeword_detected(void);
void voice_publish_speech_start(void);
void voice_publish_speech_end(void);
void voice_publish_request_send(const char *request_id);
void voice_publish_response_audio(void);
void voice_publish_response_text(const char *text);
void voice_publish_playback_start(void);
void voice_publish_playback_end(void);
void voice_publish_error(const char *error_msg);

#ifdef __cplusplus
}
#endif
