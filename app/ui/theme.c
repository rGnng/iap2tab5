#include "theme.h"

const ui_theme_t ui_themes[UI_THEME_COUNT] = {
    [UI_THEME_GRAY] = {
        .id = UI_THEME_GRAY,
        .menu_top_bg = 0x282828, .menu_top_fg = 0xFFFFFF,
        .menu_body_bg = 0x080808, .menu_body_fg = 0xC8C8C8,
        .mini_bg = 0x303030, .mini_fg = 0xFFFFFF,
        .mini_muted = 0xD0D0D0, .mini_dim = 0xA0A0A0,
        .bg = 0x080808, .surface = 0x101010,
        .panel = 0x101010, .panel_alt = 0x202020, .border = 0x282828,
        .text = 0xF0F0F0, .muted = 0x989898, .dim = 0x686868,
        .accent = 0xC8C8C8, .selection = 0x282828, .focus = 0x303030,
        .on_selection = 0xFFFFFF, .outline = 0xFFFFFF, .counter = 0x282828,
        .fast_scroll_band = 0xE6B66B,
        .green = 0xB8B8B8, .amber = 0xC8C8C8, .red = 0xE8E8E8,
        .purple = 0xA8A8A8, .cyan = 0xB8B8B8,
        .context_panel = 0x202020, .context_group = 0x181818,
        .context_border = 0x484848, .progress = 0x22303F, .scrim = 0x000000,
        .button_top = 0x303030, .button_bottom = 0x282828,
        .bevel_light = 0x484848, .bevel_shadow = 0x080808, .scrim_opa = 153,
    },
    [UI_THEME_AQUA] = {
        .id = UI_THEME_AQUA,
        .menu_top_bg = 0x23598F, .menu_top_fg = 0xFFFFFF,
        .menu_body_bg = 0xD6E0EC, .menu_body_fg = 0x294765,
        .mini_bg = 0xDCE5EF, .mini_fg = 0x172D45,
        .mini_muted = 0x40536B, .mini_dim = 0x51647A,
        .bg = 0xE8EDF3, .surface = 0xF7FAFD,
        .panel = 0xF4F7FB, .panel_alt = 0xE5ECF4, .border = 0xA1B3C8,
        .text = 0x172D45, .muted = 0x465970, .dim = 0x5B6B80,
        .accent = 0x075FBE, .selection = 0x526B88, .focus = 0x1768BB,
        .on_selection = 0xFFFFFF, .outline = 0x075FBE, .counter = 0xD5E0EE,
        .fast_scroll_band = 0xFFE082,
        .green = 0x236D43, .amber = 0x865310, .red = 0xAF3043,
        .purple = 0x7148A2, .cyan = 0x006B83,
        .context_panel = 0xEDF3FA, .context_group = 0xDEE7F2,
        .context_border = 0x8BA3BE, .progress = 0xB7C7DA, .scrim = 0xF4F7FB,
        .button_top = 0xFFFFFF, .button_bottom = 0xCAD9EB,
        .bevel_light = 0xFFFFFF, .bevel_shadow = 0x829BB8, .scrim_opa = 216,
    },
    [UI_THEME_WINDOWS_95] = {
        .id = UI_THEME_WINDOWS_95,
        .menu_top_bg = 0x000080, .menu_top_fg = 0xFFFFFF,
        .menu_body_bg = 0xC0C0C0, .menu_body_fg = 0x202020,
        .mini_bg = 0xC0C0C0, .mini_fg = 0x000000,
        .mini_muted = 0x303030, .mini_dim = 0x484848,
        .bg = 0xC0C0C0, .surface = 0xFFFFFF,
        .panel = 0xC0C0C0, .panel_alt = 0xC0C0C0, .border = 0x808080,
        .text = 0x000000, .muted = 0x383838, .dim = 0x505050,
        .accent = 0x000080, .selection = 0x606060, .focus = 0x000080,
        .on_selection = 0xFFFFFF, .outline = 0x000080, .counter = 0xD4D0C8,
        .fast_scroll_band = 0xFFFF00,
        .green = 0x005000, .amber = 0x704000, .red = 0x800000,
        .purple = 0x800080, .cyan = 0x005050,
        .context_panel = 0xC0C0C0, .context_group = 0xD4D0C8,
        .context_border = 0x808080, .progress = 0x808080, .scrim = 0xC0C0C0,
        .button_top = 0xC0C0C0, .button_bottom = 0xC0C0C0,
        .bevel_light = 0xFFFFFF, .bevel_shadow = 0x404040, .scrim_opa = 230,
    },
};

const ui_theme_t * ui_theme_get(int id)
{
    if(id < 0 || id >= UI_THEME_COUNT) id = UI_THEME_GRAY;
    return &ui_themes[id];
}
