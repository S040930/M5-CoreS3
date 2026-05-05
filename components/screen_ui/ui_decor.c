#include "ui_decor.h"
#include <math.h>
#include <string.h>

#define DECOR_MARGIN 12
#define DECOR_BRACKET_LEN 18
#define DECOR_DOT_COUNT 5
#define DECOR_DOT_SPACING 10
#define DECOR_DOT_SIZE 1.5f

void ui_decor_init(ui_decor_t *decor) {
    if (!decor) return;
    memset(decor, 0, sizeof(*decor));
}

void ui_decor_render(ui_decor_t *decor, ui_renderer_t *r, uint16_t hue, float now_sec) {
    if (!decor || !r) return;

    lv_color_t bracket_color = lv_color_hsv_to_rgb(hue, 60, 40);
    lv_opa_t bracket_opa = (lv_opa_t)(0.15f * 255);

    int m = DECOR_MARGIN;
    int l = DECOR_BRACKET_LEN;
    int w = UI_SCREEN_WIDTH;
    int h = UI_SCREEN_HEIGHT;

    ui_renderer_draw_line(r, m + l, m, m, m, bracket_color, bracket_opa, 1);
    ui_renderer_draw_line(r, m, m, m, m + l, bracket_color, bracket_opa, 1);

    ui_renderer_draw_line(r, w - m - l, m, w - m, m, bracket_color, bracket_opa, 1);
    ui_renderer_draw_line(r, w - m, m, w - m, m + l, bracket_color, bracket_opa, 1);

    ui_renderer_draw_line(r, m + l, h - m, m, h - m, bracket_color, bracket_opa, 1);
    ui_renderer_draw_line(r, m, h - m, m, h - m - l, bracket_color, bracket_opa, 1);

    ui_renderer_draw_line(r, w - m - l, h - m, w - m, h - m, bracket_color, bracket_opa, 1);
    ui_renderer_draw_line(r, w - m, h - m, w - m, h - m - l, bracket_color, bracket_opa, 1);

    (void)now_sec;

    lv_color_t scan_color = lv_color_hex(0x000000);
    for (int y_line = 0; y_line < UI_SCREEN_HEIGHT; y_line += 3) {
        ui_renderer_draw_rect(r, 0, y_line, UI_SCREEN_WIDTH, 1, scan_color, LV_OPA_10, true);
    }
}
