#include "ui_particles.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void ui_particles_init(ui_particles_t *ps) {
    if (!ps) return;
    memset(ps, 0, sizeof(*ps));
    ps->config.maxSpeed = 0.3f;
    ps->config.maxSize = 2.0f;
    ps->config.minSize = 0.5f;
    ps->config.maxAlpha = 0.4f;
    ps->config.minAlpha = 0.1f;
    ps->config.pulseSpeed = 0.02f;

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        int off = i * PARTICLE_FIELDS;
        ps->data[off + 0] = (float)(rand() % UI_SCREEN_WIDTH);
        ps->data[off + 1] = (float)(rand() % UI_SCREEN_HEIGHT);
        ps->data[off + 2] = ((float)rand() / RAND_MAX - 0.5f) * ps->config.maxSpeed;
        ps->data[off + 3] = ((float)rand() / RAND_MAX - 0.5f) * ps->config.maxSpeed;
        ps->data[off + 4] = ((float)rand() / RAND_MAX) * (ps->config.maxSize - ps->config.minSize) + ps->config.minSize;
        ps->data[off + 5] = ((float)rand() / RAND_MAX) * (ps->config.maxAlpha - ps->config.minAlpha) + ps->config.minAlpha;
        ps->data[off + 6] = ((float)rand() / RAND_MAX) * 3.14159f * 2;
    }
}

void ui_particles_update(ui_particles_t *ps, float delta_time) {
    if (!ps) return;
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        int off = i * PARTICLE_FIELDS;
        ps->data[off + 0] += ps->data[off + 2];
        ps->data[off + 1] += ps->data[off + 3];
        ps->data[off + 6] += ps->config.pulseSpeed;

        if (ps->data[off + 0] < 0) ps->data[off + 0] = UI_SCREEN_WIDTH;
        if (ps->data[off + 0] > UI_SCREEN_WIDTH) ps->data[off + 0] = 0;
        if (ps->data[off + 1] < 0) ps->data[off + 1] = UI_SCREEN_HEIGHT;
        if (ps->data[off + 1] > UI_SCREEN_HEIGHT) ps->data[off + 1] = 0;
    }
}

void ui_particles_render(ui_particles_t *ps, ui_renderer_t *r, uint16_t hue) {
    if (!ps || !r) return;
    uint16_t hue_bucket = (hue / 6) * 6;

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        int off = i * PARTICLE_FIELDS;
        float x = ps->data[off + 0];
        float y = ps->data[off + 1];
        float size = ps->data[off + 4];
        float alpha = ps->data[off + 5] * (0.6f + 0.4f * sinf(ps->data[off + 6]));

        lv_color_t color = lv_color_hsv_to_rgb(hue_bucket, 80, 60);
        int opa_i = (int)(alpha * 255.0f + 0.5f);
        if (opa_i > (int)LV_OPA_COVER) opa_i = (int)LV_OPA_COVER;
        if (opa_i < 0) opa_i = 0;
        lv_opa_t opa = (lv_opa_t)opa_i;

        ui_renderer_draw_circle(r, (int)x, (int)y, (int)(size + 0.5f), color, opa);
    }
}
