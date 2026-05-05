#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "resampler.h"

#if !defined(CONFIG_RESAMPLER_TAPS)
#define CONFIG_RESAMPLER_TAPS 64
#endif
#define VOICE_DSP_RS_TAPS ((CONFIG_RESAMPLER_TAPS) & ~3)

void *voice_buf_alloc(size_t bytes);
void voice_buf_free(void *p);

uint32_t voice_hw_codec_rate_hz(void);
int voice_hw_mclk_multiple(uint32_t rate);

bool voice_dsp_resampler_create_fixed(Resample **out_rs, double src_hz, double dst_hz,
                                      double *out_ratio);

void voice_rs_destroy_cap(void);
void voice_rs_destroy_play(void);
bool voice_rs_cap_ensure(void);
bool voice_rs_play_ensure(void);
void voice_play_rs_reset(void);
void voice_rs_cap_reset(void);

Resample *voice_rs_cap_rs(void);
double voice_rs_cap_ratio(void);
Resample *voice_rs_play_rs(void);
double voice_rs_play_ratio(void);

size_t voice_rs_output_cap(size_t in_frames, double ratio);
size_t voice_rs_process_mono(Resample *rs, double ratio, const int16_t *in, size_t in_frames,
                             int16_t *out, size_t out_cap);
void voice_rs_free_float_bufs(void);
