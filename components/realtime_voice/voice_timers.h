#pragma once

#include <stdbool.h>
#include <stdint.h>

void voice_timers_init(void);
void voice_timers_deinit(void);

/** Start a one-shot relative timer. Returns public timer id (>0) or 0 on failure. */
uint32_t voice_timers_set(uint32_t duration_sec, const char *label);

/** Cancel a pending timer by id. Returns true if a timer was stopped. */
bool voice_timers_cancel(uint32_t timer_id);

int voice_timers_active_count(void);
