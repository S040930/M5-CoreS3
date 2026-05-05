#pragma once
#include "ui_renderer.h"

#define SPECTRUM_BAR_COUNT 6
#define SPECTRUM_BAR_WIDTH 6
#define SPECTRUM_BAR_GAP 2
#define SPECTRUM_MAX_HEIGHT 100
#define SPECTRUM_BASE_Y (UI_CENTER_Y + 75)
#define SPECTRUM_BAR_OFFSET 130

typedef struct {
    float bands[SPECTRUM_BAR_COUNT];
    float animation_speed;
    bool is_playing;
} ui_spectrum_t;

void ui_spectrum_init(ui_spectrum_t *sp);
void ui_spectrum_update(ui_spectrum_t *sp, float delta_time, float now_sec, bool is_playing);
void ui_spectrum_render(ui_spectrum_t *sp, ui_renderer_t *r, uint16_t hue);
