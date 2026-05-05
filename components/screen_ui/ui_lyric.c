#include "ui_lyric.h"
#include <math.h>
#include <string.h>

static float ease_out_expo(float x) {
    return x >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * x);
}

void ui_lyric_init(ui_lyric_t *lyric, const lv_font_t *font) {
    if (!lyric) return;
    memset(lyric, 0, sizeof(*lyric));
    lyric->font = font ? font : &lv_font_montserrat_14;
    lyric->line_count = 0;
    lyric->current_index = 0;

    strcpy(lyric->lines[0], "NEON LIGHTS IN THE RAIN");
    lyric->line_count = 1;
}

void ui_lyric_set_text(ui_lyric_t *lyric, const char *text) {
    if (!lyric || !text) return;
    strncpy(lyric->lines[0], text, LYRIC_TEXT_MAX - 1);
    lyric->lines[0][LYRIC_TEXT_MAX - 1] = '\0';
    lyric->line_count = 1;
    lyric->current_index = 0;
    lyric->last_switch_ms = 0;
}

void ui_lyric_set_lines(ui_lyric_t *lyric, const char *lines[], int count) {
    if (!lyric || !lines || count <= 0) return;
    lyric->line_count = count > LYRIC_MAX_LINES ? LYRIC_MAX_LINES : count;
    for (int i = 0; i < lyric->line_count; i++) {
        if (lines[i]) {
            strncpy(lyric->lines[i], lines[i], LYRIC_TEXT_MAX - 1);
            lyric->lines[i][LYRIC_TEXT_MAX - 1] = '\0';
        } else {
            lyric->lines[i][0] = '\0';
        }
    }
    lyric->current_index = 0;
    lyric->last_switch_ms = 0;
}

void ui_lyric_update(ui_lyric_t *lyric, uint32_t now_ms) {
    if (!lyric || lyric->line_count <= 1) return;

    if (now_ms - lyric->last_switch_ms >= LYRIC_SWITCH_INTERVAL_MS) {
        lyric->current_index = (lyric->current_index + 1) % lyric->line_count;
        lyric->last_switch_ms = now_ms;
    }
}

void ui_lyric_render(ui_lyric_t *lyric, ui_renderer_t *r, uint16_t hue, uint32_t now_ms) {
    if (!lyric || !r || lyric->line_count == 0) return;

    uint32_t cycle_time = now_ms - lyric->last_switch_ms;
    float opacity = 1.0f;
    float offset_y = 0;

    if (cycle_time < LYRIC_TRANSITION_MS) {
        float t = (float)cycle_time / LYRIC_TRANSITION_MS;
        opacity = ease_out_expo(t);
        offset_y = (1.0f - t) * 8.0f;
    } else if (cycle_time > LYRIC_SWITCH_INTERVAL_MS - LYRIC_TRANSITION_MS) {
        float t = (float)(LYRIC_SWITCH_INTERVAL_MS - cycle_time) / LYRIC_TRANSITION_MS;
        opacity = ease_out_expo(t);
        offset_y = -(1.0f - t) * 8.0f;
    }

    float breath = (sinf((float)now_ms / 1000.0f * 1.5f) * 0.5f + 0.5f);
    float final_opacity = (0.75f + breath * 0.25f) * opacity;

    const char *text = lyric->lines[lyric->current_index];
    int y = UI_CENTER_Y - 10 + (int)offset_y;

    lv_color_t glow_color = lv_color_hsv_to_rgb(hue, 100, 50);
    int glow_opa_i = (int)(0.3f * final_opacity * 255.0f + 0.5f);
    if (glow_opa_i > (int)LV_OPA_COVER) glow_opa_i = (int)LV_OPA_COVER;
    if (glow_opa_i < 0) glow_opa_i = 0;
    lv_opa_t glow_opa = (lv_opa_t)glow_opa_i;

    for (int dx = -1; dx <= 1; dx += 2) {
        for (int dy = -1; dy <= 1; dy += 2) {
            ui_renderer_draw_text(r, text, UI_CENTER_X + dx, y + dy,
                                  lyric->font, glow_color, glow_opa, LV_TEXT_ALIGN_CENTER);
        }
    }

    lv_color_t text_color = lv_color_hsv_to_rgb(0, 0, 100);
    int text_opa_i = (int)(final_opacity * 255.0f + 0.5f);
    if (text_opa_i > (int)LV_OPA_COVER) text_opa_i = (int)LV_OPA_COVER;
    if (text_opa_i < 0) text_opa_i = 0;
    lv_opa_t text_opa = (lv_opa_t)text_opa_i;

    ui_renderer_draw_text(r, text, UI_CENTER_X, y,
                          lyric->font, text_color, text_opa, LV_TEXT_ALIGN_CENTER);

    int line_width = (int)(60 * opacity);
    lv_color_t line_color = lv_color_hsv_to_rgb(hue, 100, 50);
    int line_opa_i = (int)(0.2f * opacity * 255.0f + 0.5f);
    if (line_opa_i > (int)LV_OPA_COVER) line_opa_i = (int)LV_OPA_COVER;
    if (line_opa_i < 0) line_opa_i = 0;
    lv_opa_t line_opa = (lv_opa_t)line_opa_i;
    ui_renderer_draw_line(r, UI_CENTER_X - line_width, UI_CENTER_Y + 18,
                          UI_CENTER_X + line_width, UI_CENTER_Y + 18,
                          line_color, line_opa, 1);
}
