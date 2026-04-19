#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AO_FRAME_SAMPLES 352
#define AO_OUTPUT_RATE   CONFIG_OUTPUT_SAMPLE_RATE_HZ

#define AO_MAX_RESAMPLE_FRAMES \
  ((size_t)((AO_FRAME_SAMPLES + 2) * ((double)AO_OUTPUT_RATE / 44100) + 16))

#if CONFIG_FREERTOS_UNICORE
#define AO_PLAYBACK_CORE 0
#else
#define AO_PLAYBACK_CORE 1
#endif

typedef struct {
  esp_err_t (*write_pcm)(void *ctx, const int16_t *data, size_t bytes,
                         TickType_t wait);
  void (*flush_output)(void *ctx);
  const char *task_name;
  bool software_volume;
  void *ctx;
} audio_output_hw_ops_t;

void audio_output_common_init(const audio_output_hw_ops_t *ops);
void audio_output_common_start(void);
void audio_output_common_stop(void);
void audio_output_common_flush(void);
void audio_output_common_set_source_rate(int rate);
bool audio_output_common_is_active(void);
