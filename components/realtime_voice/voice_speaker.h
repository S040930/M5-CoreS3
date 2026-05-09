#pragma once

#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

void voice_speaker_set_handles(esp_codec_dev_handle_t mic, esp_codec_dev_handle_t spk);

#ifdef __cplusplus
}
#endif
