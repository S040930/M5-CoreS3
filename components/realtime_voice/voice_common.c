#include "voice_common.h"

#include "audio/audio_output_common.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "voice_speaker.h"

void *voice_buf_alloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  return p;
}

void *voice_dma_buf_alloc(size_t bytes) {
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (p != NULL) return p;
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void voice_buf_free(void *p) {
  if (p != NULL) {
    heap_caps_free(p);
  }
}

uint64_t voice_now_ms(void) {
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t voice_hw_codec_rate_hz(void) {
  return (uint32_t)AO_OUTPUT_RATE;
}

int voice_hw_mclk_multiple(uint32_t rate) {
  switch (rate) {
  case 44100:
  case 88200:
  case 176400:
    return I2S_MCLK_MULTIPLE_384;
  default:
    return 0;
  }
}

voice_pcm_format_t voice_pcm_speaker_native_format(void) {
  voice_pcm_format_t fmt = {
      .sample_rate_hz = voice_hw_codec_rate_hz(),
      .channels = VOICE_HW_CHANNELS,
      .bits_per_sample = 16,
  };
  return fmt;
}

voice_pcm_format_t voice_pcm_cloud_default_format(void) {
  voice_pcm_format_t fmt = {
      .sample_rate_hz = (uint32_t)CONFIG_VOICE_OUTPUT_SAMPLE_RATE,
      .channels = 1,
      .bits_per_sample = 16,
  };
  return fmt;
}
