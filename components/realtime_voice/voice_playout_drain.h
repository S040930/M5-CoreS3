#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_PLAYOUT_DRAIN_CHUNK_MS 10U

typedef struct {
  bool speaking;
  bool response_audio_closed;
} voice_playout_drain_input_t;

typedef struct {
  bool speaker_closed;
  bool speaking_done;
  bool wrote;
} voice_playout_drain_output_t;

void voice_playout_drain(const voice_playout_drain_input_t *in,
                         voice_playout_drain_output_t *out);

#ifdef __cplusplus
}
#endif
