#include "screen_ui.h"

#include "bsp/m5stack_core_s3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "screen_ui_theme.h"
#include "ui_renderer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "screen_ui";

/* ========== Pixel Face geometry constants (320x240) ========== */
#define PF_EYE_SIZE        36
#define PF_EYE_GAP         76
#define PF_EYE_Y           80
#define PF_PUPIL_R         6
#define PF_MOUTH_W         16
#define PF_MOUTH_H_CLOSED  4
#define PF_MOUTH_H_OPEN    18
#define PF_MOUTH_Y         150
#define PF_DOT_SIZE        6
#define PF_BAR_COUNT       5
#define PF_BAR_W           4
#define PF_BAR_GAP         4
#define PF_BAR_MAX_H       28
#define PF_BREATH_AMP      2
#define PF_BLINK_INTERVAL  50  /* frames, ~5s at 100ms */
#define PF_BLINK_DUR       3   /* frames */

#ifndef CONFIG_SCREEN_UI_ANIM_INTERVAL_MS
#define CONFIG_SCREEN_UI_ANIM_INTERVAL_MS 500
#endif

#ifndef CONFIG_SCREEN_UI_ANIM_STRESS_INTERVAL_MS
#define CONFIG_SCREEN_UI_ANIM_STRESS_INTERVAL_MS 500
#endif

/* ========== Context ========== */
typedef struct {
  lv_obj_t *screen;
  lv_timer_t *anim_timer;
  bool initialized;
  bool streaming;
  bool playing;
  bool network_busy;
  screen_ui_state_t state;
  screen_ui_voice_state_t voice_state;
  void (*voice_ptt_callback)(void);

  /* Pixel renderer */
  ui_renderer_t renderer;
  lv_obj_t *face_container;

  /* Mic button */
  lv_obj_t *mic_btn;
  lv_obj_t *mic_label;

  /* Assistant reply caption (LVGL label above canvas; streaming text appended in voice_ctrl). */
  lv_obj_t *assistant_lbl;

  /* Animation state */
  uint32_t anim_phase;
  float cur_eye_open;
  float cur_mouth_open;
  float pupil_offset_x;
  float breath_y;
  lv_color_t eye_sclera_color;
  lv_color_t mouth_color;
  bool blinking;
  int blink_timer;
  bool render_dirty;
  screen_ui_voice_state_t last_voice_state;
  bool last_streaming;

} screen_ui_ctx_t;

static screen_ui_ctx_t s_ui = {0};

static bool s_wdt_subscribed = false;

static void screen_tap_event_cb(lv_event_t *event);
static void anim_timer_cb(lv_timer_t *timer);
static void render_pixel_face(void);
static void set_colors_for_state(screen_ui_voice_state_t state);
static void update_mic_button_state(screen_ui_voice_state_t voice_state);

/* ========== Pixel drawing helpers ========== */
static void draw_eye_block(ui_renderer_t *r, int cx, int cy, int size, float open, lv_color_t color) {
  int h = (int)(size * open);
  if (h < 2) h = 2;
  int x = cx - size / 2;
  int y = cy - h / 2;
  ui_renderer_draw_rect(r, x, y, size, h, color, LV_OPA_COVER, true);
}

static void draw_pupil_circle(ui_renderer_t *r, int cx, int cy, int radius, float eye_open,
                              float off_x, lv_color_t color) {
  if (eye_open < 0.15f) return;
  int max_off = (PF_EYE_SIZE / 2 - radius - 2);
  int px = cx + (int)off_x;
  if (px > cx + max_off) px = cx + max_off;
  if (px < cx - max_off) px = cx - max_off;
  ui_renderer_draw_circle(r, px, cy, radius, color, LV_OPA_COVER);
}

static void draw_mouth_block(ui_renderer_t *r, int cx, int cy, float open, lv_color_t color) {
  int w = PF_MOUTH_W;
  int h = PF_MOUTH_H_CLOSED + (int)((PF_MOUTH_H_OPEN - PF_MOUTH_H_CLOSED) * open);
  int x = cx - w / 2;
  int y = cy - h / 2;
  ui_renderer_draw_rect(r, x, y, w, h, color, LV_OPA_COVER, true);
}

