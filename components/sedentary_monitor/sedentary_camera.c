#include "sedentary_camera.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_SEDENTARY_ENABLE
#include "esp_camera.h"
#endif

#include <stdlib.h>
#include <string.h>

#if CONFIG_SEDENTARY_ENABLE

static const char *TAG = "sed_cam";
static bool s_inited;

/* M5Stack CoreS3 + GC0308 (official pin table, docs.m5stack.com core CoreS3). */
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  CONFIG_SEDENTARY_CAM_XCLK_GPIO

#define CAM_PIN_SIOD  12
#define CAM_PIN_SIOC  11

#define CAM_PIN_D0    39
#define CAM_PIN_D1    40
#define CAM_PIN_D2    41
#define CAM_PIN_D3    42
#define CAM_PIN_D4    15
#define CAM_PIN_D5    16
#define CAM_PIN_D6    48
#define CAM_PIN_D7    47

#define CAM_PIN_VSYNC 46
#define CAM_PIN_HREF  38
#define CAM_PIN_PCLK  45

void sedentary_camera_free_small_frame(sedentary_camera_small_frame_t *f) {
  if (f == NULL) {
    return;
  }
  free(f->gray);
  f->gray = NULL;
  f->width = f->height = 0;
}

esp_err_t sedentary_camera_init(void) {
  if (s_inited) {
    return ESP_OK;
  }

  camera_config_t config = {
      .pin_pwdn = CAM_PIN_PWDN,
      .pin_reset = CAM_PIN_RESET,
      .pin_xclk = CAM_PIN_XCLK,
      .pin_sccb_sda = CAM_PIN_SIOD,
      .pin_sccb_scl = CAM_PIN_SIOC,
      .pin_d7 = CAM_PIN_D7,
      .pin_d6 = CAM_PIN_D6,
      .pin_d5 = CAM_PIN_D5,
      .pin_d4 = CAM_PIN_D4,
      .pin_d3 = CAM_PIN_D3,
      .pin_d2 = CAM_PIN_D2,
      .pin_d1 = CAM_PIN_D1,
      .pin_d0 = CAM_PIN_D0,
      .pin_vsync = CAM_PIN_VSYNC,
      .pin_href = CAM_PIN_HREF,
      .pin_pclk = CAM_PIN_PCLK,
      .xclk_freq_hz = 20000000,
      .ledc_timer = LEDC_TIMER_0,
      .ledc_channel = LEDC_CHANNEL_0,
      .pixel_format = PIXFORMAT_GRAYSCALE,
      .frame_size = FRAMESIZE_QQVGA,
      .jpeg_quality = 12,
      .fb_count = 1,
      .fb_location = CAMERA_FB_IN_PSRAM,
      .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
  };

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
    return err;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    (void)s->set_pixformat(s, PIXFORMAT_GRAYSCALE);
    (void)s->set_framesize(s, FRAMESIZE_QQVGA);
  }

  s_inited = true;
  ESP_LOGI(TAG, "camera ready (GC0308 grayscale QQVGA)");
  return ESP_OK;
}

void sedentary_camera_deinit(void) {
  if (!s_inited) {
    return;
  }
  esp_camera_deinit();
  s_inited = false;
}

esp_err_t sedentary_camera_capture_frame_small(sedentary_camera_small_frame_t *out) {
  if (out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  if (!s_inited) {
    return ESP_ERR_INVALID_STATE;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == NULL || fb->buf == NULL || fb->len == 0) {
    ESP_LOGW(TAG, "fb_get failed");
    return ESP_FAIL;
  }
  size_t nbytes = (size_t)fb->width * (size_t)fb->height;
  if (fb->len < nbytes) {
    esp_camera_fb_return(fb);
    ESP_LOGW(TAG, "fb len %u < expected %zu", (unsigned)fb->len, nbytes);
    return ESP_FAIL;
  }
  uint8_t *copy = (uint8_t *)malloc(nbytes);
  if (copy == NULL) {
    esp_camera_fb_return(fb);
    return ESP_ERR_NO_MEM;
  }
  memcpy(copy, fb->buf, nbytes);
  out->width = (uint16_t)fb->width;
  out->height = (uint16_t)fb->height;
  out->gray = copy;
  esp_camera_fb_return(fb);
  return ESP_OK;
}

esp_err_t sedentary_camera_capture_jpeg_debug(uint8_t **out_buf, size_t *out_len) {
  (void)out_buf;
  (void)out_len;
  return ESP_ERR_NOT_SUPPORTED;
}

#else /* !CONFIG_SEDENTARY_ENABLE */

void sedentary_camera_free_small_frame(sedentary_camera_small_frame_t *f) {
  (void)f;
}

esp_err_t sedentary_camera_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void sedentary_camera_deinit(void) {}

esp_err_t sedentary_camera_capture_frame_small(sedentary_camera_small_frame_t *out) {
  (void)out;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sedentary_camera_capture_jpeg_debug(uint8_t **out_buf, size_t *out_len) {
  (void)out_buf;
  (void)out_len;
  return ESP_ERR_NOT_SUPPORTED;
}

#endif
