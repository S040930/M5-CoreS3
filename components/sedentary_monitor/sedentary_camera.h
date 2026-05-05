#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t sedentary_camera_init(void);
void sedentary_camera_deinit(void);

/** One QQVGA grayscale frame; `gray` is heap malloc'd. */
typedef struct {
  uint16_t width;
  uint16_t height;
  uint8_t *gray;
} sedentary_camera_small_frame_t;

void sedentary_camera_free_small_frame(sedentary_camera_small_frame_t *f);

/** Capture one small grayscale frame for local detection. */
esp_err_t sedentary_camera_capture_frame_small(sedentary_camera_small_frame_t *out);

/** Optional JPEG snapshot for debugging (not used in main path). */
esp_err_t sedentary_camera_capture_jpeg_debug(uint8_t **out_buf, size_t *out_len);
