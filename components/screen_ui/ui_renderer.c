#include "ui_renderer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ui_renderer";

#define CANVAS_BUF_SIZE (LV_DRAW_BUF_SIZE(UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT, LV_COLOR_FORMAT_RGB565))

// Convert lv_color_t (RGB888) to RGB565 uint16_t
static inline uint16_t color_to_u16(lv_color_t color) {
    return ((color.red & 0xF8) << 8) | ((color.green & 0xFC) << 3) | ((color.blue & 0xF8) >> 3);
}

// Direct pixel set with alpha blending
static inline void set_pixel(uint8_t *buf, int x, int y, uint16_t color16, lv_opa_t opa) {
    if (x < 0 || x >= UI_SCREEN_WIDTH || y < 0 || y >= UI_SCREEN_HEIGHT) return;
    if (opa == LV_OPA_TRANSP) return;

    uint32_t offset = (y * UI_SCREEN_WIDTH + x) * 2;
    uint16_t *pixel = (uint16_t *)(buf + offset);

    if (opa == LV_OPA_COVER) {
        *pixel = color16;
    } else {
        uint16_t bg = *pixel;
        uint8_t bg_r = (bg >> 11) & 0x1F;
        uint8_t bg_g = (bg >> 5) & 0x3F;
        uint8_t bg_b = bg & 0x1F;

        uint8_t fg_r = (color16 >> 11) & 0x1F;
        uint8_t fg_g = (color16 >> 5) & 0x3F;
        uint8_t fg_b = color16 & 0x1F;

        uint8_t inv_a = 255 - opa;
        uint8_t r = (fg_r * opa + bg_r * inv_a) / 255;
        uint8_t g = (fg_g * opa + bg_g * inv_a) / 255;
        uint8_t b = (fg_b * opa + bg_b * inv_a) / 255;

        *pixel = (r << 11) | (g << 5) | b;
    }
}

// Bresenham line algorithm
static void draw_line_pixels(uint8_t *buf, int x0, int y0, int x1, int y1, uint16_t color16, lv_opa_t opa) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        set_pixel(buf, x0, y0, color16, opa);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

