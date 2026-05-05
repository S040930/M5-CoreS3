#pragma once
#include "ui_renderer.h"

#define LYRIC_MAX_LINES 8
#define LYRIC_TEXT_MAX 64
#define LYRIC_SWITCH_INTERVAL_MS 3500
#define LYRIC_TRANSITION_MS 600

typedef struct {
    char lines[LYRIC_MAX_LINES][LYRIC_TEXT_MAX];
    int line_count;
    int current_index;
    uint32_t last_switch_ms;
    const lv_font_t *font;
} ui_lyric_t;

void ui_lyric_init(ui_lyric_t *lyric, const lv_font_t *font);
void ui_lyric_set_text(ui_lyric_t *lyric, const char *text);
void ui_lyric_set_lines(ui_lyric_t *lyric, const char *lines[], int count);
void ui_lyric_update(ui_lyric_t *lyric, uint32_t now_ms);
void ui_lyric_render(ui_lyric_t *lyric, ui_renderer_t *r, uint16_t hue, uint32_t now_ms);
