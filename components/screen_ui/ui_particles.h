#pragma once
#include "ui_renderer.h"

#define PARTICLE_COUNT 40
#define PARTICLE_FIELDS 7

typedef struct {
    float data[PARTICLE_COUNT * PARTICLE_FIELDS];
    struct {
        float maxSpeed;
        float maxSize;
        float minSize;
        float maxAlpha;
        float minAlpha;
        float pulseSpeed;
    } config;
} ui_particles_t;

void ui_particles_init(ui_particles_t *ps);
void ui_particles_update(ui_particles_t *ps, float delta_time);
void ui_particles_render(ui_particles_t *ps, ui_renderer_t *r, uint16_t hue);
