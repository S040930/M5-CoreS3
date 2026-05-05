#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL animation periods: CONFIG_SCREEN_UI_ANIM_* in sdkconfig (Kconfig / config.toml). */
#define SCREEN_UI_LYRIC_FONT_PRIMARY (&lv_font_source_han_sans_sc_16_cjk)
#define SCREEN_UI_LYRIC_FONT_FALLBACK (&lv_font_montserrat_14)

#define SCREEN_UI_HUE_BASE 190.0f
#define SCREEN_UI_HUE_SWING 60.0f
#define SCREEN_UI_HUE_SPEED 0.4f

static inline lv_color_t screen_ui_theme_bg_color(void) { return lv_color_hex(0x000000); }

#ifdef __cplusplus
}
#endif
