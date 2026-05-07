#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_OWNER_TAG "realtime_voice"
#define VOICE_HW_CHANNELS 2
#define VOICE_HW_CHANNEL_MASK 0x03

typedef void (*voice_airplay_refresh_cb_t)(void);

void voice_speaker_set_airplay_refresh_cb(voice_airplay_refresh_cb_t cb);

void voice_speaker_set_handles(esp_codec_dev_handle_t mic, esp_codec_dev_handle_t spk);

bool voice_speaker_acquire(bool stop_worker);
void voice_speaker_release(void);

bool voice_speaker_spk_open(void);
void voice_speaker_spk_close(void);

bool voice_speaker_playback_active(void);
void voice_speaker_set_playback_active(bool active);
bool voice_speaker_is_owned(void);
void voice_speaker_set_playout_prefilled(bool v);
bool voice_speaker_playout_prefilled(void);

bool voice_speaker_workbufs_ensure(size_t pop_samples, size_t hw_samples, size_t stereo_samples);
void voice_speaker_workbufs_release(void);

int16_t *voice_speaker_pop_buf(void);
size_t voice_speaker_pop_cap(void);
int16_t *voice_speaker_hw_buf(void);
size_t voice_speaker_hw_cap(void);
int16_t *voice_speaker_stereo_buf(void);
size_t voice_speaker_stereo_cap(void);

esp_codec_dev_handle_t voice_speaker_spk_handle(void);

float soft_limit_f32(float sample);
int16_t soft_clip_i16(int16_t sample);
int16_t voice_peak_abs_i16(const int16_t *samples, size_t count);

#ifdef __cplusplus
}
#endif