static void draw_thinking_dots(ui_renderer_t *r, int cx, int cy, uint32_t phase, lv_color_t color) {
  for (int i = 0; i < 3; i++) {
    float p = (float)phase * 0.25f + i * 1.5f;
    int y_off = (int)(sinf(p) * 5.0f);
    int x = cx - PF_DOT_SIZE * 2 + i * (PF_DOT_SIZE + 4);
    int y = cy - PF_DOT_SIZE / 2 + y_off;
    ui_renderer_draw_rect(r, x, y, PF_DOT_SIZE, PF_DOT_SIZE, color, LV_OPA_COVER, true);
  }
}

static void draw_spectrum_bars(ui_renderer_t *r, int cx, int cy, uint32_t phase, lv_color_t color) {
  int group_w = PF_BAR_COUNT * (PF_BAR_W + PF_BAR_GAP) - PF_BAR_GAP;
  int start_x = cx - group_w / 2;
  for (int i = 0; i < PF_BAR_COUNT; i++) {
    float freq = 2.5f + i * 1.2f;
    float p = i * 0.9f;
    float now = (float)phase * 0.12f;
    float v = fabsf(sinf(now * freq + p) * 0.6f + sinf(now * freq * 1.7f + p) * 0.4f);
    int h = (int)(3 + v * (PF_BAR_MAX_H - 3));
    int x = start_x + i * (PF_BAR_W + PF_BAR_GAP);
    int y = cy - h / 2;
    ui_renderer_draw_rect(r, x, y, PF_BAR_W, h, color, LV_OPA_COVER, true);
  }
}

/* ========== Main render ========== */
static void render_pixel_face(void) {
  if (!s_ui.initialized) return;

  ui_renderer_t *r = &s_ui.renderer;
  ui_renderer_clear(r, lv_color_hex(0x000000));

  int breath = (int)s_ui.breath_y;
  int eye_y = PF_EYE_Y + breath;
  int mouth_y = PF_MOUTH_Y + breath;

  int left_cx = UI_CENTER_X - PF_EYE_GAP / 2;
  int right_cx = UI_CENTER_X + PF_EYE_GAP / 2;
  lv_color_t black = lv_color_hex(0x000000);

  /* Eyes */
  draw_eye_block(r, left_cx, eye_y, PF_EYE_SIZE, s_ui.cur_eye_open, s_ui.eye_sclera_color);
  draw_eye_block(r, right_cx, eye_y, PF_EYE_SIZE, s_ui.cur_eye_open, s_ui.eye_sclera_color);

  /* Pupils */
  draw_pupil_circle(r, left_cx, eye_y, PF_PUPIL_R, s_ui.cur_eye_open, s_ui.pupil_offset_x, black);
  draw_pupil_circle(r, right_cx, eye_y, PF_PUPIL_R, s_ui.cur_eye_open, s_ui.pupil_offset_x, black);

  /* Mouth / thinking / spectrum */
  switch (s_ui.voice_state) {
  case SCREEN_UI_VOICE_THINKING:
    draw_thinking_dots(r, UI_CENTER_X, mouth_y, s_ui.anim_phase, s_ui.mouth_color);
    break;
  case SCREEN_UI_VOICE_SPEAKING:
    draw_spectrum_bars(r, UI_CENTER_X, mouth_y, s_ui.anim_phase, s_ui.mouth_color);
    break;
  default:
    draw_mouth_block(r, UI_CENTER_X, mouth_y, s_ui.cur_mouth_open, s_ui.mouth_color);
    break;
  }

  /* Tiny AirPlay pixel indicator (top-right 4x4 dot) */
  if (s_ui.voice_state == SCREEN_UI_VOICE_OFF || s_ui.voice_state == SCREEN_UI_VOICE_STANDBY) {
    if (s_ui.state == SCREEN_UI_STATE_STREAMING ||
        s_ui.state == SCREEN_UI_STATE_DISCOVERABLE ||
        s_ui.state == SCREEN_UI_STATE_SESSION_ESTABLISHING) {
      ui_renderer_draw_rect(r, UI_SCREEN_WIDTH - 12, 8, 4, 4, lv_color_hex(0xFFFFFF), LV_OPA_COVER, true);
    }
  }

  /* NOTE: env sensor data display was removed. Users can query
     temperature/humidity/pressure via voice (the env_monitor module still
     runs and injects real-time readings into the AI system prompt). */
}

