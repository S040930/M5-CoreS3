#include "sedentary_local_detect.h"

#include "sedentary_camera.h"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_SEDENTARY_ENABLE
#define CONFIG_SEDENTARY_ENABLE 0
#endif

#if CONFIG_SEDENTARY_ENABLE

static const char *TAG = "sed_loc";
static const char *k_nvs_ns = "sed_mon";
static const char *k_nvs_key = "cal_v1";

#define CAL_MAGIC 0x53454c43u /* 'SELC' */
#define CAL_VER 1u

#define SED_W 160
#define SED_H 120
#define SED_PIX ((unsigned)(SED_W * SED_H))

typedef struct {
  uint32_t magic;
  uint16_t ver;
  uint16_t fw;
  uint16_t fh;
  uint16_t roi_x;
  uint16_t roi_y;
  uint16_t roi_w;
  uint16_t roi_h;
  uint8_t baseline[SED_PIX];
} sed_cal_blob_t;

static sed_cal_blob_t s_cal;
static bool s_cal_ram_valid;

static int s_streak_present;
static int s_streak_absent;

#if !CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
static bool s_no_cal_baseline_done;
#endif

static esp_err_t nvs_save_blob(const sed_cal_blob_t *b) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(k_nvs_ns, NVS_READWRITE, &h);
  if (e != ESP_OK) {
    return e;
  }
  e = nvs_set_blob(h, k_nvs_key, b, sizeof(*b));
  if (e == ESP_OK) {
    e = nvs_commit(h);
  }
  nvs_close(h);
  return e;
}

static esp_err_t nvs_load_blob(sed_cal_blob_t *b) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(k_nvs_ns, NVS_READONLY, &h);
  if (e != ESP_OK) {
    return e;
  }
  size_t sz = sizeof(*b);
  e = nvs_get_blob(h, k_nvs_key, b, &sz);
  nvs_close(h);
  if (e != ESP_OK || sz != sizeof(*b)) {
    return ESP_ERR_NOT_FOUND;
  }
  return ESP_OK;
}

static bool blob_valid_for_detect(const sed_cal_blob_t *b) {
  if (b->magic != CAL_MAGIC || b->ver != CAL_VER) {
    return false;
  }
  if (b->fw != SED_W || b->fh != SED_H) {
    return false;
  }
  if (b->roi_w == 0 || b->roi_h == 0) {
    return false;
  }
  if ((unsigned)b->roi_x + (unsigned)b->roi_w > SED_W || (unsigned)b->roi_y + (unsigned)b->roi_h > SED_H) {
    return false;
  }
  return true;
}

static bool blob_valid_empty_only(const sed_cal_blob_t *b) {
  return (b->magic == CAL_MAGIC && b->ver == CAL_VER && b->fw == SED_W && b->fh == SED_H && b->roi_w == 0 &&
          b->roi_h == 0);
}

static void clamp_roi(int *x, int *y, int *w, int *h, int margin) {
  *x -= margin;
  *y -= margin;
  *w += margin * 2;
  *h += margin * 2;
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*x + *w > SED_W) {
    *w = SED_W - *x;
  }
  if (*y + *h > SED_H) {
    *h = SED_H - *y;
  }
  if (*w < 8) {
    *w = 8;
  }
  if (*h < 8) {
    *h = 8;
  }
  if (*x + *w > SED_W) {
    *x = SED_W - *w;
  }
  if (*y + *h > SED_H) {
    *y = SED_H - *h;
  }
}

static esp_err_t compute_roi_from_diff(const uint8_t *present, const uint8_t *empty, int *rx, int *ry, int *rw,
                                       int *rh) {
  const int edge = 14;
  int minx = SED_W, miny = SED_H, maxx = -1, maxy = -1;
  for (int y = 0; y < SED_H; ++y) {
    for (int x = 0; x < SED_W; ++x) {
      unsigned i = (unsigned)(y * SED_W + x);
      int d = (int)present[i] - (int)empty[i];
      if (d < 0) {
        d = -d;
      }
      if (d >= edge) {
        if (x < minx) {
          minx = x;
        }
        if (y < miny) {
          miny = y;
        }
        if (x > maxx) {
          maxx = x;
        }
        if (y > maxy) {
          maxy = y;
        }
      }
    }
  }
  if (maxx < minx || maxy < miny) {
    return ESP_FAIL;
  }
  *rx = minx;
  *ry = miny;
  *rw = maxx - minx + 1;
  *rh = maxy - miny + 1;
  return ESP_OK;
}

