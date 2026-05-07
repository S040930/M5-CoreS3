#include "voice_playout_drain.h"
#include "voice_common.h"
#include "voice_dsp.h"
#include "voice_frontend_v2.h"
#include "voice_playout.h"
#include "voice_speaker.h"
#include "audio/audio_output.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <math.h>
#include <string.h>

#define TAG "voice_playout_drain"
#define VOICE_PLAYBACK_PEAK_LOG_INTERVAL 40U
#define VOICE_PLAYBACK_CLIP_RISK_PEAK 30000
#define VOICE_PLAYBACK_WRITE_WARN_MS 50U
#define VOICE_PLAYOUT_OUTPUT_HEADROOM_Q15 27853 /* ~0.85 */

static int16_t apply_playout_headroom_and_clip(int16_t sample) {
  int32_t scaled = ((int32_t)sample * VOICE_PLAYOUT_OUTPUT_HEADROOM_Q15) >> 15;
  if (scaled > 32767) {
    scaled = 32767;
  } else if (scaled < -32768) {
    scaled = -32768;
  }
  return soft_clip_i16((int16_t)scaled);
}

void voice_playout_drain(const voice_playout_drain_input_t *in,
                         voice_playout_drain_output_t *out) {
  if (voice_speaker_spk_handle() == NULL) return;

  voice_pcm_format_t stream_fmt = voice_playout_stream_format();
  if (!voice_pcm_format_is_valid(&stream_fmt)) {
    stream_fmt = voice_pcm_cloud_default_format();
  }
  const uint32_t output_rate = stream_fmt.sample_rate_hz;
  const size_t drain_chunk = output_rate * VOICE_PLAYOUT_DRAIN_CHUNK_MS / 1000U;
  const size_t prefill = (size_t)output_rate * VOICE_PLAYOUT_PREFILL_MS / 1000U;
  const size_t low_threshold = (size_t)output_rate * VOICE_PLAYOUT_LOW_MS / 1000U;
  const size_t resume_threshold =
      (size_t)output_rate * VOICE_PLAYOUT_REBUFFER_RESUME_MS / 1000U;
  const size_t silence_fill_frames =
      (size_t)output_rate * VOICE_PLAYOUT_SILENCE_FILL_MS / 1000U;
  static bool s_rebuffering;
  static bool s_silence_filled;

  if (!voice_speaker_playout_prefilled()) {
    size_t avail = voice_playout_avail();
    if (avail < prefill && !in->response_audio_closed) return;
    if (avail > 0) {
      if (avail < prefill && in->response_audio_closed) {
        ESP_LOGI(TAG, "playout prefill bypass: avail=%lums threshold=%lums audio_closed=1",
                 (unsigned long)(avail * 1000 / output_rate),
                 (unsigned long)VOICE_PLAYOUT_PREFILL_MS);
      }
      voice_speaker_set_playout_prefilled(true);
      voice_playout_set_last_write_ms(voice_now_ms());
      s_rebuffering = false;
      s_silence_filled = false;
      static bool s_format_logged;
      if (!s_format_logged) {
        voice_pcm_format_t speaker_fmt = voice_pcm_speaker_native_format();
        ESP_LOGI(TAG,
                 "playout format contract: stream=%luHz ch=%u bits=%u speaker=%luHz ch=%u bits=%u",
                 (unsigned long)stream_fmt.sample_rate_hz, stream_fmt.channels,
                 stream_fmt.bits_per_sample, (unsigned long)speaker_fmt.sample_rate_hz,
                 speaker_fmt.channels, speaker_fmt.bits_per_sample);
        s_format_logged = true;
      }
    }
  }

  if (voice_speaker_pop_buf() == NULL || voice_speaker_hw_buf() == NULL || voice_speaker_stereo_buf() == NULL) {
    return;
  }
  int16_t *pop_buf = voice_speaker_pop_buf();
  size_t popped = voice_playout_pop(pop_buf, drain_chunk);

  static uint32_t s_drain_diag_count;
  static int16_t s_silence_buf[4096];

  if (s_rebuffering) {
    size_t avail = voice_playout_avail();
    bool can_resume = avail >= resume_threshold || (in->response_audio_closed && avail > 0);
    if (!can_resume) {
      return;
    }
    ESP_LOGI(TAG, "playout rebuffer resume: avail=%lums threshold=%lums audio_closed=%d",
             (unsigned long)(avail * 1000 / output_rate),
             (unsigned long)VOICE_PLAYOUT_REBUFFER_RESUME_MS,
             in->response_audio_closed ? 1 : 0);
    s_rebuffering = false;
    s_silence_filled = false;
  }

  if (popped > 0) {
    voice_playout_set_last_write_ms(voice_now_ms());
    s_silence_filled = false;

    int16_t *hw_mono = voice_speaker_hw_buf();
    size_t hw_mono_cap = voice_speaker_hw_cap();

    int16_t input_peak = voice_peak_abs_i16(pop_buf, popped);
    size_t hw_frames = popped;
    if (voice_rs_play_rs() != NULL) {
      hw_frames = voice_rs_process_mono(voice_rs_play_rs(), voice_rs_play_ratio(), pop_buf, popped,
                                        hw_mono, hw_mono_cap);
    } else {
      if (popped <= hw_mono_cap) {
        memcpy(hw_mono, pop_buf, popped * sizeof(int16_t));
      }
      else hw_frames = hw_mono_cap;
    }
    if (hw_frames == 0) {
      return;
    }

    // Keep assistant replies comfortably below full scale before the shared
    // speaker path applies its own volume/headroom stages.
    for (size_t i = 0; i < hw_frames; i++) {
      hw_mono[i] = apply_playout_headroom_and_clip(hw_mono[i]);
    }

    int16_t *stereo = voice_speaker_stereo_buf();
    for (size_t i = 0; i < hw_frames; i++) {
      stereo[i * 2] = hw_mono[i];
      stereo[i * 2 + 1] = hw_mono[i];
    }

    voice_playout_save_last_stereo(stereo, hw_frames);

    int16_t output_peak = voice_peak_abs_i16(stereo, hw_frames * VOICE_HW_CHANNELS);
    s_drain_diag_count++;
    if (output_peak >= VOICE_PLAYBACK_CLIP_RISK_PEAK) {
      ESP_LOGW(TAG, "spk: clip-risk input_peak=%d output_peak=%d frames=%lu",
               input_peak, output_peak, (unsigned long)hw_frames);
    } else if ((s_drain_diag_count % VOICE_PLAYBACK_PEAK_LOG_INTERVAL) == 0U) {
      ESP_LOGI(TAG, "spk: pcm peak input=%d output=%d frames=%lu",
               input_peak, output_peak, (unsigned long)hw_frames);
    }
    int wb = (int)(hw_frames * VOICE_HW_CHANNELS * sizeof(int16_t));
    uint64_t write_start_ms = voice_now_ms();
    esp_err_t ret = audio_output_external_write(stereo, (size_t)wb, pdMS_TO_TICKS(40));
    uint64_t write_time_ms = voice_now_ms() - write_start_ms;
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "spk: write failed: %s bytes=%d write_ms=%llums",
               esp_err_to_name(ret), wb, (unsigned long long)write_time_ms);
    } else if (write_time_ms >= VOICE_PLAYBACK_WRITE_WARN_MS) {
      ESP_LOGW(TAG, "spk: slow write bytes=%d frames=%lu write_ms=%llums",
               wb, (unsigned long)hw_frames, (unsigned long long)write_time_ms);
    }
    out->wrote = true;

    {
      static uint64_t s_last_lowmark_log_ms;
      size_t avail_after = voice_playout_avail();
      if (avail_after < low_threshold && in->speaking && !in->response_audio_closed) {
        uint64_t now2 = voice_now_ms();
        if (now2 - s_last_lowmark_log_ms >= 2000ULL) {
          ESP_LOGW(TAG, "playout ring low: avail=%lums threshold=%lums",
                   (unsigned long)(avail_after * 1000 / output_rate),
                   (unsigned long)VOICE_PLAYOUT_LOW_MS);
          s_last_lowmark_log_ms = now2;
        }
      }
      if (avail_after < low_threshold && in->speaking && !in->response_audio_closed) {
        s_rebuffering = true;
        ESP_LOGW(TAG, "playout rebuffer enter: avail=%lums low=%lums resume=%lums",
                 (unsigned long)(avail_after * 1000 / output_rate),
                 (unsigned long)VOICE_PLAYOUT_LOW_MS,
                 (unsigned long)VOICE_PLAYOUT_REBUFFER_RESUME_MS);
      }
    }
  } else {
    uint64_t now = voice_now_ms();
    {
      static uint64_t s_last_underrun_log_ms;
      if (now - s_last_underrun_log_ms >= 3000ULL) {
        ESP_LOGW(TAG, "playout underrun: avail=%lu speaking=%d audio_closed=%d",
                 (unsigned long)voice_playout_avail(), in->speaking ? 1 : 0,
                 in->response_audio_closed ? 1 : 0);
        s_last_underrun_log_ms = now;
      }
    }
    uint64_t last_write = voice_playout_last_write_ms();
    if (!in->response_audio_closed && last_write > 0 &&
        (now - last_write) < VOICE_PLAYOUT_GAP_TIMEOUT_MS) {
      size_t frames = silence_fill_frames > 0 ? silence_fill_frames : drain_chunk;
      if (frames * VOICE_HW_CHANNELS <= sizeof(s_silence_buf) / sizeof(s_silence_buf[0]) &&
          !s_silence_filled) {
        size_t sample_count = frames * VOICE_HW_CHANNELS;
        memset(s_silence_buf, 0, sample_count * sizeof(int16_t));
        uint64_t write_start_ms = voice_now_ms();
        esp_err_t ret = audio_output_external_write(s_silence_buf,
                                                    sample_count * sizeof(int16_t),
                                                    pdMS_TO_TICKS(20));
        uint64_t write_time_ms = voice_now_ms() - write_start_ms;
        if (ret == ESP_OK) {
          out->wrote = true;
          s_silence_filled = true;
        } else {
          ESP_LOGW(TAG, "spk: silence fill failed: %s write_ms=%llums",
                   esp_err_to_name(ret), (unsigned long long)write_time_ms);
        }
      }
      s_rebuffering = true;
    } else if (!in->response_audio_closed && last_write > 0 &&
               (now - last_write) >= VOICE_PLAYOUT_GAP_TIMEOUT_MS) {
      s_rebuffering = true;
    }
  }
  if (in->response_audio_closed && voice_speaker_playback_active() && voice_playout_avail() == 0) {
    s_rebuffering = false;
    s_silence_filled = false;
    voice_speaker_spk_close();
    out->speaker_closed = true;
    out->speaking_done = true;
  }
}
