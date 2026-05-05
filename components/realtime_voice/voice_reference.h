#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct resample Resample;

bool voice_reference_ring_init(void);
void voice_reference_ring_deinit(void);
bool voice_reference_ring_is_ready(void);
void voice_reference_ring_push(const int16_t *pcm_mono_16k, size_t samples);
size_t voice_reference_ring_pop(int16_t *dst, size_t max_samples);

void voice_reference_playout_rs_destroy(void);
bool voice_reference_playout_rs_ensure(void);
Resample *voice_reference_playout_rs(void);
double voice_reference_playout_ratio(void);

void voice_reference_airplay_rs_destroy(void);
bool voice_reference_airplay_rs_ensure(uint32_t sample_rate_hz);
void voice_reference_airplay_scratch_free(void);

void voice_reference_airplay_tap(const int16_t *stereo_interleaved, size_t frames,
                                 uint32_t sample_rate_hz, void *ctx);
