#include "voice_speaker.h"

#include "audio/audio_output.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "resource/resource_manager.h"
#include "sdkconfig.h"
#include "voice_common.h"
#include "voice_dsp.h"
#include "voice_playout.h"
#include "voice_playout_drain.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"

#define TAG "voice_speaker"

static esp_codec_dev_handle_t s_mic = NULL;
static esp_codec_dev_handle_t s_spk = NULL;
static bool s_speaker_owned = false;
static bool s_playback_active = false;
static bool s_playout_prefilled = false;

static voice_airplay_refresh_cb_t s_refresh_cb;

void voice_speaker_set_airplay_refresh_cb(voice_airplay_refresh_cb_t cb) { s_refresh_cb = cb; }

static int16_t *s_pop_buf = NULL;
static size_t s_pop_cap = 0;
static int16_t *s_hw_buf = NULL;
static size_t s_hw_cap = 0;
static int16_t *s_stereo_buf = NULL;
static size_t s_stereo_cap = 0;

void voice_speaker_set_handles(esp_codec_dev_handle_t mic, esp_codec_dev_handle_t spk) {
  s_mic = mic;
  s_spk = spk;
}

void voice_speaker_release(void) {
  if (!s_speaker_owned) {
    return;
  }
  voice_play_rs_reset();
  esp_err_t err = audio_output_release_external(VOICE_OWNER_TAG, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "speaker ownership release failed: %s", esp_err_to_name(err));
  }
  s_speaker_owned = false;
  if (s_refresh_cb) s_refresh_cb();
  resource_manager_release(RESOURCE_OWNER_VOICE);
}

bool voice_speaker_acquire(bool stop_worker) {
  if (s_speaker_owned) {
    if (stop_worker) {
      esp_err_t rm_err = resource_manager_acquire(RESOURCE_OWNER_VOICE);
      if (rm_err != ESP_OK) {
        ESP_LOGW(TAG, "resource manager voice acquire failed: %s", esp_err_to_name(rm_err));
        return false;
      }
      esp_err_t err = audio_output_acquire_external(VOICE_OWNER_TAG, true);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker ownership upgrade failed: %s", esp_err_to_name(err));
        return false;
      }
    }
    return true;
  }
  esp_err_t rm_err = resource_manager_acquire(RESOURCE_OWNER_VOICE);
  if (rm_err != ESP_OK) {
    ESP_LOGW(TAG, "resource manager voice acquire failed: %s", esp_err_to_name(rm_err));
    return false;
  }
  esp_err_t err = audio_output_acquire_external(VOICE_OWNER_TAG, stop_worker);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "speaker ownership acquire failed: %s", esp_err_to_name(err));
    return false;
  }
  s_speaker_owned = true;
  return true;
}