static void stats_roi(const sed_cal_blob_t *cal, const uint8_t *cur, uint32_t *mean_abs) {
  unsigned sx = cal->roi_x;
  unsigned sy = cal->roi_y;
  unsigned rw = cal->roi_w;
  unsigned rh = cal->roi_h;
  uint64_t sum = 0;
  for (unsigned y = 0; y < rh; ++y) {
    for (unsigned x = 0; x < rw; ++x) {
      unsigned i = (sy + y) * SED_W + (sx + x);
      int d = (int)cur[i] - (int)cal->baseline[i];
      if (d < 0) {
        d = -d;
      }
      sum += (unsigned)d;
    }
  }
  unsigned np = rw * rh;
  *mean_abs = (uint32_t)(np ? (sum / (uint64_t)np) : 0u);
}

void sedentary_local_detect_on_boot(void) {
#if CONFIG_SEDENTARY_LOCAL_RECAL_ON_BOOT
  nvs_handle_t h;
  if (nvs_open(k_nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
    (void)nvs_erase_all(h);
    (void)nvs_commit(h);
    nvs_close(h);
  }
#endif
  s_cal_ram_valid = false;
  s_streak_present = 0;
  s_streak_absent = 0;
#if !CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  s_no_cal_baseline_done = false;
#endif
}

bool sedentary_calibration_is_ready(void) {
#if !CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  return true;
#else
  if (!s_cal_ram_valid) {
    sed_cal_blob_t tmp;
    if (nvs_load_blob(&tmp) != ESP_OK) {
      return false;
    }
    if (!blob_valid_for_detect(&tmp)) {
      return false;
    }
    memcpy(&s_cal, &tmp, sizeof(s_cal));
    s_cal_ram_valid = true;
  }
  return blob_valid_for_detect(&s_cal);
#endif
}

esp_err_t sedentary_calibration_begin_empty(void) {
#if !CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  return ESP_OK;
#else
  sedentary_camera_small_frame_t fr = {0};
  esp_err_t c = sedentary_camera_capture_frame_small(&fr);
  if (c != ESP_OK) {
    return c;
  }
  if (fr.width != SED_W || fr.height != SED_H || fr.gray == NULL) {
    sedentary_camera_free_small_frame(&fr);
    return ESP_ERR_INVALID_SIZE;
  }
  sed_cal_blob_t b;
  memset(&b, 0, sizeof(b));
  b.magic = CAL_MAGIC;
  b.ver = (uint16_t)CAL_VER;
  b.fw = SED_W;
  b.fh = SED_H;
  b.roi_x = 0;
  b.roi_y = 0;
  b.roi_w = 0;
  b.roi_h = 0;
  memcpy(b.baseline, fr.gray, SED_PIX);
  sedentary_camera_free_small_frame(&fr);
  esp_err_t e = nvs_save_blob(&b);
  if (e != ESP_OK) {
    return e;
  }
  memcpy(&s_cal, &b, sizeof(s_cal));
  s_cal_ram_valid = true;
  s_streak_present = 0;
  s_streak_absent = 0;
  ESP_LOGI(TAG, "calibration step 1: empty baseline saved");
  return ESP_OK;
#endif
}

esp_err_t sedentary_calibration_begin_present(void) {
#if !CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  return ESP_ERR_NOT_SUPPORTED;
#else
  sed_cal_blob_t b;
  esp_err_t e = nvs_load_blob(&b);
  if (e != ESP_OK) {
    return e;
  }
  if (!blob_valid_empty_only(&b)) {
    return ESP_ERR_INVALID_STATE;
  }

  sedentary_camera_small_frame_t fr = {0};
  e = sedentary_camera_capture_frame_small(&fr);
  if (e != ESP_OK) {
    return e;
  }
  if (fr.width != SED_W || fr.height != SED_H || fr.gray == NULL) {
    sedentary_camera_free_small_frame(&fr);
    return ESP_ERR_INVALID_SIZE;
  }

  int rx = 0, ry = 0, rw = 0, rh = 0;
  e = compute_roi_from_diff(fr.gray, b.baseline, &rx, &ry, &rw, &rh);
  sedentary_camera_free_small_frame(&fr);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "present calibration: no motion vs empty");
    return e;
  }
  int m = (int)CONFIG_SEDENTARY_LOCAL_ROI_MARGIN_PX;
  clamp_roi(&rx, &ry, &rw, &rh, m);
  b.roi_x = (uint16_t)rx;
  b.roi_y = (uint16_t)ry;
  b.roi_w = (uint16_t)rw;
  b.roi_h = (uint16_t)rh;
  e = nvs_save_blob(&b);
  if (e != ESP_OK) {
    return e;
  }
  memcpy(&s_cal, &b, sizeof(s_cal));
  s_cal_ram_valid = true;
  s_streak_present = 0;
  s_streak_absent = 0;
  ESP_LOGI(TAG, "calibration step 2: ROI %" PRIu16 ",%" PRIu16 " %" PRIu16 "x%" PRIu16, b.roi_x, b.roi_y, b.roi_w,
           b.roi_h);
  return ESP_OK;
