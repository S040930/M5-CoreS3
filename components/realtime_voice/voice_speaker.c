#include "voice_speaker.h"

static esp_codec_dev_handle_t s_mic = NULL;
static esp_codec_dev_handle_t s_spk = NULL;

void voice_speaker_set_handles(esp_codec_dev_handle_t mic, esp_codec_dev_handle_t spk) {
    s_mic = mic;
    s_spk = spk;
}
