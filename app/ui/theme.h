#ifndef UI_THEME_H
#define UI_THEME_H

#include <stdint.h>

typedef enum {
    UI_THEME_GRAY,
    UI_THEME_AQUA,
    UI_THEME_WINDOWS_95,
    UI_THEME_COUNT
} ui_theme_id_t;

/* Semantic colors shared by every page. Geometry stays square in all themes. */
typedef struct {
    ui_theme_id_t id;
    uint32_t menu_top_bg, menu_top_fg, menu_body_bg, menu_body_fg;
    uint32_t mini_bg, mini_fg, mini_muted, mini_dim;
    uint32_t bg, surface, panel, panel_alt, border, text, muted, dim;
    uint32_t accent, selection, focus, on_selection, outline, counter;
    uint32_t fast_scroll_band;
    uint32_t green, amber, red, purple, cyan;
    uint32_t context_panel, context_group, context_border, progress, scrim;
    uint32_t button_top, button_bottom, bevel_light, bevel_shadow;
    uint8_t scrim_opa;
} ui_theme_t;

extern const ui_theme_t ui_themes[UI_THEME_COUNT];
const ui_theme_t * ui_theme_get(int id);

#endif