#endif
}

esp_err_t sedentary_local_detect_run(sedentary_detect_result_t *out) {
  if (out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  out->valid = true;
  out->clazz = SEDENTARY_DETECT_UNKNOWN;

#if !CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  if (!s_no_cal_baseline_done) {
    sedentary_camera_small_frame_t fr0 = {0};
    esp_err_t c0 = sedentary_camera_capture_frame_small(&fr0);
    if (c0 != ESP_OK) {
      return c0;
    }
    if (fr0.width != SED_W || fr0.height != SED_H || fr0.gray == NULL) {
      sedentary_camera_free_small_frame(&fr0);
      return ESP_ERR_INVALID_SIZE;
    }
    memset(&s_cal, 0, sizeof(s_cal));
    s_cal.magic = CAL_MAGIC;
    s_cal.ver = (uint16_t)CAL_VER;
    s_cal.fw = SED_W;
    s_cal.fh = SED_H;
    memcpy(s_cal.baseline, fr0.gray, SED_PIX);
    s_cal.roi_x = 0;
    s_cal.roi_y = 0;
    s_cal.roi_w = SED_W;
    s_cal.roi_h = SED_H;
    sedentary_camera_free_small_frame(&fr0);
    s_no_cal_baseline_done = true;
    s_cal_ram_valid = true;
    out->valid = false;
    out->clazz = SEDENTARY_DETECT_UNKNOWN;
    ESP_LOGW(TAG, "no-calibration mode: first frame captured as baseline (desk should be empty)");
    return ESP_OK;
  }
#endif

#if CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  if (!sedentary_calibration_is_ready()) {
    out->valid = false;
    return ESP_ERR_INVALID_STATE;
  }
#endif

  sedentary_camera_small_frame_t fr = {0};
  esp_err_t c = sedentary_camera_capture_frame_small(&fr);
  if (c != ESP_OK) {
    return c;
  }
  if (fr.width != SED_W || fr.height != SED_H || fr.gray == NULL) {
    sedentary_camera_free_small_frame(&fr);
    return ESP_ERR_INVALID_SIZE;
  }

#if CONFIG_SEDENTARY_LOCAL_ENABLE_CALIBRATION
  if (!s_cal_ram_valid) {
    sed_cal_blob_t tmp;
    if (nvs_load_blob(&tmp) != ESP_OK || !blob_valid_for_detect(&tmp)) {
      sedentary_camera_free_small_frame(&fr);
      out->valid = false;
      return ESP_ERR_INVALID_STATE;
    }
    memcpy(&s_cal, &tmp, sizeof(s_cal));
    s_cal_ram_valid = true;
  }
#endif

  uint32_t mean_abs = 0;
  stats_roi(&s_cal, fr.gray, &mean_abs);
  sedentary_camera_free_small_frame(&fr);

  uint32_t pthr = (uint32_t)CONFIG_SEDENTARY_LOCAL_PRESENT_THRESHOLD;
  uint32_t athr = (uint32_t)CONFIG_SEDENTARY_LOCAL_ABSENT_THRESHOLD;
  if (athr >= pthr && pthr > 2u) {
    athr = pthr - 2u;
  }
  const int need_p = (int)CONFIG_SEDENTARY_LOCAL_PRESENT_CONFIRM_COUNT;
  const int need_a = (int)CONFIG_SEDENTARY_LOCAL_ABSENT_CONFIRM_COUNT;

  if (mean_abs >= pthr) {
    s_streak_present++;
    s_streak_absent = 0;
  } else if (mean_abs <= athr) {
    s_streak_absent++;
    s_streak_present = 0;
  } else {
    s_streak_present = 0;
    s_streak_absent = 0;
    out->clazz = SEDENTARY_DETECT_UNKNOWN;
    return ESP_OK;
  }

  if (s_streak_present >= need_p) {
    out->clazz = SEDENTARY_DETECT_PRESENT;
    return ESP_OK;
  }
  if (s_streak_absent >= need_a) {
    out->clazz = SEDENTARY_DETECT_ABSENT;
    return ESP_OK;
  }

  out->clazz = SEDENTARY_DETECT_UNKNOWN;
  return ESP_OK;
}

#else /* !CONFIG_SEDENTARY_ENABLE */

void sedentary_local_detect_on_boot(void) {}

bool sedentary_calibration_is_ready(void) { return false; }

esp_err_t sedentary_calibration_begin_empty(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t sedentary_calibration_begin_present(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t sedentary_local_detect_run(sedentary_detect_result_t *out) {
  if (out) {
    memset(out, 0, sizeof(*out));
  }
  return ESP_ERR_NOT_SUPPORTED;
}

#endif
