#pragma once

#include "lvgl.h"
#include "esp_err.h"

#define UI_SCREEN_WIDTH   320
#define UI_SCREEN_HEIGHT  240
#define UI_CENTER_X       160
#define UI_CENTER_Y       120

typedef struct {
    lv_obj_t *canvas;
    lv_draw_buf_t draw_buf;
    lv_layer_t layer;
    uint8_t *buf;
    size_t buf_size;
} ui_renderer_t;

esp_err_t ui_renderer_init(ui_renderer_t *r, lv_obj_t *parent);
void ui_renderer_deinit(ui_renderer_t *r);
void ui_renderer_clear(ui_renderer_t *r, lv_color_t color);
void ui_renderer_draw_pixel(ui_renderer_t *r, int x, int y, lv_color_t color, lv_opa_t opa);
void ui_renderer_draw_rect(ui_renderer_t *r, int x, int y, int w, int h, lv_color_t color, lv_opa_t opa, bool filled);
void ui_renderer_draw_circle(ui_renderer_t *r, int x, int y, int radius, lv_color_t color, lv_opa_t opa);
void ui_renderer_draw_line(ui_renderer_t *r, int x1, int y1, int x2, int y2, lv_color_t color, lv_opa_t opa, int width);
void ui_renderer_draw_text(ui_renderer_t *r, const char *text, int x, int y, const lv_font_t *font, lv_color_t color, lv_opa_t opa, lv_text_align_t align);
void ui_renderer_draw_gradient_rect(ui_renderer_t *r, int x, int y, int w, int h, lv_color_t color_start, lv_color_t color_end, lv_opa_t opa_start, lv_opa_t opa_end, bool vertical);
