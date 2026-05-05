#include "ui_spectrum.h"
#include <math.h>
#include <string.h>

void ui_spectrum_init(ui_spectrum_t *sp) {
    if (!sp) return;
    memset(sp, 0, sizeof(*sp));
    sp->animation_speed = 0.15f;
}

void ui_spectrum_update(ui_spectrum_t *sp, float delta_time, float now_sec, bool is_playing) {
    if (!sp) return;
    sp->is_playing = is_playing;

    if (is_playing) {
        for (int i = 0; i < SPECTRUM_BAR_COUNT; i++) {
            float freq = 3.0f + i * 1.5f;
            float phase = i * 0.7f;
            float target = fabsf(sinf(now_sec * freq + phase) * 0.5f + sinf(now_sec * freq * 2.3f + phase) * 0.3f) + 0.05f;
            sp->bands[i] += (target - sp->bands[i]) * sp->animation_speed;
        }
    } else {
        for (int i = 0; i < SPECTRUM_BAR_COUNT; i++) {
            sp->bands[i] *= 0.92f;
        }
    }
}

void ui_spectrum_render(ui_spectrum_t *sp, ui_renderer_t *r, uint16_t hue) {
    if (!sp || !r) return;

    int group_width = SPECTRUM_BAR_COUNT * (SPECTRUM_BAR_WIDTH + SPECTRUM_BAR_GAP) - SPECTRUM_BAR_GAP;
    int left_start = UI_CENTER_X - SPECTRUM_BAR_OFFSET - group_width / 2;
    int right_start = UI_CENTER_X + SPECTRUM_BAR_OFFSET - group_width / 2;

    for (int i = 0; i < SPECTRUM_BAR_COUNT; i++) {
        float band = sp->bands[i];
        int bar_height = (int)(fmaxf(3.0f, SPECTRUM_MAX_HEIGHT * band));

        int x_left = left_start + i * (SPECTRUM_BAR_WIDTH + SPECTRUM_BAR_GAP);
        int x_right = right_start + i * (SPECTRUM_BAR_WIDTH + SPECTRUM_BAR_GAP);

        lv_color_t color_bottom = lv_color_hsv_to_rgb(hue, 100, 50);
        lv_color_t color_top = lv_color_hsv_to_rgb(hue, 100, 65);

        ui_renderer_draw_gradient_rect(r, x_left, SPECTRUM_BASE_Y - bar_height,
                                       SPECTRUM_BAR_WIDTH, bar_height,
                                       color_bottom, color_top,
                                       LV_OPA_10, LV_OPA_90, true);
        ui_renderer_draw_gradient_rect(r, x_right, SPECTRUM_BASE_Y - bar_height,
                                       SPECTRUM_BAR_WIDTH, bar_height,
                                       color_bottom, color_top,
                                       LV_OPA_10, LV_OPA_90, true);

        lv_color_t highlight = lv_color_hsv_to_rgb(hue, 100, 80);
        ui_renderer_draw_rect(r, x_left, SPECTRUM_BASE_Y - bar_height,
                              SPECTRUM_BAR_WIDTH, 1, highlight, LV_OPA_60, true);
        ui_renderer_draw_rect(r, x_right, SPECTRUM_BASE_Y - bar_height,
                              SPECTRUM_BAR_WIDTH, 1, highlight, LV_OPA_60, true);
    }
}