/* ========== Color helpers ========== */
static void set_colors_for_state(screen_ui_voice_state_t state) {
  switch (state) {
  case SCREEN_UI_VOICE_STANDBY:
  case SCREEN_UI_VOICE_CONNECTING:
    /* 绿色 → 就绪 */
    s_ui.eye_sclera_color = lv_color_hex(0x333333);
    s_ui.mouth_color = lv_color_hex(0x4CAF50);
    break;
  case SCREEN_UI_VOICE_LISTENING:
    /* 蓝色 → 录音 */
    s_ui.eye_sclera_color = lv_color_hex(0xFFFFFF);
    s_ui.mouth_color = lv_color_hex(0x2196F3);
    break;
  case SCREEN_UI_VOICE_SENDING:
  case SCREEN_UI_VOICE_THINKING:
    /* 黄色 → 上传 / 处理 */
    s_ui.eye_sclera_color = lv_color_hex(0xFFFFFF);
    s_ui.mouth_color = lv_color_hex(0xFFC107);
    break;
  case SCREEN_UI_VOICE_SPEAKING:
    /* 红色 → 播放音频 */
    s_ui.eye_sclera_color = lv_color_hex(0xFFFFFF);
    s_ui.mouth_color = lv_color_hex(0xF44336);
    break;
  default:
    break;
  }
}

/* ========== Animation timer ========== */
static void anim_timer_cb(lv_timer_t *timer) {
  (void)timer;
  if (!s_ui.initialized) return;

  TaskHandle_t lvgl_task = xTaskGetCurrentTaskHandle();
  if (!s_wdt_subscribed && lvgl_task != NULL) {
    esp_task_wdt_add(lvgl_task);
    s_wdt_subscribed = true;
    ESP_LOGI(TAG, "taskLVGL subscribed to TWDT (timeout=%us)", (unsigned)CONFIG_ESP_TASK_WDT_TIMEOUT_S);
  }

  s_ui.anim_phase++;

  /* NOTE: env sensor data polling was removed together with the display.
     The env_monitor module continues to run in the background and feeds
     sensor data into the voice AI prompt, so users can ask via voice. */

  esp_task_wdt_reset();

  bool state_changed = (s_ui.voice_state != s_ui.last_voice_state) ||
                       (s_ui.streaming != s_ui.last_streaming);
  s_ui.last_voice_state = s_ui.voice_state;
  s_ui.last_streaming = s_ui.streaming;

  if (s_ui.blinking) {
    s_ui.blink_timer--;
    if (s_ui.blink_timer <= 0) {
      s_ui.blinking = false;
      s_ui.render_dirty = true;
    }
  } else {
    int blink_chance = (s_ui.voice_state == SCREEN_UI_VOICE_SENDING) ? 4 : 1;
    if ((rand() & 0xFF) < blink_chance) {
      s_ui.blinking = true;
      s_ui.blink_timer = PF_BLINK_DUR;
      s_ui.render_dirty = true;
    }
  }

  bool needs_render = state_changed || s_ui.render_dirty || s_ui.blinking ||
                      (s_ui.voice_state == SCREEN_UI_VOICE_THINKING);

  if (!needs_render) {
    return;
  }

  int64_t start_us = esp_timer_get_time();

  /* Breathing float - simplified to every 4 frames */
  if ((s_ui.anim_phase & 3) == 0) {
    s_ui.breath_y = sinf((float)s_ui.anim_phase * 0.0125f) * PF_BREATH_AMP;
  }

  /* Determine target geometry from voice state */
  float target_eye = 1.0f;
  float target_mouth = 0.0f;
  s_ui.pupil_offset_x = 0.0f;

  switch (s_ui.voice_state) {
  case SCREEN_UI_VOICE_STANDBY:
    target_eye = 0.4f;
    target_mouth = 0.0f;
    break;
  case SCREEN_UI_VOICE_CONNECTING:
    target_eye = 1.0f;
    target_mouth = 0.0f;
    break;
  case SCREEN_UI_VOICE_LISTENING:
  case SCREEN_UI_VOICE_SENDING:
    target_eye = 1.0f;
    target_mouth = 0.0f;
    break;
  case SCREEN_UI_VOICE_THINKING:
    target_eye = 1.0f;
    target_mouth = 0.0f;
    if ((s_ui.anim_phase & 3) == 0) {
      s_ui.pupil_offset_x = sinf((float)s_ui.anim_phase * 0.04f) * 8.0f;
    }
    break;
  case SCREEN_UI_VOICE_SPEAKING:
    target_eye = 1.0f;
    target_mouth = 1.0f;
    break;
  case SCREEN_UI_VOICE_ERROR:
    target_eye = 0.3f;
    target_mouth = 0.0f;
    break;
  default:
    break;
  }

  if (s_ui.blinking) {
    target_eye = 0.05f;
  }

  /* Smooth interpolation */
  float lerp_t = 0.3f;
  s_ui.cur_eye_open += (target_eye - s_ui.cur_eye_open) * lerp_t;
  s_ui.cur_mouth_open += (target_mouth - s_ui.cur_mouth_open) * lerp_t;

  if (s_ui.cur_eye_open < 0.0f) s_ui.cur_eye_open = 0.0f;
  if (s_ui.cur_eye_open > 1.0f) s_ui.cur_eye_open = 1.0f;
  if (s_ui.cur_mouth_open < 0.0f) s_ui.cur_mouth_open = 0.0f;
  if (s_ui.cur_mouth_open > 1.0f) s_ui.cur_mouth_open = 1.0f;

  /* Render frame */
  render_pixel_face();
  s_ui.render_dirty = false;

  int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
  if (elapsed_ms > 50) {
    ESP_LOGW(TAG, "anim render slow: %lld ms", elapsed_ms);
  }
  /* Canvas draws directly into lv_canvas buffer; must always invalidate or the LCD may not refresh. */
  lv_obj_invalidate(s_ui.renderer.canvas);

  esp_task_wdt_reset();
}

