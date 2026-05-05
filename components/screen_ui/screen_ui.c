#include "screen_ui.h"

#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "screen_ui_theme.h"

static const char *TAG = "screen_ui";

typedef struct {
  lv_obj_t *screen;
  lv_obj_t *airplay_label;
  lv_obj_t *omni_label;
  lv_timer_t *anim_timer;
  bool initialized;
  bool streaming;
  bool playing;
  screen_ui_state_t state;
  screen_ui_voice_state_t voice_state;
  void (*voice_ptt_callback)(void);
} screen_ui_ctx_t;

static screen_ui_ctx_t s_ui = {0};

static void screen_ui_update_labels(void);
static void screen_tap_event_cb(lv_event_t *event);

#ifndef CONFIG_SCREEN_UI_ANIM_INTERVAL_MS
#define CONFIG_SCREEN_UI_ANIM_INTERVAL_MS 100
#endif

static void anim_timer_cb(lv_timer_t *timer) {
  (void)timer;
  if (!s_ui.initialized) return;
  screen_ui_update_labels();
}

static void screen_ui_update_labels(void) {
  if (!s_ui.initialized) return;

  bool show_omni = (s_ui.voice_state != SCREEN_UI_VOICE_OFF);
  bool show_airplay = !show_omni &&
      (s_ui.state == SCREEN_UI_STATE_STREAMING ||
       s_ui.state == SCREEN_UI_STATE_DISCOVERABLE ||
       s_ui.state == SCREEN_UI_STATE_SESSION_ESTABLISHING);

  if (show_airplay) {
    lv_obj_remove_flag(s_ui.airplay_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_ui.airplay_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (show_omni) {
    lv_obj_remove_flag(s_ui.omni_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_ui.omni_label, LV_OBJ_FLAG_HIDDEN);
  }
}

static void screen_tap_event_cb(lv_event_t *event) {
  (void)event;
  if (!s_ui.initialized || s_ui.voice_state == SCREEN_UI_VOICE_OFF) {
    return;
  }
  if (s_ui.voice_state == SCREEN_UI_VOICE_STANDBY) {
    return;
  }
  if (s_ui.voice_ptt_callback != NULL) {
    s_ui.voice_ptt_callback();
  }
}

esp_err_t screen_ui_init(void) {
  if (s_ui.initialized) return ESP_OK;

  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = BSP_LCD_H_RES * 20,
      .double_buffer = true,
      .flags = {
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

  esp_err_t err = bsp_display_backlight_on();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "backlight on failed: %s", esp_err_to_name(err));
  }

  if (!bsp_display_lock(0)) {
    return ESP_FAIL;
  }

  s_ui.screen = lv_screen_active();
  lv_obj_set_style_bg_color(s_ui.screen, screen_ui_theme_bg_color(), 0);
  lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_ui.screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_ui.screen, screen_tap_event_cb, LV_EVENT_CLICKED, NULL);

  static lv_style_t airplay_style;
  lv_style_init(&airplay_style);
  lv_style_set_text_color(&airplay_style, lv_color_hex(0xE8F4FD));
  lv_style_set_text_font(&airplay_style, &lv_font_montserrat_14);
  lv_style_set_text_align(&airplay_style, LV_TEXT_ALIGN_CENTER);
  lv_style_set_bg_opa(&airplay_style, LV_OPA_TRANSP);

  s_ui.airplay_label = lv_label_create(s_ui.screen);
  lv_obj_add_style(s_ui.airplay_label, &airplay_style, 0);
  lv_label_set_text(s_ui.airplay_label, "AirPlay");
  lv_obj_align(s_ui.airplay_label, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_add_flag(s_ui.airplay_label, LV_OBJ_FLAG_HIDDEN);

  static lv_style_t omni_style;
  lv_style_init(&omni_style);
  lv_style_set_text_color(&omni_style, lv_color_hex(0x00D4AA));
  lv_style_set_text_font(&omni_style, &lv_font_montserrat_14);
  lv_style_set_text_align(&omni_style, LV_TEXT_ALIGN_CENTER);
  lv_style_set_bg_opa(&omni_style, LV_OPA_TRANSP);

  s_ui.omni_label = lv_label_create(s_ui.screen);
  lv_obj_add_style(s_ui.omni_label, &omni_style, 0);
  lv_label_set_text(s_ui.omni_label, "omni");
  lv_obj_align(s_ui.omni_label, LV_ALIGN_BOTTOM_MID, 0, -24);
  lv_obj_add_flag(s_ui.omni_label, LV_OBJ_FLAG_HIDDEN);

  s_ui.initialized = true;
  s_ui.state = SCREEN_UI_STATE_BOOT;
  s_ui.voice_state = SCREEN_UI_VOICE_OFF;
  s_ui.streaming = false;
  s_ui.playing = false;

  s_ui.anim_timer = lv_timer_create(anim_timer_cb,
                                      (uint32_t)CONFIG_SCREEN_UI_ANIM_INTERVAL_MS, NULL);
  if (s_ui.anim_timer) {
    lv_timer_ready(s_ui.anim_timer);
  }

  bsp_display_unlock();

  ESP_LOGI(TAG, "screen UI initialized (AirPlay/omni labels)");
  return ESP_OK;
}

void screen_ui_set_voice_ptt_callback(void (*callback)(void)) {
  s_ui.voice_ptt_callback = callback;
}

void screen_ui_set_voice_network_busy(bool busy) {
  (void)busy;
  if (!s_ui.initialized) return;
}

void screen_ui_deinit(void) {
  if (!s_ui.initialized) return;
  if (!bsp_display_lock(0)) return;

  if (s_ui.anim_timer) {
    lv_timer_delete(s_ui.anim_timer);
    s_ui.anim_timer = NULL;
  }

  if (s_ui.screen) {
    lv_obj_clean(s_ui.screen);
  }

  memset(&s_ui, 0, sizeof(s_ui));
  bsp_display_unlock();
}

void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming) {
  (void)wifi_connected;

  s_ui.state = state;
  s_ui.streaming = streaming;

  if (!s_ui.initialized) return;

  if (s_ui.voice_state != SCREEN_UI_VOICE_OFF) {
    return;
  }

  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;
  screen_ui_update_labels();
  bsp_display_unlock();
}

void screen_ui_set_metadata(const screen_ui_metadata_t *metadata) {
  (void)metadata;
  if (!s_ui.initialized) return;
}

void screen_ui_set_playing(bool playing) {
  s_ui.playing = playing;
  if (!s_ui.initialized) return;
}

void screen_ui_set_voice_state(screen_ui_voice_state_t state, const char *user_text,
                               const char *assistant_text, const char *error_text) {
  (void)user_text;
  (void)assistant_text;
  (void)error_text;

  s_ui.voice_state = state;

  if (!s_ui.initialized) return;
  if (!s_ui.screen) return;
  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;
  screen_ui_update_labels();
  bsp_display_unlock();
}
