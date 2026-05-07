#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint32_t sample_rate_hz;
  uint8_t channels;
  uint8_t bits_per_sample;
} voice_pcm_format_t;

static inline bool voice_pcm_format_is_valid(const voice_pcm_format_t *fmt) {
  return fmt != NULL && fmt->sample_rate_hz > 0 && fmt->channels > 0 && fmt->bits_per_sample > 0;
}

static inline bool voice_pcm_format_matches(const voice_pcm_format_t *lhs,
                                            const voice_pcm_format_t *rhs) {
  return lhs != NULL && rhs != NULL && lhs->sample_rate_hz == rhs->sample_rate_hz &&
         lhs->channels == rhs->channels && lhs->bits_per_sample == rhs->bits_per_sample;
}

#ifdef __cplusplus
extern "C" {
#endif

void *voice_buf_alloc(size_t bytes);
void *voice_dma_buf_alloc(size_t bytes);
void voice_buf_free(void *p);

uint64_t voice_now_ms(void);

uint32_t voice_hw_codec_rate_hz(void);
int voice_hw_mclk_multiple(uint32_t rate);
voice_pcm_format_t voice_pcm_speaker_native_format(void);
voice_pcm_format_t voice_pcm_cloud_default_format(void);

#ifdef __cplusplus
}
#endif