/* ========== Mic button state update ========== */
static void update_mic_button_state(screen_ui_voice_state_t voice_state) {
  if (!s_ui.initialized || s_ui.mic_btn == NULL) return;

  switch (voice_state) {
    case SCREEN_UI_VOICE_STANDBY:
    case SCREEN_UI_VOICE_ERROR:
      lv_obj_set_style_bg_color(s_ui.mic_btn, lv_color_hex(0x4CAF50), 0);
      lv_obj_set_style_bg_opa(s_ui.mic_btn, LV_OPA_80, 0);
      lv_obj_remove_state(s_ui.mic_btn, LV_STATE_DISABLED);
      lv_obj_set_style_text_color(s_ui.mic_label, lv_color_hex(0xFFFFFF), 0);
      break;

    case SCREEN_UI_VOICE_CONNECTING:
    case SCREEN_UI_VOICE_LISTENING:
    case SCREEN_UI_VOICE_SENDING:
      lv_obj_set_style_bg_color(s_ui.mic_btn, lv_color_hex(0x2196F3), 0);
      lv_obj_set_style_bg_opa(s_ui.mic_btn, LV_OPA_100, 0);
      lv_obj_set_style_text_color(s_ui.mic_label, lv_color_hex(0xFFFFFF), 0);
      break;

    case SCREEN_UI_VOICE_THINKING:
    case SCREEN_UI_VOICE_SPEAKING:
      lv_obj_set_style_bg_color(s_ui.mic_btn, lv_color_hex(0xFF9800), 0);
      lv_obj_set_style_bg_opa(s_ui.mic_btn, LV_OPA_100, 0);
      lv_obj_set_style_text_color(s_ui.mic_label, lv_color_hex(0xFFFFFF), 0);
      break;

    default:
      break;
  }
}