esp_err_t ui_renderer_init(ui_renderer_t *r, lv_obj_t *parent) {
    if (!r || !parent) return ESP_ERR_INVALID_ARG;
    memset(r, 0, sizeof(*r));

    r->buf_size = CANVAS_BUF_SIZE;

    r->buf = heap_caps_malloc(r->buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->buf) {
        r->buf = heap_caps_malloc(r->buf_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!r->buf) {
            ESP_LOGE(TAG, "Failed to allocate canvas buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    memset(r->buf, 0, r->buf_size);

    r->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(r->canvas, r->buf, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(r->canvas, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(r->canvas, 0, 0);

    ESP_LOGI(TAG, "Renderer initialized: %dx%d RGB565", UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    return ESP_OK;
}

void ui_renderer_deinit(ui_renderer_t *r) {
    if (!r) return;
    if (r->canvas) {
        lv_obj_delete(r->canvas);
        r->canvas = NULL;
    }
    if (r->buf) {
        heap_caps_free(r->buf);
        r->buf = NULL;
    }
    memset(r, 0, sizeof(*r));
}

void ui_renderer_clear(ui_renderer_t *r, lv_color_t color) {
    if (!r || !r->buf) return;
    uint16_t c = color_to_u16(color);
    uint16_t *pixels = (uint16_t *)r->buf;
    size_t count = UI_SCREEN_WIDTH * UI_SCREEN_HEIGHT;
    for (size_t i = 0; i < count; i++) {
        pixels[i] = c;
    }
}

void ui_renderer_draw_pixel(ui_renderer_t *r, int x, int y, lv_color_t color, lv_opa_t opa) {
    if (!r || !r->buf) return;
    set_pixel(r->buf, x, y, color_to_u16(color), opa);
}

void ui_renderer_draw_rect(ui_renderer_t *r, int x, int y, int w, int h, lv_color_t color, lv_opa_t opa, bool filled) {
    if (!r || !r->buf || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_SCREEN_WIDTH) w = UI_SCREEN_WIDTH - x;
    if (y + h > UI_SCREEN_HEIGHT) h = UI_SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    uint16_t c = color_to_u16(color);

    if (filled) {
        if (opa == LV_OPA_COVER) {
            for (int row = y; row < y + h; row++) {
                uint16_t *row_ptr = (uint16_t *)(r->buf + (row * UI_SCREEN_WIDTH + x) * 2);
                for (int col = 0; col < w; col++) {
                    row_ptr[col] = c;
                }
            }
        } else if (opa > LV_OPA_TRANSP) {
            for (int row = y; row < y + h; row++) {
                for (int col = x; col < x + w; col++) {
                    set_pixel(r->buf, col, row, c, opa);
                }
            }
        }
    } else {
        draw_line_pixels(r->buf, x, y, x + w - 1, y, c, opa);
        draw_line_pixels(r->buf, x, y + h - 1, x + w - 1, y + h - 1, c, opa);
        draw_line_pixels(r->buf, x, y, x, y + h - 1, c, opa);
        draw_line_pixels(r->buf, x + w - 1, y, x + w - 1, y + h - 1, c, opa);
    }
}

void ui_renderer_draw_circle(ui_renderer_t *r, int x, int y, int radius, lv_color_t color, lv_opa_t opa) {
    if (!r || !r->buf || radius <= 0) return;

    uint16_t c = color_to_u16(color);
    int x0 = 0, y0 = radius;
    int d = 3 - 2 * radius;

    while (y0 >= x0) {
        draw_line_pixels(r->buf, x - x0, y + y0, x + x0, y + y0, c, opa);
        draw_line_pixels(r->buf, x - x0, y - y0, x + x0, y - y0, c, opa);
        draw_line_pixels(r->buf, x - y0, y + x0, x + y0, y + x0, c, opa);
        draw_line_pixels(r->buf, x - y0, y - x0, x + y0, y - x0, c, opa);

        if (d < 0) {
            d = d + 4 * x0 + 6;
        } else {
            d = d + 4 * (x0 - y0) + 10;
            y0--;
        }
        x0++;
    }
}

void ui_renderer_draw_line(ui_renderer_t *r, int x1, int y1, int x2, int y2, lv_color_t color, lv_opa_t opa, int width) {
    if (!r || !r->buf || width <= 0) return;

    uint16_t c = color_to_u16(color);

    if (width == 1) {
        draw_line_pixels(r->buf, x1, y1, x2, y2, c, opa);
    } else {
        int half_w = width / 2;
        for (int dy = -half_w; dy <= half_w; dy++) {
            for (int dx = -half_w; dx <= half_w; dx++) {
                draw_line_pixels(r->buf, x1 + dx, y1 + dy, x2 + dx, y2 + dy, c, opa);
            }
        }
    }
}

void ui_renderer_draw_text(ui_renderer_t *r, const char *text, int x, int y, const lv_font_t *font, lv_color_t color, lv_opa_t opa, lv_text_align_t align) {
    if (!r || !r->canvas || !text || !font) return;

    lv_layer_t layer;
    lv_canvas_init_layer(r->canvas, &layer);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = color;
    dsc.opa = opa;
    dsc.text_local = 1;
    dsc.text = text;
    dsc.align = align;
    dsc.text_length = 0;
    dsc.line_space = 0;
    dsc.letter_space = 0;
    dsc.ofs_x = 0;
    dsc.ofs_y = 0;
    dsc.rotation = 0;
    dsc.sel_start = LV_DRAW_LABEL_NO_TXT_SEL;
    dsc.sel_end = LV_DRAW_LABEL_NO_TXT_SEL;
    dsc.flag = LV_TEXT_FLAG_NONE;
    dsc.hint = NULL;
    dsc.bidi_dir = LV_BASE_DIR_LTR;

    lv_area_t coords;
    switch (align) {
        case LV_TEXT_ALIGN_CENTER:
            coords.x1 = 0;
            coords.x2 = UI_SCREEN_WIDTH - 1;
            break;
        case LV_TEXT_ALIGN_RIGHT:
            coords.x1 = 0;
            coords.x2 = x;
            break;
        default:
            coords.x1 = x;
            coords.x2 = UI_SCREEN_WIDTH - 1;
            break;
    }
    coords.y1 = y;
    coords.y2 = UI_SCREEN_HEIGHT - 1;

    lv_draw_label(&layer, &dsc, &coords);
    lv_canvas_finish_layer(r->canvas, &layer);
}

void ui_renderer_draw_gradient_rect(ui_renderer_t *r, int x, int y, int w, int h, lv_color_t color_start, lv_color_t color_end, lv_opa_t opa_start, lv_opa_t opa_end, bool vertical) {
    if (!r || !r->buf || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_SCREEN_WIDTH) w = UI_SCREEN_WIDTH - x;
    if (y + h > UI_SCREEN_HEIGHT) h = UI_SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    uint16_t start_c = color_to_u16(color_start);
    uint16_t end_c = color_to_u16(color_end);

    uint8_t start_r = (start_c >> 11) & 0x1F;
    uint8_t start_g = (start_c >> 5) & 0x3F;
    uint8_t start_b = start_c & 0x1F;
    uint8_t end_r = (end_c >> 11) & 0x1F;
    uint8_t end_g = (end_c >> 5) & 0x3F;
    uint8_t end_b = end_c & 0x1F;

    if (vertical) {
        for (int row = 0; row < h; row++) {
            float t = (h > 1) ? (float)row / (h - 1) : 0;
            uint8_t rc = start_r + (end_r - start_r) * t;
            uint8_t gc = start_g + (end_g - start_g) * t;
            uint8_t bc = start_b + (end_b - start_b) * t;
            uint16_t c = (rc << 11) | (gc << 5) | bc;

            uint8_t opa = opa_start + (opa_end - opa_start) * t;

            for (int col = x; col < x + w; col++) {
                set_pixel(r->buf, col, y + row, c, opa);
            }
        }
    } else {
        for (int col = 0; col < w; col++) {
            float t = (w > 1) ? (float)col / (w - 1) : 0;
            uint8_t rc = start_r + (end_r - start_r) * t;
            uint8_t gc = start_g + (end_g - start_g) * t;
            uint8_t bc = start_b + (end_b - start_b) * t;
            uint16_t c = (rc << 11) | (gc << 5) | bc;

            uint8_t opa = opa_start + (opa_end - opa_start) * t;

            for (int row = y; row < y + h; row++) {
                set_pixel(r->buf, col + x, row, c, opa);
            }
        }
    }
}
