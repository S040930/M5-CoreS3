#include "screen_ui.h"

#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "screen_ui";

static bool s_initialized = false;
static bool s_streaming = false;
static bool s_wifi_connected = false;
static bool s_airplay_ready = false;
static uint32_t s_phase = 0;
static screen_ui_state_t s_state = SCREEN_UI_STATE_BOOT;
static screen_ui_metadata_t s_metadata = {0};

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_left_bar = NULL;
static lv_obj_t *s_right_bar = NULL;
static lv_obj_t *s_lyric_label = NULL;
static lv_timer_t *s_anim_timer = NULL;

static lv_obj_t *screen_active(void) {
#if LVGL_VERSION_MAJOR >= 9
  return lv_screen_active();
#else
  return lv_scr_act();
#endif
}

static void delete_obj(lv_obj_t **obj) {
  if (obj == NULL || *obj == NULL) {
    return;
  }
#if LVGL_VERSION_MAJOR >= 9
  lv_obj_delete(*obj);
#else
  lv_obj_del(*obj);
#endif
  *obj = NULL;
}

// Ultra-minimalist mode: no status text helpers needed

// No longer using status labels in ultra-minimalist mode

static void anim_timer_cb(lv_timer_t *timer) {
  (void)timer;
  if (!s_initialized || s_left_bar == NULL || s_right_bar == NULL) {
    return;
  }

  s_phase = (s_phase + 3U) % 80U;
  uint32_t tri = s_phase <= 40U ? s_phase : (80U - s_phase);
  int base_h = s_streaming ? 26 : 8;
  int delta_l = s_streaming ? (int)(tri / 2U) : 0;
  int delta_r = s_streaming ? (int)((40U - tri) / 2U) : 0;
  int left_h = base_h + delta_l;
  int right_h = base_h + delta_r;

  lv_obj_set_size(s_left_bar, 20, left_h);
  lv_obj_set_size(s_right_bar, 20, right_h);
  lv_obj_align(s_left_bar, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_align(s_right_bar, LV_ALIGN_RIGHT_MID, -16, 0);

  // Cyberpunk cycling color logic (Blue -> Purple -> Pink)
  static uint16_t color_hue = 190;
  static int8_t hue_step = 1;
  color_hue += hue_step;
  if (color_hue >= 320) hue_step = -1;
  if (color_hue <= 190) hue_step = 1;

  lv_color_t bar_color = lv_color_hsv_to_rgb(color_hue, 90, 100);
  lv_obj_set_style_bg_color(s_left_bar, bar_color, 0);
  lv_obj_set_style_bg_color(s_right_bar, bar_color, 0);

  // Dynamic glow effect using opa instead of green channel tweaking
  uint8_t opa = (uint8_t)(160U + tri * 2U);
  lv_obj_set_style_bg_opa(s_left_bar, opa, 0);
  lv_obj_set_style_bg_opa(s_right_bar, opa, 0);

  // Real-time track info display in center (matching 'lyric' style)
  if (s_lyric_label != NULL) {
    if (s_streaming) {
      const char *track_text = (s_metadata.title[0] != '\0') ? s_metadata.title : "STREAMING";
      lv_label_set_text(s_lyric_label, track_text);
      // Breathing effect
      uint8_t opa = (uint8_t)(180U + tri * 1.5f);
      lv_obj_set_style_text_opa(s_lyric_label, opa, 0);
    } else {
      lv_label_set_text(s_lyric_label, "READY");
      lv_obj_set_style_text_opa(s_lyric_label, 128, 0);
    }
  }
}

esp_err_t screen_ui_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }

  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = BSP_LCD_H_RES * 30,
      .double_buffer = false,
      .flags =
          {
              .buff_dma = false,
              .buff_spiram = true,
          },
  };
  cfg.lvgl_port_cfg.task_affinity = 1;

  lv_display_t *disp = bsp_display_start_with_config(&cfg);
  if (disp == NULL) {
    ESP_LOGE(TAG, "display start failed");
    return ESP_FAIL;
  }
  (void)disp;

  esp_err_t err = bsp_display_backlight_on();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "backlight on failed: %s", esp_err_to_name(err));
  }

  if (!bsp_display_lock(0)) {
    return ESP_FAIL;
  }

  s_screen = screen_active();
  lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

  // Lyric Label (Centered on screen like simulator)
  s_lyric_label = lv_label_create(s_screen);
  lv_obj_set_width(s_lyric_label, 260); // Wide area for lyrics
  lv_obj_set_style_text_color(s_lyric_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(s_lyric_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(s_lyric_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_lyric_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(s_lyric_label, "READY");
  lv_obj_align(s_lyric_label, LV_ALIGN_CENTER, 0, 0);

  s_left_bar = lv_obj_create(s_screen);
  lv_obj_remove_style_all(s_left_bar);
  lv_obj_set_style_radius(s_left_bar, 6, 0);
  lv_obj_set_style_bg_opa(s_left_bar, LV_OPA_COVER, 0);
  lv_obj_set_size(s_left_bar, 20, 8);
  lv_obj_align(s_left_bar, LV_ALIGN_LEFT_MID, 16, 0);

  s_right_bar = lv_obj_create(s_screen);
  lv_obj_remove_style_all(s_right_bar);
  lv_obj_set_style_radius(s_right_bar, 6, 0);
  lv_obj_set_style_bg_opa(s_right_bar, LV_OPA_COVER, 0);
  lv_obj_set_size(s_right_bar, 20, 8);
  lv_obj_align(s_right_bar, LV_ALIGN_RIGHT_MID, -16, 0);

  s_anim_timer = lv_timer_create(anim_timer_cb, 50, NULL);
  s_initialized = true;
  bsp_display_unlock();

  ESP_LOGI(TAG, "screen UI initialized (Ultra-minimalist)");
  return ESP_OK;
}

void screen_ui_deinit(void) {
  if (!s_initialized) {
    return;
  }
  if (!bsp_display_lock(0)) {
    return;
  }

  if (s_anim_timer != NULL) {
    lv_timer_del(s_anim_timer);
    s_anim_timer = NULL;
  }
  delete_obj(&s_left_bar);
  delete_obj(&s_right_bar);
  delete_obj(&s_lyric_label);

  s_screen = NULL;
  s_initialized = false;
  bsp_display_unlock();
}

void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming) {
  s_state = state;
  s_wifi_connected = wifi_connected;
  s_airplay_ready = airplay_ready;
  s_streaming = streaming;

  if (!s_initialized) {
    return;
  }
  if (!bsp_display_lock(0)) {
    return;
  }
  bsp_display_unlock();
}

void screen_ui_set_metadata(const screen_ui_metadata_t *metadata) {
  if (metadata == NULL) {
    return;
  }

  memcpy(&s_metadata, metadata, sizeof(s_metadata));
  s_metadata.title[SCREEN_UI_TEXT_MAX - 1] = '\0';
  s_metadata.artist[SCREEN_UI_TEXT_MAX - 1] = '\0';

  if (!s_initialized) {
    return;
  }
  if (!bsp_display_lock(0)) {
    return;
  }
  bsp_display_unlock();
}
