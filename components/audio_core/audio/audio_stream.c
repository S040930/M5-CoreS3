#include <stdlib.h>
#include <string.h>

#include "audio_stream.h"

#include "audio_buffer.h"
#include "audio_decoder.h"
#include "audio_receiver_internal.h"
#include "esp_log.h"

static bool apply_aac_transient_mute(audio_receiver_state_t *state,
                                     int16_t *buffer, size_t samples,
                                     int channels) {
  if (!audio_decoder_is_aac(state->decoder)) {
    return false;
  }

  if ((state->blocks_read_in_sequence <= 2) &&
      (state->blocks_read_in_sequence != state->blocks_read)) {
    memset(buffer, 0, samples * channels * sizeof(int16_t));
    return true;
  }

  return false;
}

bool audio_stream_process_frame(audio_receiver_state_t *state,
                                uint32_t timestamp, const uint8_t *audio_data,
                                size_t audio_len) {
  if (!state || !state->decoder) {
    return false;
  }

  // Post-seek RTP window gate: discard frames outside [discard_before_rtp,
  // discard_above_rtp].  The TCP socket buffer can hold many seconds of
  // pre-seek audio; both gates together handle both seek directions:
  //   discard_before_rtp — forward seek: stale frames have lower RTP
  //   discard_above_rtp  — backward seek: stale frames have much higher RTP
  // Each self-disarms on the first frame that passes it.
  if (state->discard_before_rtp_valid) {
    if ((int32_t)(timestamp - state->discard_before_rtp) < 0) {
      return false; // below lower bound — forward-seek stale frame
    }
    state->discard_before_rtp_valid = false;
  }
  if (state->discard_above_rtp_valid) {
    if ((int32_t)(timestamp - state->discard_above_rtp) > 0) {
      return false; // above upper bound — backward-seek stale frame
    }
    state->discard_above_rtp_valid = false;
  }
  size_t capacity_samples = 0;
  int16_t *decode_buffer =
      audio_buffer_get_decode_buffer(&state->buffer, &capacity_samples);
  if (!decode_buffer || capacity_samples == 0) {
    return false;
  }

  audio_decode_info_t info = {0};
  int decoded_samples =
      audio_decoder_decode(state->decoder, audio_data, audio_len, decode_buffer,
                           capacity_samples, &info);
  if (decoded_samples <= 0) {
    return false;
  }

  int channels =
      info.channels > 0 ? info.channels : state->stream->format.channels;
  if (channels <= 0) {
    channels = 2;
  }

  apply_aac_transient_mute(state, decode_buffer, (size_t)decoded_samples,
                           channels);

  // 诊断：入队前检查帧是否为静音
  {
    int16_t frame_peak = 0;
    size_t sample_count = (size_t)decoded_samples * (size_t)channels;
    for (size_t i = 0; i < sample_count; i++) {
      int32_t v = decode_buffer[i];
      int32_t mag = v < 0 ? -v : v;
      if (mag > 32767) mag = 32767;
      if ((int16_t)mag > frame_peak) {
        frame_peak = (int16_t)mag;
      }
    }
    
    if (frame_peak == 0) {
      static uint32_t silent_frame_counter = 0;
      if (++silent_frame_counter % 50 == 0) {
        ESP_LOGW("audio_stream", 
                 "stream_diag: silent frame before queue rtp=%u samples=%d peak=0 count=%lu",
                 timestamp, decoded_samples, (unsigned long)silent_frame_counter);
      }
    }
  }

  return audio_buffer_queue_decoded(&state->buffer, &state->stats, timestamp,
                                    decode_buffer, (size_t)decoded_samples,
                                    channels);
}

void audio_stream_destroy(audio_stream_t *stream) {
  if (!stream) {
    return;
  }

  if (stream->ops && stream->ops->destroy) {
    stream->ops->destroy(stream);
    return;
  }

  free(stream);
}

bool audio_stream_uses_buffer(audio_stream_type_t type) {
  return type == AUDIO_STREAM_BUFFERED;
}