bool voice_speaker_spk_open(void) {
  if (s_spk == NULL) {
    return false;
  }
  if (s_playback_active) {
    return true;
  }
  if (!voice_speaker_acquire(true)) {
    ESP_LOGW(TAG, "spk_open failed: speaker ownership unavailable");
    return false;
  }
  if (!voice_rs_play_ensure()) {
    ESP_LOGE(TAG, "playout resampler init failed: internal_free=%lu internal_largest=%lu spiram_free=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    voice_speaker_release();
    return false;
  }
  audio_output_diag_t diag = {0};
  if (audio_output_get_diag(&diag) == ESP_OK) {
    ESP_LOGI(TAG, "voice playback volume diag: target=%.1f current=%.1f hw=%d muted=%d",
             diag.target_volume_db, diag.current_volume_db, diag.volume,
             diag.muted ? 1 : 0);
  }
  {
    voice_pcm_format_t stream_fmt = voice_playout_stream_format();
    if (!voice_pcm_format_is_valid(&stream_fmt)) {
      stream_fmt = voice_pcm_cloud_default_format();
    }
    const uint32_t output_rate = stream_fmt.sample_rate_hz;
    // Keep speaker workbuf sizing in lockstep with the actual playout drain batch size.
    const size_t drain_chunk = output_rate * VOICE_PLAYOUT_DRAIN_CHUNK_MS / 1000U;
    size_t hw_mono_cap = drain_chunk;
    if (voice_rs_play_rs() != NULL) {
      hw_mono_cap = voice_rs_output_cap(drain_chunk, voice_rs_play_ratio());
    }
    size_t stereo_cap = hw_mono_cap * VOICE_HW_CHANNELS;
    if (!voice_speaker_workbufs_ensure(drain_chunk, hw_mono_cap, stereo_cap)) {
      ESP_LOGE(TAG,
               "playout workbuf alloc failed: pop=%lu hw=%lu stereo=%lu internal_free=%lu internal_largest=%lu spiram_free=%lu",
               (unsigned long)drain_chunk, (unsigned long)hw_mono_cap, (unsigned long)stereo_cap,
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
      voice_speaker_release();
      return false;
    }
    ESP_LOGI(TAG, "spk_open workbufs: pop=%lu hw=%lu stereo=%lu resampler=%d",
             (unsigned long)s_pop_cap, (unsigned long)s_hw_cap, (unsigned long)s_stereo_cap,
             voice_rs_play_rs() != NULL ? 1 : 0);
  }
  const uint32_t hw_hz = voice_hw_codec_rate_hz();
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = VOICE_HW_CHANNELS,
      .channel_mask = VOICE_HW_CHANNEL_MASK,
      .sample_rate = hw_hz,
      .mclk_multiple = voice_hw_mclk_multiple(hw_hz),
  };
  if (audio_output_external_open(hw_hz) != ESP_OK) {
    ESP_LOGW(TAG, "spk: codec open failed rate=%lu", (unsigned long)fs.sample_rate);
    voice_speaker_release();
    return false;
  }
  ESP_LOGI(TAG, "spk: codec open success, rate=%d resampler=%d",
           (int)fs.sample_rate, voice_rs_play_rs() != NULL ? 1 : 0);
  s_playout_prefilled = false;
  s_playback_active = true;
  return true;
}

void voice_speaker_spk_close(void) {
  if (!s_playback_active) {
    return;
  }
  s_playback_active = false;
  audio_output_external_close();
  voice_playout_reset();
  voice_play_rs_reset();
  voice_speaker_release();
}

bool voice_speaker_playback_active(void) { return s_playback_active; }
void voice_speaker_set_playback_active(bool active) { s_playback_active = active; }
bool voice_speaker_is_owned(void) { return s_speaker_owned; }
void voice_speaker_set_playout_prefilled(bool v) { s_playout_prefilled = v; }
bool voice_speaker_playout_prefilled(void) { return s_playout_prefilled; }

bool voice_speaker_workbufs_ensure(size_t pop_samples, size_t hw_samples, size_t stereo_samples) {
  if (s_pop_cap < pop_samples) {
    int16_t *tmp = (int16_t *)voice_buf_alloc(pop_samples * sizeof(int16_t));
    if (tmp != NULL) {
      voice_buf_free(s_pop_buf);
      s_pop_buf = tmp;
      s_pop_cap = pop_samples;
    }
  }
  if (s_hw_cap < hw_samples) {
    int16_t *tmp = (int16_t *)voice_buf_alloc(hw_samples * sizeof(int16_t));
    if (tmp != NULL) {
      voice_buf_free(s_hw_buf);
      s_hw_buf = NULL;
      s_hw_buf = tmp;
      s_hw_cap = hw_samples;
    }
  }
  if (s_stereo_cap < stereo_samples) {
    int16_t *tmp = (int16_t *)voice_buf_alloc(stereo_samples * sizeof(int16_t));
    if (tmp != NULL) {
      voice_buf_free(s_stereo_buf);
      s_stereo_buf = tmp;
      s_stereo_cap = stereo_samples;
    }
  }
  return s_pop_buf != NULL && s_hw_buf != NULL && s_stereo_buf != NULL;
}

void voice_speaker_workbufs_release(void) {
  voice_buf_free(s_pop_buf);
  s_pop_buf = NULL;
  s_pop_cap = 0;
  voice_buf_free(s_hw_buf);
  s_hw_buf = NULL;
  s_hw_cap = 0;
  voice_buf_free(s_stereo_buf);
  s_stereo_buf = NULL;
  s_stereo_cap = 0;
}

int16_t *voice_speaker_pop_buf(void) { return s_pop_buf; }
size_t voice_speaker_pop_cap(void) { return s_pop_cap; }
int16_t *voice_speaker_hw_buf(void) { return s_hw_buf; }
size_t voice_speaker_hw_cap(void) { return s_hw_cap; }
int16_t *voice_speaker_stereo_buf(void) { return s_stereo_buf; }
size_t voice_speaker_stereo_cap(void) { return s_stereo_cap; }

esp_codec_dev_handle_t voice_speaker_spk_handle(void) { return s_spk; }

float soft_limit_f32(float sample) {
  const float limit = 0.95f;
  if (sample > limit) {
    return limit + (1.0f - limit) * tanhf((sample - limit) / (1.0f - limit));
  }
  if (sample < -limit) {
    return -limit - (1.0f - limit) * tanhf((-sample - limit) / (1.0f - limit));
  }
  return sample;
}

int16_t soft_clip_i16(int16_t sample) {
  float f = (float)sample / 32768.0f;
  if (f > 0.95f || f < -0.95f) {
    f = soft_limit_f32(f);
    int v = (int)(f * 32768.0f + (f > 0 ? 0.5f : -0.5f));
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
  }
  return sample;
}

int16_t voice_peak_abs_i16(const int16_t *samples, size_t count) {
  int16_t peak = 0;
  if (samples == NULL) {
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    int32_t v = samples[i];
    int32_t mag = v < 0 ? -v : v;
    if (mag > 32767) {
      mag = 32767;
    }
    if ((int16_t)mag > peak) {
      peak = (int16_t)mag;
    }
  }
  return peak;
}