/* ========== Tap callback ========== */
static void screen_tap_event_cb(lv_event_t *event) {
  (void)event;
  if (!s_ui.initialized) return;
  if (s_ui.voice_state == SCREEN_UI_VOICE_OFF) {
    return;
  }
  if (s_ui.voice_ptt_callback != NULL) {
    s_ui.voice_ptt_callback();
  }
}

/* ========== Init ========== */

esp_err_t screen_ui_init(void) {
  if (s_ui.initialized) return ESP_OK;

  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = BSP_LCD_H_RES * 10,
      .double_buffer = false,
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

  TaskHandle_t lvgl_task = xTaskGetHandle("taskLVGL");
  if (lvgl_task != NULL) {
    esp_err_t twdt_err = esp_task_wdt_add(lvgl_task);
    if (twdt_err == ESP_OK) {
      ESP_LOGI(TAG, "taskLVGL added to TWDT (prevents IDLE1 starvation)");
    } else {
      ESP_LOGW(TAG, "TWDT add failed: %s (TTL=%us)", esp_err_to_name(twdt_err),
               (unsigned)CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    }
  } else {
    ESP_LOGW(TAG, "taskLVGL not found, cannot add to TWDT");
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

  /* Pixel renderer canvas */
  if (ui_renderer_init(&s_ui.renderer, s_ui.screen) != ESP_OK) {
    ESP_LOGE(TAG, "renderer init failed");
    bsp_display_unlock();
    return ESP_FAIL;
  }

  /* Transparent click overlay for PTT */
  s_ui.face_container = lv_obj_create(s_ui.screen);
  lv_obj_set_size(s_ui.face_container, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
  lv_obj_set_pos(s_ui.face_container, 0, 0);
  lv_obj_set_style_bg_opa(s_ui.face_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_ui.face_container, 0, 0);
  lv_obj_set_style_pad_all(s_ui.face_container, 0, 0);
  lv_obj_remove_flag(s_ui.face_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_ui.face_container, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_ui.face_container, screen_tap_event_cb, LV_EVENT_CLICKED, NULL);

  /* Mic button - bottom center */
  s_ui.mic_btn = lv_button_create(s_ui.screen);
  lv_obj_set_size(s_ui.mic_btn, 80, 80);
  lv_obj_set_pos(s_ui.mic_btn, (UI_SCREEN_WIDTH - 80) / 2, UI_SCREEN_HEIGHT - 100);
  lv_obj_set_style_radius(s_ui.mic_btn, 40, 0);
  lv_obj_set_style_bg_color(s_ui.mic_btn, lv_color_hex(0x4CAF50), 0);
  lv_obj_set_style_bg_opa(s_ui.mic_btn, LV_OPA_80, 0);
  lv_obj_add_flag(s_ui.mic_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_ui.mic_btn, screen_tap_event_cb, LV_EVENT_CLICKED, NULL);

  s_ui.mic_label = lv_label_create(s_ui.mic_btn);
  lv_label_set_text(s_ui.mic_label, LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_font(s_ui.mic_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_ui.mic_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(s_ui.mic_label);

  s_ui.assistant_lbl = lv_label_create(s_ui.screen);
  lv_obj_set_width(s_ui.assistant_lbl, UI_SCREEN_WIDTH - 16);
  lv_label_set_long_mode(s_ui.assistant_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(s_ui.assistant_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_ui.assistant_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_pos(s_ui.assistant_lbl, 8, 108);
  lv_label_set_text(s_ui.assistant_lbl, "");
  lv_obj_add_flag(s_ui.assistant_lbl, LV_OBJ_FLAG_HIDDEN);

  /* Init animation state */
  s_ui.cur_eye_open = 1.0f;
  s_ui.cur_mouth_open = 0.0f;
  s_ui.pupil_offset_x = 0.0f;
  s_ui.breath_y = 0.0f;
  s_ui.anim_phase = 0;
  s_ui.blinking = false;
  s_ui.blink_timer = 0;
  s_ui.render_dirty = true;
  s_ui.last_voice_state = SCREEN_UI_VOICE_OFF;
  s_ui.last_streaming = false;
  set_colors_for_state(SCREEN_UI_VOICE_STANDBY);
  update_mic_button_state(SCREEN_UI_VOICE_STANDBY);

  s_ui.initialized = true;
  s_ui.state = SCREEN_UI_STATE_BOOT;
  s_ui.voice_state = SCREEN_UI_VOICE_OFF;
  s_ui.streaming = false;
  s_ui.playing = false;
  s_ui.network_busy = false;

  s_ui.anim_timer = lv_timer_create(anim_timer_cb,
                                      (uint32_t)CONFIG_SCREEN_UI_ANIM_INTERVAL_MS, NULL);
  if (s_ui.anim_timer) {
    lv_timer_ready(s_ui.anim_timer);
  }

  bsp_display_unlock();

  ESP_LOGI(TAG, "screen UI initialized (Pixel Buddy Face)");
  return ESP_OK;
}

/* ========== Public API ========== */
void screen_ui_set_voice_ptt_callback(void (*callback)(void)) {
  s_ui.voice_ptt_callback = callback;
}

void screen_ui_set_voice_network_busy(bool busy) {
  if (!s_ui.initialized) return;

  if (s_ui.anim_timer == NULL) return;
  if (s_ui.network_busy == busy) return;
  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;

  uint32_t target_ms = busy ? (uint32_t)CONFIG_SCREEN_UI_ANIM_STRESS_INTERVAL_MS
                            : (uint32_t)CONFIG_SCREEN_UI_ANIM_INTERVAL_MS;
  if (target_ms < 8U) {
    target_ms = 8U;
  }

  lv_timer_set_period(s_ui.anim_timer, target_ms);
  s_ui.network_busy = busy;
  ESP_LOGI(TAG, "ui anim period -> %lu ms (network_busy=%d)",
           (unsigned long)target_ms, (int)busy);

  bsp_display_unlock();
}

void screen_ui_deinit(void) {
  if (!s_ui.initialized) return;
  if (!bsp_display_lock(0)) return;

  if (s_ui.anim_timer) {
    lv_timer_delete(s_ui.anim_timer);
    s_ui.anim_timer = NULL;
  }

  ui_renderer_deinit(&s_ui.renderer);

  if (s_ui.screen) {
    lv_obj_clean(s_ui.screen);
  }

  memset(&s_ui, 0, sizeof(s_ui));
  bsp_display_unlock();
}

void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming) {
  (void)wifi_connected;
  (void)airplay_ready;

  s_ui.state = state;
  if (s_ui.streaming != streaming) {
    s_ui.streaming = streaming;
    s_ui.render_dirty = true;
  }

  if (!s_ui.initialized) return;
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

  /* Map OFF to STANDBY so face is always visible */
  if (state == SCREEN_UI_VOICE_OFF) {
    state = SCREEN_UI_VOICE_STANDBY;
  }

  if (s_ui.voice_state != state) {
    s_ui.voice_state = state;
    s_ui.render_dirty = true;
  }

  if (!s_ui.initialized) return;
  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;

  set_colors_for_state(state);
  update_mic_button_state(state);

  if (s_ui.assistant_lbl != NULL) {
    if (error_text != NULL && error_text[0] != '\0') {
      lv_label_set_text(s_ui.assistant_lbl, error_text);
      lv_obj_remove_flag(s_ui.assistant_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (assistant_text != NULL && assistant_text[0] != '\0') {
      lv_label_set_text(s_ui.assistant_lbl, assistant_text);
      lv_obj_remove_flag(s_ui.assistant_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (assistant_text != NULL && assistant_text[0] == '\0') {
      lv_label_set_text(s_ui.assistant_lbl, "");
      lv_obj_add_flag(s_ui.assistant_lbl, LV_OBJ_FLAG_HIDDEN);
    } else if (assistant_text == NULL &&
               (state == SCREEN_UI_VOICE_STANDBY || state == SCREEN_UI_VOICE_LISTENING ||
                state == SCREEN_UI_VOICE_CONNECTING)) {
      lv_label_set_text(s_ui.assistant_lbl, "");
      lv_obj_add_flag(s_ui.assistant_lbl, LV_OBJ_FLAG_HIDDEN);
    }
  }

  bsp_display_unlock();
}
