#pragma once
#include "ui_renderer.h"

typedef struct {
    int dummy;
} ui_decor_t;

void ui_decor_init(ui_decor_t *decor);
void ui_decor_render(ui_decor_t *decor, ui_renderer_t *r, uint16_t hue, float now_sec);
