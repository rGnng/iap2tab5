#include "ui.h"

#include "icons.h"
#include "fonts/inter_fonts.h"
#include "player.h"
#include "ble_remote.h"
#include "thumb_cache_format.h"

#include "lvgl.h"

/* TinyTTF is enabled in lv_conf.h. Some ESP-IDF component builds do not
 * propagate the public declaration through lvgl.h, so declare it here. */
lv_font_t * lv_tiny_ttf_create_data_ex(const void * data, size_t data_size,
                                       int32_t font_size,
                                       lv_font_kerning_t kerning,
                                       size_t cache_size);

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define UI_PERF_LOG(...) ESP_LOGI("uiperf", __VA_ARGS__)
#else
#define UI_PERF_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

/* PPA reads image sources with DMA and requires cache-line aligned buffers.
 * Keep all cached RGB565 artwork 64-byte aligned and in PSRAM. */
static void * ui_alloc_aligned(size_t size)
{
    size_t rounded = (size + 63u) & ~(size_t)63u;
#if defined(ESP_PLATFORM)
    return heap_caps_aligned_alloc(64, rounded,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void * p = NULL;
    if(posix_memalign(&p, 64, rounded) != 0) return NULL;
    return p;
#endif
}

/* -------------------------------------------------------------------------
 * Theme / layout
 * ------------------------------------------------------------------------- */
/* Section colors: four distinct gray zones.
 * Tune these values first when moving to the final in-car theme. */
#define COL_MENU_TOP_BG   0x2A2A2A
#define COL_MENU_TOP_FG   0xFFFFFF
#define COL_MENU_BODY_BG  0x0A0A0A
#define COL_MENU_BODY_FG  0xC8C8C8
#define COL_MENU_BATT_BG  0x202020
#define COL_MINI_BG       0x333333
#define COL_MINI_FG       0xFFFFFF
#define COL_MINI_MUTED    0xD0D0D0
#define COL_MINI_DIM      0xA0A0A0
#define COL_MAIN_BG       0x050505
#define COL_MAIN_FG       0xF0F0F0

#define LIST_ROW_GAP      4

#define COL_BG          0x050505
#define COL_STATUS      0x080808
#define COL_SURFACE     0x101010
#define COL_PANEL       0x141414
#define COL_PANEL_ALT   0x1E1E1E
#define COL_BORDER      0x2A2A2A
#define COL_TEXT        COL_MAIN_FG
#define COL_MUTED       0x9A9A9A
#define COL_DIM         0x666666
#define COL_ACCENT      0xC8C8C8
#define COL_ACCENT_DARK 0x262626
#define COL_FOCUS       0x303030
#define COL_GREEN       0xB8B8B8
#define COL_AMBER       0xC8C8C8
#define COL_RED         0xE6E6E6
#define COL_PURPLE      0xA8A8A8
#define COL_CYAN        0xB8B8B8

#define SCREEN_W   1280
#define SCREEN_H   720
/* The menubar is 112 px wide. Five 112 px square menu tiles fill 560 px,
 * leaving 80 px strips for the clock and battery at the top/bottom. */
#define MENUBAR_TIME_H     72
#define MENUBAR_BATTERY_H  72
#define MENUBAR_W          96
/* The mini bar now follows the menubar's 72 px top strip. */
#define MINI_NOW_H         72
/* Shared master-detail split: Albums / Artists / Playlists / Settings. */
#define SIDE_PANEL_W       480
#define CONTENT_Y          0
#define CONTENT_W          (SCREEN_W - MENUBAR_W)
#define CONTENT_H          SCREEN_H

#define MAX_FOCUS 1024

/* Type scale: four proportional levels plus tabular numeric/clock faces.
 * The queue is a compact 54 px list, so it keeps its own smaller pair. */
static const lv_font_t * FONT_H1 = &inter_36;
static const lv_font_t * FONT_NP_TITLE = &inter_40;
static const lv_font_t * FONT_NP_ARTIST = &inter_32;
static const lv_font_t * FONT_NP_TIME = &inter_tabular_20;
static const lv_font_t * FONT_DETAIL_TITLE = &inter_28;
static const lv_font_t * FONT_PAGE_TITLE = &inter_28;
static const lv_font_t * FONT_H2 = &inter_28;
static const lv_font_t * FONT_MENU = &inter_24;
static const lv_font_t * FONT_GROUP = &inter_20;
static const lv_font_t * FONT_BODY = &inter_22;
static const lv_font_t * FONT_CAPTION = &inter_18;
static const lv_font_t * FONT_QUEUE = &inter_20;
static const lv_font_t * FONT_QUEUE_SUB = &inter_16;
static const lv_font_t * FONT_NUM = &inter_tabular_18;
static const lv_font_t * FONT_CLOCK = &inter_tabular_28;
static const lv_font_t * FONT_CLOCK_12 = &inter_tabular_24;
static const lv_font_t * FONT_CLOCK_LG = &inter_tabular_36;

static uint8_t * jp_font_data;

static void fonts_init(void)
{
    static const char * paths[] = {
        "/sdcard/JPFONT.TTF",
        "JPFONT.TTF",
    };
    FILE * f = NULL;
    for(size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        f = fopen(paths[i], "rb");
        if(f != NULL) {
            UI_PERF_LOG("Japanese font: %s", paths[i]);
            break;
        }
    }
    if(f == NULL) {
        UI_PERF_LOG("Japanese font not found; using Inter");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(size <= 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        return;
    }
#if defined(ESP_PLATFORM)
    jp_font_data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
#else
    jp_font_data = malloc((size_t)size);
#endif
    if(jp_font_data == NULL ||
       fread(jp_font_data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        UI_PERF_LOG("Japanese font read failed");
        return;
    }
    fclose(f);

#define MAKE_JP(px, target)                                                        \
    do {                                                                           \
        lv_font_t * jp = lv_tiny_ttf_create_data_ex(                               \
            jp_font_data, (size_t)size, (px), LV_FONT_KERNING_NORMAL,              \
            LV_TINY_TTF_CACHE_GLYPH_CNT);                                          \
        if(jp != NULL) target = jp;                                                \
    } while(0)

    MAKE_JP(16, FONT_QUEUE_SUB);
    MAKE_JP(18, FONT_CAPTION);
    MAKE_JP(20, FONT_GROUP);
    MAKE_JP(20, FONT_QUEUE);
    MAKE_JP(22, FONT_BODY);
    MAKE_JP(24, FONT_MENU);
    MAKE_JP(28, FONT_DETAIL_TITLE);
    MAKE_JP(28, FONT_PAGE_TITLE);
    MAKE_JP(28, FONT_H2);
    MAKE_JP(32, FONT_NP_ARTIST);
    MAKE_JP(36, FONT_H1);
    MAKE_JP(40, FONT_NP_TITLE);
#undef MAKE_JP
    UI_PERF_LOG("Japanese font ready (%ld bytes)", size);
}

typedef enum {
    SCR_NOW_PLAYING = 0,
    SCR_ALBUMS,
    SCR_ARTISTS,
    SCR_SONGS,
    SCR_PLAYLISTS,
    SCR_BLUETOOTH,
    SCR_SETTINGS,
    SCR_COUNT,
} screen_id_t;

/* -------------------------------------------------------------------------
 * Focus engine
 * ------------------------------------------------------------------------- */
typedef void (*focus_activate_fn)(void * user);
typedef void (*focus_adjust_fn)(int delta, void * user);
typedef void (*focus_focus_fn)(void * user);

typedef struct {
    lv_obj_t * obj;
    focus_activate_fn activate;
    focus_adjust_fn adjust;
    focus_focus_fn on_focus;
    void * user;
    bool fill;
    bool silent;
} focus_item_t;

static void enable_event_bubble(lv_obj_t * obj);
static void settings_update_system_labels(void);

static focus_item_t g_focus[MAX_FOCUS];
static int g_focus_count;
static int g_focus_index = -1;
static bool g_edit_mode;
static const char * g_page_hint;
static lv_obj_t * albums_left_list;
static lv_obj_t * songs_list;

static ui_system_config_t g_system_config = {
    .screen_flip = 0,
    .wifi_mode = 0,
    .sound_enabled = true,
    .volume = 65,
    .wifi_status = "WiFi off",
    .wifi_ip = "--",
};
static void (*g_system_config_changed_cb)(const ui_system_config_t * cfg);
static void (*g_feedback_cb)(int type);
static void (*g_afc_status_cb)(ui_afc_status_t * out);
static void (*g_afc_action_cb)(int action);

static void ui_feedback_emit(ui_feedback_t type)
{
    if(g_feedback_cb != NULL) g_feedback_cb((int)type);
}

static void ui_set_hint(const char * text)
{
    g_page_hint = text;
}

static void ui_enter_edit(const char * hint)
{
    LV_UNUSED(hint);
    g_edit_mode = true;
}

static void ui_exit_edit(void)
{
    g_edit_mode = false;
}

static void focus_visual(focus_item_t * item, bool focused)
{
    if(item == NULL || item->obj == NULL || item->silent) return;

    if(item->fill) {
        lv_obj_set_style_bg_opa(item->obj, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(item->obj, lv_color_hex(COL_FOCUS), 0);
        lv_obj_set_style_border_width(item->obj, focused ? 3 : 0, 0);
        lv_obj_set_style_border_side(item->obj, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(item->obj, lv_color_hex(COL_ACCENT), 0);
    }
    else {
        lv_obj_set_style_outline_width(item->obj, focused ? 3 : 0, 0);
        lv_obj_set_style_outline_color(item->obj, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_outline_opa(item->obj, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_pad(item->obj, 3, 0);
    }

    if(focused) lv_obj_scroll_to_view(item->obj, LV_ANIM_OFF);
}

static void focus_clear(void)
{
    if(g_focus_index >= 0 && g_focus_index < g_focus_count) {
        focus_visual(&g_focus[g_focus_index], false);
    }
    g_focus_count = 0;
    g_focus_index = -1;
    g_edit_mode = false;
}

static int focus_add(lv_obj_t * obj, focus_activate_fn activate,
                     focus_adjust_fn adjust, void * user, bool fill)
{
    if(g_focus_count >= MAX_FOCUS) return -1;
    int index = g_focus_count++;
    g_focus[index].obj = obj;
    g_focus[index].activate = activate;
    g_focus[index].adjust = adjust;
    g_focus[index].on_focus = NULL;
    g_focus[index].user = user;
    g_focus[index].fill = fill;
    g_focus[index].silent = false;
    return index;
}

static void focus_set_silent(int index, bool silent)
{
    if(index < 0 || index >= g_focus_count) return;
    g_focus[index].silent = silent;
}

static void focus_set_on_focus(int index, focus_focus_fn fn)
{
    if(index < 0 || index >= g_focus_count) return;
    g_focus[index].on_focus = fn;
}

static void focus_select(int index)
{
    if(g_focus_count <= 0) return;
    if(index < 0) index = 0;
    if(index >= g_focus_count) index = g_focus_count - 1;
    if(index == g_focus_index) return;

    if(g_focus_index >= 0 && g_focus_index < g_focus_count) {
        focus_visual(&g_focus[g_focus_index], false);
    }
    g_focus_index = index;
    focus_visual(&g_focus[g_focus_index], true);
    if(g_focus[g_focus_index].on_focus) {
        g_focus[g_focus_index].on_focus(g_focus[g_focus_index].user);
    }
}

static void focus_move(int delta)
{
    if(g_focus_count <= 0) return;

    if(g_edit_mode) {
        focus_item_t * item = &g_focus[g_focus_index];
        if(item->adjust) item->adjust(delta, item->user);
        return;
    }

    int next = g_focus_index + delta;
    if(next < 0) next = 0;
    if(next >= g_focus_count) next = g_focus_count - 1;
    focus_select(next);
}

static void focus_activate(void)
{
    if(g_focus_count <= 0 || g_focus_index < 0) return;
    if(g_edit_mode) {
        ui_exit_edit();
        return;
    }
    focus_item_t * item = &g_focus[g_focus_index];
    ui_feedback_emit(UI_FEEDBACK_ACTIVATE);
    if(item->activate) item->activate(item->user);
}

static void focus_click_async(void * arg)
{
    int index = (int)(intptr_t)arg;
    focus_select(index);
    focus_activate();
}

/* Actions triggered from LVGL input callbacks must not delete the object that
 * is still handling the event.  Queue them and run them from the next timer
 * tick, after the input event has completely unwound. */
typedef void (*ui_pending_fn_t)(void * arg);
typedef struct {
    ui_pending_fn_t fn;
    void * arg;
} ui_pending_t;

#define UI_PENDING_MAX 8
static ui_pending_t s_pending[UI_PENDING_MAX];
static int s_pending_count;
static bool s_pending_scheduled;

static void ui_pending_run(void * unused)
{
    LV_UNUSED(unused);
    while(s_pending_count > 0) {
        ui_pending_t action = s_pending[0];
        for(int i = 1; i < s_pending_count; i++) {
            s_pending[i - 1] = s_pending[i];
        }
        s_pending_count--;
        if(action.fn != NULL) action.fn(action.arg);
    }
    s_pending_scheduled = false;
}

static bool ui_call_async(ui_pending_fn_t fn, void * arg)
{
    if(fn == NULL || s_pending_count >= UI_PENDING_MAX) return false;
    if(!s_pending_scheduled) {
        if(lv_async_call(ui_pending_run, NULL) != LV_RESULT_OK) return false;
        s_pending_scheduled = true;
    }
    s_pending[s_pending_count].fn = fn;
    s_pending[s_pending_count].arg = arg;
    s_pending_count++;
    return true;
}

static void focus_click_cb(lv_event_t * event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    ui_call_async(focus_click_async, (void *)(intptr_t)index);
}

static int focus_add_clickable(lv_obj_t * obj, focus_activate_fn activate,
                               focus_adjust_fn adjust, void * user, bool fill)
{
    lv_obj_set_clickable(obj, true);
    enable_event_bubble(obj);
    int index = focus_add(obj, activate, adjust, user, fill);
    if(index >= 0) {
        lv_obj_add_event_cb(obj, focus_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)index);
    }
    return index;
}

/* Touch-only action: usable on the inactive panel without joining the knob
 * focus chain. */
static void touch_add_clickable(lv_obj_t * obj, lv_event_cb_t cb, void * user)
{
    lv_obj_set_clickable(obj, true);
    enable_event_bubble(obj);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user);
}

/* -------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */
static lv_obj_t * make_label(lv_obj_t * parent, const char * text,
                             const lv_font_t * font, uint32_t color)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_scrollable(label, false);
    lv_obj_set_scroll_chain(label, false);
    lv_obj_set_event_bubble(label, true);
    return label;
}

static void enable_event_bubble(lv_obj_t * obj)
{
    lv_obj_set_event_bubble(obj, true);
    uint32_t count = lv_obj_get_child_count(obj);
    for(uint32_t i = 0; i < count; i++) {
        enable_event_bubble(lv_obj_get_child(obj, i));
    }
}

static lv_obj_t * make_icon(lv_obj_t * parent, const char * icon, uint32_t color)
{
    return make_label(parent, icon, &bootstrap_icons_24, color);
}

static lv_obj_t * make_panel(lv_obj_t * parent)
{
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return panel;
}

static lv_obj_t * make_panel_content(lv_obj_t * panel, int pad)
{
    lv_obj_t * content = lv_obj_create(panel);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, pad, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(content, false);
    return content;
}

static lv_obj_t * make_scroll(lv_obj_t * parent)
{
    lv_obj_t * list = lv_obj_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, LIST_ROW_GAP, 0);
    lv_obj_set_style_radius(list, 0, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_elastic(list, true);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_chain(list, false);
    return list;
}

static lv_obj_t * make_row(lv_obj_t * parent, int height)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);
    /* Let a drag that starts on a row chain to the scrollable list. */
    lv_obj_set_scroll_chain(row, true);
    return row;
}

static lv_obj_t * make_flat_icon_button(lv_obj_t * parent, const char * icon,
                                        int width, int height, uint32_t fg,
                                        const lv_font_t * font)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    make_label(button, icon, font, fg);
    return button;
}

#define THUMB_COUNT 100

static uint8_t * thumb34_pack;
static uint8_t * thumb54_pack;
static uint8_t * thumb80_pack;
static lv_image_dsc_t thumb34_dsc[THUMB_COUNT];
static lv_image_dsc_t thumb54_dsc[THUMB_COUNT];
static lv_image_dsc_t thumb80_dsc[THUMB_COUNT];
static bool thumbs_ready;

static bool load_thumb_pack(const char * path, uint8_t ** pack,
                            lv_image_dsc_t * dsc, int size)
{
    FILE * f = fopen(path, "rb");
    if(f == NULL) return false;

    const size_t one = (size_t)size * size * 2;
    const size_t total = one * THUMB_COUNT;
    uint8_t * buffer = malloc(total);
    if(buffer == NULL) {
        fclose(f);
        return false;
    }

    size_t n = fread(buffer, 1, total, f);
    fclose(f);
    if(n != total) {
        free(buffer);
        return false;
    }

    for(int i = 0; i < THUMB_COUNT; i++) {
        lv_memzero(&dsc[i], sizeof(dsc[i]));
        dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        dsc[i].header.w = size;
        dsc[i].header.h = size;
        dsc[i].header.stride = size * 2;
        dsc[i].data_size = one;
        dsc[i].data = buffer + (size_t)i * one;
    }
    *pack = buffer;
    return true;
}

static void thumbs_init(void)
{
    if(thumbs_ready) return;
#if defined(ESP_PLATFORM)
    /* Hardware uses the real packed thumbnail cache; the demo packs are not
     * part of the firmware image. */
    thumbs_ready = false;
    return;
#else
    bool ok = load_thumb_pack(MOCK_ASSET_DIR "/thumbs34.bin", &thumb34_pack,
                              thumb34_dsc, 34);
    ok = load_thumb_pack(MOCK_ASSET_DIR "/thumbs54.bin", &thumb54_pack,
                         thumb54_dsc, 54) && ok;
    ok = load_thumb_pack(MOCK_ASSET_DIR "/thumbs80.bin", &thumb80_pack,
                         thumb80_dsc, 80) && ok;
    thumbs_ready = ok;
    if(!ok) UI_PERF_LOG("thumbnail pack load failed");
#endif
}

#define REAL_THUMB_SIZE 54
#define COVER_CACHE_SLOTS 192

static char s_thumb_pack_path[128];
static char s_thumb_pack_dir[128];
static FILE * s_thumb_pack_file;
static uint8_t * s_thumb_pack_ram;
static size_t s_thumb_pack_ram_size;
static bool s_thumb_pack_ram_ready;
static uint32_t s_thumb_pack_count;
static uint8_t s_thumb_valid[512];
static bool s_thumb_pack_ready;

void ui_set_thumb_pack_dir(const char * dir)
{
    snprintf(s_thumb_pack_dir, sizeof(s_thumb_pack_dir), "%s",
             dir != NULL ? dir : "");
}

#if defined(ESP_PLATFORM)
typedef struct {
    char path[128];
    int albums;
} thumb_load_arg_t;

static void thumb_pack_load_task(void * p)
{
    thumb_load_arg_t * a = (thumb_load_arg_t *)p;
    const size_t one = (size_t)REAL_THUMB_SIZE * REAL_THUMB_SIZE * 2;
    size_t bytes = (size_t)a->albums * one;
    if(bytes > 0 && bytes <= 16u * 1024u * 1024u) {
        FILE * f = fopen(a->path, "rb");
        if(f != NULL) {
            uint32_t t0 = (uint32_t)esp_timer_get_time();
            uint8_t * ram = ui_alloc_aligned(bytes);
            if(ram != NULL && fread(ram, 1, bytes, f) == bytes) {
                s_thumb_pack_ram = ram;
                s_thumb_pack_ram_size = bytes;
                s_thumb_pack_ram_ready = true;
                UI_PERF_LOG("thumb pack in PSRAM: %d albums, %u KB, %u ms",
                            a->albums, (unsigned)(bytes / 1024),
                            (unsigned)((esp_timer_get_time() - t0) / 1000));
            }
            else {
                free(ram);
            }
            fclose(f);
        }
    }
    free(a);
    vTaskDelete(NULL);
}
#endif

void ui_set_thumb_pack_path(const char * path, int album_count)
{
    s_thumb_pack_ready = false;
    s_thumb_pack_count = 0;
    if(s_thumb_pack_file != NULL) {
        fclose(s_thumb_pack_file);
        s_thumb_pack_file = NULL;
    }
    if(s_thumb_pack_ram != NULL) {
        free(s_thumb_pack_ram);
        s_thumb_pack_ram = NULL;
    }
    s_thumb_pack_ram_size = 0;
    s_thumb_pack_ram_ready = false;
    memset(s_thumb_valid, 0, sizeof(s_thumb_valid));
    if(path == NULL || path[0] == '\0' || album_count <= 0) return;

    snprintf(s_thumb_pack_path, sizeof(s_thumb_pack_path), "%s", path);
    s_thumb_pack_count = (uint32_t)album_count;

    const char * slash = strrchr(path, '/');
    char idx_path[128];
    if(slash != NULL) {
        snprintf(idx_path, sizeof(idx_path), "%.*s/thumbs.idx",
                 (int)(slash - path), path);
    }
    else {
        snprintf(idx_path, sizeof(idx_path), "thumbs.idx");
    }

    size_t need = ((size_t)album_count + 7) / 8;
    if(need > sizeof(s_thumb_valid)) need = sizeof(s_thumb_valid);
    FILE * f = fopen(idx_path, "rb");
    if(f == NULL) return;
    size_t n = fread(s_thumb_valid, 1, need, f);
    fclose(f);
    if(n == 0) return;
    /* Keep the pack open: opening the thumbnail file for every row was the
     * main source of album-list scrolling stalls.  Only the UI thread reads
     * thumbnails, so a single shared FILE is safe here. */
    s_thumb_pack_file = fopen(s_thumb_pack_path, "rb");
    if(s_thumb_pack_file == NULL) return;
    s_thumb_pack_ready = true;

    /* Keep the whole packed thumbnail set in PSRAM.  Reading one 54x54
     * cover from SD during a scroll costs tens of milliseconds; a memcpy
     * from PSRAM makes list scrolling effectively free.  Load it in the
     * background so the 10+ MB read does not delay boot. */
#if defined(ESP_PLATFORM)
    thumb_load_arg_t * arg = malloc(sizeof(*arg));
    if(arg != NULL) {
        snprintf(arg->path, sizeof(arg->path), "%s", s_thumb_pack_path);
        arg->albums = album_count;
        if(xTaskCreate(thumb_pack_load_task, "thumb_load", 6144, arg, 1,
                       NULL) != pdPASS) {
            free(arg);
        }
    }
#endif
}

static char s_artwork_dir[64] = "/sdcard/covers";

static bool real_thumb_read(uint16_t id, uint8_t * dst)
{
    const size_t one = (size_t)REAL_THUMB_SIZE * REAL_THUMB_SIZE * 2;
    if(id == 0) return false;

    if(s_thumb_pack_ready && id <= s_thumb_pack_count) {
        uint32_t n = (uint32_t)id - 1;
        if((s_thumb_valid[n >> 3] & (1u << (n & 7))) != 0) {
            size_t off = (size_t)thumb_cache_slot_offset(id);
            if(s_thumb_pack_ram_ready && off + one <= s_thumb_pack_ram_size) {
                memcpy(dst, s_thumb_pack_ram + off, one);
                return true;
            }
            if(s_thumb_pack_file != NULL &&
               fseek(s_thumb_pack_file, (long)off, SEEK_SET) == 0 &&
               fread(dst, 1, one, s_thumb_pack_file) == one) {
                return true;
            }
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%u.thm", s_artwork_dir, (unsigned)id);
    FILE * f = fopen(path, "rb");
    if(f == NULL) return false;
    size_t n = fread(dst, 1, one, f);
    fclose(f);
    return n == one;
}

typedef struct {
    uint16_t id;
    uint8_t * data;
    lv_image_dsc_t dsc;
} cover_cache_slot_t;

static cover_cache_slot_t cover_cache[COVER_CACHE_SLOTS];

void ui_set_artwork_dir(const char * dir)
{
    snprintf(s_artwork_dir, sizeof(s_artwork_dir), "%s",
             dir != NULL && dir[0] != '\0' ? dir : "/sdcard/covers");
}
static int cover_cache_next;

static const lv_image_dsc_t * real_thumb_for_album(uint16_t id)
{
    if(id == 0) return NULL;
    for(int i = 0; i < COVER_CACHE_SLOTS; i++) {
        if(cover_cache[i].id == id) return &cover_cache[i].dsc;
    }

    cover_cache_slot_t * slot = &cover_cache[cover_cache_next++ % COVER_CACHE_SLOTS];
    const size_t one = (size_t)REAL_THUMB_SIZE * REAL_THUMB_SIZE * 2;
    if(slot->data == NULL) {
        slot->data = ui_alloc_aligned(one);
        if(slot->data == NULL) return NULL;
    }
    slot->id = 0;

    if(!real_thumb_read(id, slot->data)) return NULL;

    lv_memzero(&slot->dsc, sizeof(slot->dsc));
    slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->dsc.header.w = REAL_THUMB_SIZE;
    slot->dsc.header.h = REAL_THUMB_SIZE;
    slot->dsc.header.stride = REAL_THUMB_SIZE * 2;
    slot->dsc.data_size = one;
    slot->dsc.data = slot->data;
    slot->id = id;
    return &slot->dsc;
}

#define REAL_COVER_SIZE 360

static uint8_t * real_cover_buf;
static lv_image_dsc_t real_cover_dsc;
static uint16_t real_cover_id;
static bool real_cover_valid;

/* Full-size artwork generated during the AFC artwork sync.  The file is a
 * packed 360x360 RGB565 image, so it can be handed to LVGL as a descriptor
 * without any runtime JPEG decoding or scaling. */
static const lv_image_dsc_t * real_cover_for_album(uint16_t id)
{
    if(id == 0) return NULL;
    if(real_cover_valid && real_cover_id == id) return &real_cover_dsc;

    real_cover_valid = false;
    const size_t one = (size_t)REAL_COVER_SIZE * REAL_COVER_SIZE * 2;
    if(real_cover_buf == NULL) {
        real_cover_buf = ui_alloc_aligned(one);
        if(real_cover_buf == NULL) return NULL;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%u.cov", s_artwork_dir, (unsigned)id);
    FILE * f = fopen(path, "rb");
    if(f == NULL) return NULL;
    size_t n = fread(real_cover_buf, 1, one, f);
    fclose(f);
    if(n != one) return NULL;

    lv_memzero(&real_cover_dsc, sizeof(real_cover_dsc));
    real_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    real_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    real_cover_dsc.header.w = REAL_COVER_SIZE;
    real_cover_dsc.header.h = REAL_COVER_SIZE;
    real_cover_dsc.header.stride = REAL_COVER_SIZE * 2;
    real_cover_dsc.data_size = one;
    real_cover_dsc.data = real_cover_buf;
    real_cover_id = id;
    real_cover_valid = true;
    return &real_cover_dsc;
}

static const void * album_cover_src(const player_album_t * album)
{
    if(album == NULL) return NULL;
    if(album->cover_index >= PLAYER_REAL_COVER_BASE) {
        return real_cover_for_album(
            (uint16_t)(album->cover_index - PLAYER_REAL_COVER_BASE));
    }
    return album->cover[0] != '\0' ? album->cover : NULL;
}

typedef struct {
    uint8_t * data;
    lv_image_dsc_t dsc;
    uint16_t id;
} real_thumb_slot_t;

static real_thumb_slot_t mini_thumb_slot;
static real_thumb_slot_t detail_thumb_slot;

/* Single-purpose thumbnail slots keep the mini player and the album detail
 * header stable while the scrolling list evicts entries from the 48-slot
 * cache. */
static const lv_image_dsc_t * real_thumb_single(real_thumb_slot_t * slot,
                                                uint16_t id)
{
    if(id == 0) return NULL;
    if(slot->data != NULL && slot->id == id) return &slot->dsc;

    const size_t one = (size_t)REAL_THUMB_SIZE * REAL_THUMB_SIZE * 2;
    if(slot->data == NULL) {
        slot->data = ui_alloc_aligned(one);
        if(slot->data == NULL) return NULL;
    }
    slot->id = 0;

    if(!real_thumb_read(id, slot->data)) return NULL;

    lv_memzero(&slot->dsc, sizeof(slot->dsc));
    slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->dsc.header.w = REAL_THUMB_SIZE;
    slot->dsc.header.h = REAL_THUMB_SIZE;
    slot->dsc.header.stride = REAL_THUMB_SIZE * 2;
    slot->dsc.data_size = one;
    slot->dsc.data = slot->data;
    slot->id = id;
    return &slot->dsc;
}

static const void * thumb_for_album(const player_album_t * album, int size)
{
    if(album == NULL) return NULL;
    if(album->cover_index >= PLAYER_REAL_COVER_BASE) {
        return real_thumb_for_album(
            (uint16_t)(album->cover_index - PLAYER_REAL_COVER_BASE));
    }
    if(!thumbs_ready || album->cover_index >= THUMB_COUNT ||
       album->cover[0] == '\0') {
        return album->cover[0] != '\0' ? album->cover : NULL;
    }
    if(size <= 40) return &thumb34_dsc[album->cover_index];
    if(size <= 60) return &thumb54_dsc[album->cover_index];
    if(size <= 90) return &thumb80_dsc[album->cover_index];
    return album->cover;
}

static const void * mini_thumb_for_album(const player_album_t * album)
{
    if(album == NULL) return NULL;
    if(album->cover_index >= PLAYER_REAL_COVER_BASE) {
        return real_thumb_single(
            &mini_thumb_slot,
            (uint16_t)(album->cover_index - PLAYER_REAL_COVER_BASE));
    }
    return thumb_for_album(album, 56);
}

static const void * detail_thumb_for_album(const player_album_t * album)
{
    if(album == NULL) return NULL;
    if(album->cover_index >= PLAYER_REAL_COVER_BASE) {
        return real_thumb_single(
            &detail_thumb_slot,
            (uint16_t)(album->cover_index - PLAYER_REAL_COVER_BASE));
    }
    return thumb_for_album(album, 80);
}

static lv_obj_t * make_cover(lv_obj_t * parent, const void * src,
                             uint32_t fallback_color, int size)
{
    lv_obj_t * cover = lv_image_create(parent);
    lv_obj_set_size(cover, size, size);
    lv_obj_set_style_pad_all(cover, 0, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(fallback_color), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_clickable(cover, false);
    lv_obj_set_event_bubble(cover, true);

    lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_STRETCH);
    if(src != NULL) lv_image_set_src(cover, src);
    return cover;
}

/* iAP2 Now Playing artwork is decoded by the firmware into a 360x360 RGB565
 * buffer. Keep one UI copy here so the LVGL image descriptor stays valid. */
#define NP_ART_SIZE 360
static uint8_t * g_iap2_cover_buf;
static lv_image_dsc_t g_iap2_cover_dsc;
static uint32_t g_iap2_cover_rev;
static bool g_iap2_cover_ready;

static const lv_image_dsc_t * iap2_cover_src(void)
{
    uint32_t rev = player_artwork_revision();
    if(rev == 0) {
        g_iap2_cover_ready = false;
        return NULL;
    }
    if(rev != g_iap2_cover_rev) {
        g_iap2_cover_rev = rev;
        size_t bytes = (size_t)NP_ART_SIZE * NP_ART_SIZE * 2;
        if(g_iap2_cover_buf == NULL) {
            g_iap2_cover_buf = ui_alloc_aligned(bytes);
        }
        g_iap2_cover_ready = g_iap2_cover_buf != NULL &&
                             player_artwork_copy(g_iap2_cover_buf, bytes);
        if(g_iap2_cover_ready) {
            lv_memzero(&g_iap2_cover_dsc, sizeof(g_iap2_cover_dsc));
            g_iap2_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            g_iap2_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            g_iap2_cover_dsc.header.w = NP_ART_SIZE;
            g_iap2_cover_dsc.header.h = NP_ART_SIZE;
            g_iap2_cover_dsc.header.stride = NP_ART_SIZE * 2;
            g_iap2_cover_dsc.data_size = bytes;
            g_iap2_cover_dsc.data = g_iap2_cover_buf;
        }
    }
    return g_iap2_cover_ready ? &g_iap2_cover_dsc : NULL;
}

static void format_time(uint32_t seconds, char * buffer, size_t size)
{
    snprintf(buffer, size, "%u:%02u", (unsigned)(seconds / 60),
             (unsigned)(seconds % 60));
}

static bool find_album(uint64_t album_id, player_album_t * out);
static bool np_resolve_current_album(const player_snapshot_t * snapshot,
                                     player_album_t * album,
                                     uint64_t * album_id);

static const char * battery_icon_for(int level, bool charging)
{
    if(charging) return ICON_BATTERY_CHARGING;
    if(level < 0) return ICON_BATTERY;
    if(level <= 15) return ICON_BATTERY;
    if(level <= 40) return ICON_BATTERY_LOW;
    if(level <= 75) return ICON_BATTERY_HALF;
    return ICON_BATTERY_FULL;
}

static uint32_t battery_color_for(int level, bool charging)
{
    (void)charging;
    if(level < 0) return COL_DIM;
    return COL_MENU_BODY_FG;
}

/* -------------------------------------------------------------------------
 * Shell: status bar + vertical menubar
 * ------------------------------------------------------------------------- */
static lv_obj_t * g_content;
static lv_obj_t * g_page;
static lv_obj_t * g_mini;
static lv_obj_t * g_mini_cover;
static lv_obj_t * g_mini_title;
static lv_obj_t * g_mini_artist;
static lv_obj_t * g_mini_shuffle;
static lv_obj_t * g_mini_repeat;
static lv_obj_t * g_mini_star;
static lv_obj_t * g_mini_queue;
static lv_obj_t * g_mini_time;
static uint64_t g_mini_track_id;
static uint32_t g_mini_art_rev;
static uint32_t g_mini_rev = 0xffffffffu;
static uint32_t g_mini_elapsed = 0xffffffffu;
static int g_mini_queue_index = -999;
static int g_mini_queue_count = -999;
static lv_obj_t * g_status_time;
static bool g_clock_12h;
static int g_clock_offset_min;
static lv_obj_t * g_status_battery;
static lv_obj_t * g_menubar;
static lv_obj_t * g_menu_items[SCR_COUNT];
static lv_obj_t * g_menu_icons[SCR_COUNT];
static bool g_menubar_left = true;
static lv_indev_t * g_touch_indev;
static bool g_touch_enabled = true;
static screen_id_t g_current_screen = SCR_NOW_PLAYING;
static screen_id_t g_ble_return_screen = SCR_COUNT;
static bool g_ctx_menu_open;
static bool g_afc_sync_mode;
static bool g_device_available;
static bool g_ui_trace;

static uint32_t perf_now(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)esp_timer_get_time();
#else
    return (uint32_t)((uint64_t)clock() * 1000000u / CLOCKS_PER_SEC);
#endif
}

static void perf_log_us(const char * name, uint32_t t0)
{
    if(g_ui_trace) {
        UI_PERF_LOG("%s: %u us", name, (unsigned)(perf_now() - t0));
    }
}

typedef struct {
    void (*create)(lv_obj_t * parent);
    void (*tick)(void);
    bool (*back)(void);
    void (*same_key)(void);
    bool (*key)(ui_key_t key);
} page_t;

static const page_t g_pages[SCR_COUNT];

static void ui_show_screen(screen_id_t id);
static void ui_rebuild_page(void);
static bool ui_list_page(int direction);
static void np_toggle_queue(void);
static void context_menu_open(void);
static void context_menu_close(void);
static bool context_menu_key(ui_key_t key);

static void menu_click_async(void * arg)
{
    ui_show_screen((screen_id_t)(intptr_t)arg);
}

static void menu_click_cb(lv_event_t * event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    ui_call_async(menu_click_async, (void *)(intptr_t)index);
}

static player_status_t g_menu_play_status = (player_status_t)-1;

static void ui_menu_play_update(void)
{
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    if(snapshot.status == g_menu_play_status) return;
    g_menu_play_status = snapshot.status;
    lv_label_set_text(g_menu_icons[SCR_NOW_PLAYING],
                      snapshot.status == PLAYER_PLAYING ? ICON_PLAY : ICON_PAUSE);
}

static void ui_player_changed(void)
{
    ui_menu_play_update();
}

static void ui_menu_update(void)
{
    for(int i = 0; i < SCR_COUNT; i++) {
        if(g_menu_items[i] == NULL || g_menu_icons[i] == NULL) continue;
        bool active = (i == (int)g_current_screen) ||
                      (i == SCR_SETTINGS && g_current_screen == SCR_BLUETOOTH);
        /* The selected tile is a solid square: the full 112 x 112 touch area
         * uses the focus grey fill, with a white icon and no edge strip. */
        lv_obj_set_style_bg_color(g_menu_items[i],
                                  lv_color_hex(active ? COL_FOCUS : COL_BG), 0);
        lv_obj_set_style_bg_opa(g_menu_items[i],
                                active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_menu_items[i], 0, 0);
        lv_obj_set_style_text_color(g_menu_icons[i],
                                    lv_color_hex(active ? COL_MENU_TOP_FG
                                                        : COL_MENU_BODY_FG), 0);
    }
}

static void mini_now_click_async(void * arg)
{
    LV_UNUSED(arg);
    ui_show_screen(SCR_NOW_PLAYING);
}

static void mini_now_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(mini_now_click_async, NULL);
}

static void ui_mini_update(void)
{
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    uint32_t art_rev = player_artwork_revision();
    /* The mini player lives on every non-Now-Playing screen.  Re-writing its
     * labels and styles every 500 ms invalidates a large part of the screen
     * even when nothing changed, which makes the lists feel sluggish. */
    if(snapshot.revision == g_mini_rev && art_rev == g_mini_art_rev) {
        if(g_mini_time != NULL && snapshot.elapsed_sec != g_mini_elapsed) {
            g_mini_elapsed = snapshot.elapsed_sec;
            char current[16];
            char total[16];
            format_time(snapshot.elapsed_sec, current, sizeof(current));
            format_time(snapshot.duration_sec, total, sizeof(total));
            lv_label_set_text_fmt(g_mini_time, "%s / %s", current, total);
        }
        return;
    }
    g_mini_rev = snapshot.revision;
    g_mini_elapsed = snapshot.elapsed_sec;
    bool art_changed = (art_rev != g_mini_art_rev);
    g_mini_art_rev = art_rev;
    int queue_index = player_queue_index();
    int queue_count = player_queue_count();
    g_mini_queue_index = queue_index;
    g_mini_queue_count = queue_count;

    if(snapshot.track.id != g_mini_track_id || art_changed) {
        g_mini_track_id = snapshot.track.id;
        lv_label_set_text(g_mini_title,
                          snapshot.track.title[0] ? snapshot.track.title : "No track");
        const char * artist = snapshot.track.artist;
        const char * album_text = snapshot.track.album;
        char second_line[192] = "";
        if(artist[0] != '\0' && album_text[0] != '\0') {
            snprintf(second_line, sizeof(second_line), "%s  |  %s",
                     artist, album_text);
        }
        else if(artist[0] != '\0') {
            snprintf(second_line, sizeof(second_line), "%s", artist);
        }
        else if(album_text[0] != '\0') {
            snprintf(second_line, sizeof(second_line), "%s", album_text);
        }
        if(second_line[0] != '\0') {
            lv_label_set_text(g_mini_artist, second_line);
            lv_obj_set_hidden(g_mini_artist, false);
        }
        else {
            lv_label_set_text(g_mini_artist, "");
            lv_obj_set_hidden(g_mini_artist, true);
        }

        player_album_t now_album;
        uint64_t now_album_id = 0;
        bool has_now_album = np_resolve_current_album(&snapshot, &now_album,
                                                      &now_album_id);
        const lv_image_dsc_t * art =
            snapshot.real_device && snapshot.track.title[0] != '\0'
                ? iap2_cover_src() : NULL;
        if(art != NULL) {
            lv_image_set_src(g_mini_cover, art);
            lv_obj_set_style_bg_color(g_mini_cover, lv_color_hex(COL_PANEL), 0);
            lv_obj_set_style_bg_opa(g_mini_cover, LV_OPA_TRANSP, 0);
        }
        else if(has_now_album) {
            lv_image_set_src(g_mini_cover, mini_thumb_for_album(&now_album));
            lv_obj_set_style_bg_color(g_mini_cover, lv_color_hex(now_album.color), 0);
            lv_obj_set_style_bg_opa(g_mini_cover, LV_OPA_COVER, 0);
        }
        else {
            lv_image_set_src(g_mini_cover, NULL);
            lv_obj_set_style_bg_color(g_mini_cover, lv_color_hex(COL_PANEL_ALT), 0);
            lv_obj_set_style_bg_opa(g_mini_cover, LV_OPA_COVER, 0);
        }
    }

    lv_obj_set_style_text_color(g_mini_shuffle,
                                lv_color_hex(snapshot.shuffle ? COL_MINI_FG
                                                              : COL_MUTED), 0);
    lv_obj_set_style_text_color(g_mini_repeat,
                                lv_color_hex(snapshot.repeat != PLAYER_REPEAT_OFF
                                             ? COL_MINI_FG : COL_MUTED), 0);
    lv_label_set_text(g_mini_repeat,
                      snapshot.repeat == PLAYER_REPEAT_ONE ? ICON_REPEAT_ONE
                                                           : ICON_REPEAT);

    if(g_mini_queue) {
        if(queue_count > 0 && queue_index >= 0) {
            lv_label_set_text_fmt(g_mini_queue, "%d / %d",
                                  queue_index + 1, queue_count);
        }
        else {
            lv_label_set_text(g_mini_queue, "— / —");
        }
    }

    if(g_mini_time) {
        char current[16];
        char total[16];
        format_time(snapshot.elapsed_sec, current, sizeof(current));
        format_time(snapshot.duration_sec, total, sizeof(total));
        lv_label_set_text_fmt(g_mini_time, "%s / %s", current, total);
    }

    if(g_mini_star) {
        lv_label_set_text(g_mini_star,
                          snapshot.starred ? ICON_STAR_FILL : ICON_STAR);
        lv_obj_set_style_text_color(g_mini_star,
                                    lv_color_hex(snapshot.starred ? COL_MINI_FG
                                                                  : COL_MINI_MUTED), 0);
    }
}

static void build_mini_now(lv_obj_t * parent)
{
    g_mini = lv_obj_create(parent);
    lv_obj_set_size(g_mini, LV_PCT(100), MINI_NOW_H);
    lv_obj_set_pos(g_mini, 0, 0);
    lv_obj_set_style_bg_color(g_mini, lv_color_hex(COL_MINI_BG), 0);
    lv_obj_set_style_bg_opa(g_mini, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_mini, 0, 0);
    lv_obj_set_style_border_width(g_mini, 0, 0);
    lv_obj_set_style_pad_hor(g_mini, 20, 0);
    lv_obj_set_style_pad_ver(g_mini, 0, 0);
    lv_obj_set_style_pad_column(g_mini, 20, 0);
    lv_obj_set_flex_flow(g_mini, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_mini, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(g_mini, false);
    lv_obj_set_clickable(g_mini, true);
    lv_obj_add_event_cb(g_mini, mini_now_click_cb, LV_EVENT_CLICKED, NULL);

    g_mini_cover = make_cover(g_mini, NULL, COL_PANEL, 56);

    lv_obj_t * text = lv_obj_create(g_mini);
    lv_obj_set_height(text, 60);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text, 0, 0);
    lv_obj_set_style_radius(text, 0, 0);
    lv_obj_set_style_pad_all(text, 0, 0);
    lv_obj_set_style_pad_row(text, 1, 0);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(text, false);

    g_mini_title = make_label(text, "No track", FONT_H2, COL_MINI_FG);
    lv_obj_set_width(g_mini_title, LV_PCT(100));
    lv_obj_set_height(g_mini_title, 34);
    lv_label_set_long_mode(g_mini_title, LV_LABEL_LONG_MODE_DOTS);
    g_mini_artist = make_label(text, "", FONT_CAPTION, COL_MINI_MUTED);
    lv_obj_set_width(g_mini_artist, LV_PCT(100));
    lv_obj_set_height(g_mini_artist, 24);
    lv_label_set_long_mode(g_mini_artist, LV_LABEL_LONG_MODE_DOTS);

    /* Queue position and elapsed/duration: 2 x 80 px time block. */
    lv_obj_t * meta = lv_obj_create(g_mini);
    lv_obj_set_size(meta, 160, 60);
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta, 0, 0);
    lv_obj_set_style_radius(meta, 0, 0);
    lv_obj_set_style_pad_all(meta, 0, 0);
    lv_obj_set_style_pad_top(meta, 2, 0);
    lv_obj_set_style_pad_row(meta, 1, 0);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_scrollable(meta, false);

    g_mini_queue = make_label(meta, "— / —", FONT_NUM,
                              COL_MINI_MUTED);
    lv_obj_set_width(g_mini_queue, LV_PCT(100));
    lv_obj_set_height(g_mini_queue, 24);
    lv_obj_set_style_text_align(g_mini_queue, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(g_mini_queue, LV_LABEL_LONG_MODE_DOTS);

    g_mini_time = make_label(meta, "0:00 / 0:00", FONT_NUM,
                             COL_MINI_MUTED);
    lv_obj_set_width(g_mini_time, LV_PCT(100));
    lv_obj_set_height(g_mini_time, 24);
    lv_obj_set_style_text_align(g_mini_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(g_mini_time, LV_LABEL_LONG_MODE_DOTS);

    /* Three status icons with a 23 px visual gap between them. */
    lv_obj_t * modes = lv_obj_create(g_mini);
    lv_obj_set_size(modes, LV_SIZE_CONTENT, MINI_NOW_H);
    lv_obj_set_style_bg_opa(modes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modes, 0, 0);
    lv_obj_set_style_radius(modes, 0, 0);
    lv_obj_set_style_pad_all(modes, 0, 0);
    lv_obj_set_style_pad_column(modes, 23, 0);
    lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modes, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(modes, false);

    g_mini_shuffle = make_icon(modes, ICON_SHUFFLE, COL_MUTED);
    g_mini_repeat = make_icon(modes, ICON_REPEAT, COL_MUTED);
    g_mini_star = make_icon(modes, ICON_STAR, COL_MINI_MUTED);
}

static void ui_layout_page_area(void)
{
    bool show_mini = (g_current_screen != SCR_NOW_PLAYING);
    lv_obj_set_hidden(g_mini, !show_mini);
    int top = show_mini ? MINI_NOW_H : 0;
    lv_obj_set_pos(g_page, 0, top);
    lv_obj_set_size(g_page, LV_PCT(100), SCREEN_H - top);
    if(show_mini) ui_mini_update();
}

static void build_menubar(lv_obj_t * parent)
{
    g_menubar = lv_obj_create(parent);
    lv_obj_set_size(g_menubar, MENUBAR_W, SCREEN_H);
    lv_obj_set_pos(g_menubar, 0, 0);
    lv_obj_set_style_bg_color(g_menubar, lv_color_hex(COL_MENU_BODY_BG), 0);
    lv_obj_set_style_bg_opa(g_menubar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_menubar, 0, 0);
    lv_obj_set_style_border_width(g_menubar, 0, 0);
    lv_obj_set_style_pad_all(g_menubar, 0, 0);
    lv_obj_set_style_pad_row(g_menubar, 0, 0);
    lv_obj_set_flex_flow(g_menubar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_menubar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(g_menubar, false);

    /* Time sits above the menu icons, in a 72 px strip. */
    lv_obj_t * time_area = lv_obj_create(g_menubar);
    lv_obj_set_size(time_area, LV_PCT(100), MENUBAR_TIME_H);
    lv_obj_set_style_pad_hor(time_area, 6, 0);
    lv_obj_set_style_bg_color(time_area, lv_color_hex(COL_MENU_TOP_BG), 0);
    lv_obj_set_style_bg_opa(time_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(time_area, 0, 0);
    lv_obj_set_style_border_width(time_area, 0, 0);
    lv_obj_set_style_pad_all(time_area, 0, 0);
    lv_obj_set_flex_flow(time_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(time_area, false);
    g_status_time = make_label(time_area, "--:--", FONT_CLOCK,
                               COL_MENU_TOP_FG);
    lv_obj_set_width(g_status_time, LV_PCT(100));
    lv_obj_set_style_text_align(g_status_time, LV_TEXT_ALIGN_CENTER, 0);

    /* Menu icons sit in the flexible middle area. */
    lv_obj_t * menu_area = lv_obj_create(g_menubar);
    lv_obj_set_width(menu_area, LV_PCT(100));
    lv_obj_set_flex_grow(menu_area, 1);
    lv_obj_set_style_bg_opa(menu_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_area, 0, 0);
    lv_obj_set_style_radius(menu_area, 0, 0);
    lv_obj_set_style_pad_all(menu_area, 0, 0);
    lv_obj_set_style_pad_row(menu_area, 0, 0);
    lv_obj_set_flex_flow(menu_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(menu_area, false);

    static const int menu_order[6] = {
        SCR_NOW_PLAYING, SCR_ALBUMS, SCR_ARTISTS, SCR_SONGS,
        SCR_PLAYLISTS, SCR_SETTINGS,
    };
    static const char * const icons[6] = {
        ICON_PLAY,
        ICON_DISC,
        ICON_PERSON,
        ICON_MUSIC_LIST,
        ICON_LIST,
        ICON_SETTINGS,
    };

    for(int slot = 0; slot < 6; slot++) {
        int screen = menu_order[slot];
        lv_obj_t * item = lv_obj_create(menu_area);
        lv_obj_set_size(item, MENUBAR_W, 80);
        lv_obj_set_style_radius(item, 0, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(item, false);
        lv_obj_set_clickable(item, true);
        lv_obj_add_event_cb(item, menu_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)screen);

        g_menu_items[screen] = item;
        g_menu_icons[screen] = make_label(item, icons[slot],
                                          &bootstrap_icons_40, COL_MUTED);
    }

    /* iOS device battery sits below the menu icons. */
    lv_obj_t * battery_area = lv_obj_create(g_menubar);
    lv_obj_set_size(battery_area, LV_PCT(100), MENUBAR_BATTERY_H);
    lv_obj_set_style_bg_color(battery_area, lv_color_hex(COL_MENU_BATT_BG), 0);
    lv_obj_set_style_bg_opa(battery_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_area, 0, 0);
    lv_obj_set_style_radius(battery_area, 0, 0);
    lv_obj_set_style_pad_all(battery_area, 0, 0);
    lv_obj_set_style_pad_all(battery_area, 0, 0);
    lv_obj_set_flex_flow(battery_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(battery_area, false);

    g_status_battery = make_label(battery_area, ICON_BATTERY_FULL,
                                  &bootstrap_icons_32, COL_MENU_BODY_FG);

}


static void ui_menubar_apply_side(void)
{
    int x = g_menubar_left ? 0 : SCREEN_W - MENUBAR_W;
    int content_x = g_menubar_left ? MENUBAR_W : 0;

    lv_obj_set_pos(g_menubar, x, 0);
    lv_obj_set_pos(g_content, content_x, CONTENT_Y);
    lv_obj_set_size(g_content, CONTENT_W, CONTENT_H);
    ui_menu_update();
}

void ui_set_menubar_left(bool left)
{
    if(g_menubar_left == left) return;
    g_menubar_left = left;
    if(g_menubar) ui_menubar_apply_side();
}

bool ui_menubar_left(void)
{
    return g_menubar_left;
}

void ui_set_system_config(const ui_system_config_t * cfg)
{
    if(cfg == NULL) return;

    char status[sizeof(g_system_config.wifi_status)];
    char ip[sizeof(g_system_config.wifi_ip)];
    snprintf(status, sizeof(status), "%s", g_system_config.wifi_status);
    snprintf(ip, sizeof(ip), "%s", g_system_config.wifi_ip);

    g_system_config = *cfg;

    if(cfg->wifi_status[0] == '\0') {
        snprintf(g_system_config.wifi_status, sizeof(g_system_config.wifi_status),
                 "%s", status);
    }
    if(cfg->wifi_ip[0] == '\0') {
        snprintf(g_system_config.wifi_ip, sizeof(g_system_config.wifi_ip), "%s", ip);
    }
}

void ui_set_system_config_changed_cb(void (*cb)(const ui_system_config_t * cfg))
{
    g_system_config_changed_cb = cb;
}

void ui_set_network_status(const char * status, const char * ip)
{
    if(status != NULL) {
        snprintf(g_system_config.wifi_status, sizeof(g_system_config.wifi_status),
                 "%s", status);
    }
    if(ip != NULL) {
        snprintf(g_system_config.wifi_ip, sizeof(g_system_config.wifi_ip), "%s", ip);
    }
    settings_update_system_labels();
}

void ui_set_feedback_cb(void (*cb)(int type))
{
    g_feedback_cb = cb;
}

void ui_set_afc_status_cb(void (*cb)(ui_afc_status_t * out))
{
    g_afc_status_cb = cb;
}

void ui_set_afc_action_cb(void (*cb)(int action))
{
    g_afc_action_cb = cb;
}

void ui_set_afc_sync_mode(bool enabled)
{
    g_afc_sync_mode = enabled;
}

void ui_system_toggle_screen_flip(void)
{
    g_system_config.screen_flip = g_system_config.screen_flip == 180 ? 0 : 180;
    if(g_system_config_changed_cb != NULL) {
        g_system_config_changed_cb(&g_system_config);
    }
}

void ui_debug_page(int direction)
{
    ui_list_page(direction >= 0 ? 1 : -1);
}

void ui_debug_trace(bool enabled)
{
    g_ui_trace = enabled;
    UI_PERF_LOG("trace %s", enabled ? "on" : "off");
}

void ui_debug_scroll(int dy)
{
    if(g_current_screen == SCR_ALBUMS && albums_left_list != NULL) {
        lv_obj_scroll_by(albums_left_list, 0, dy, LV_ANIM_OFF);
    }
    else if(g_current_screen == SCR_SONGS && songs_list != NULL) {
        lv_obj_scroll_by(songs_list, 0, dy, LV_ANIM_OFF);
    }
}

static void update_clock(void)
{
    static char last[16] = "";
    static bool last_12h;
    time_t now = time(NULL) + g_clock_offset_min * 60;
    struct tm local;
    localtime_r(&now, &local);
    char text[16];
    if(g_clock_12h) {
        strftime(text, sizeof(text), "%I:%M %p", &local);
        if(text[0] == '0' && text[1] != '\0') {
            memmove(text, text + 1, strlen(text));
        }
    }
    else {
        strftime(text, sizeof(text), "%H:%M", &local);
    }
    if(strcmp(text, last) != 0 || g_clock_12h != last_12h) {
        last_12h = g_clock_12h;
        snprintf(last, sizeof(last), "%s", text);
        lv_obj_set_style_text_font(g_status_time,
                                   g_clock_12h ? FONT_CLOCK_12
                                               : FONT_CLOCK, 0);
        lv_label_set_text(g_status_time, text);
    }
}

static void update_status(void)
{
    static int last_level = -999;
    static bool last_charging = false;

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    if(snapshot.battery != last_level || snapshot.charging != last_charging) {
        last_level = snapshot.battery;
        last_charging = snapshot.charging;
        lv_label_set_text(g_status_battery,
                          battery_icon_for(snapshot.battery, snapshot.charging));
        lv_obj_set_style_text_color(g_status_battery,
                                    lv_color_hex(battery_color_for(snapshot.battery,
                                                                   snapshot.charging)), 0);
    }
}

static void settings_time_blink_tick(void);

static void ui_tick_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    static uint32_t last_second;
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    uint32_t perf_t1 = 0, perf_t2 = 0, perf_t3 = 0, perf_t4 = 0;
    uint32_t perf_t5 = 0, perf_t6 = 0;

    update_clock();
    if(g_ui_trace) perf_t1 = perf_now();
    update_status();
    if(g_ui_trace) perf_t2 = perf_now();
    ui_menu_play_update();
    if(g_ui_trace) perf_t3 = perf_now();

    bool device_available = player_device_available();
    if(device_available != g_device_available) {
        g_device_available = device_available;
        if(!g_afc_sync_mode) {
            ui_rebuild_page();
            return;
        }
    }
    if(g_ui_trace) perf_t4 = perf_now();

    uint32_t now = lv_tick_get();
    if(now - last_second >= 1000) {
        last_second = now;
        player_tick();
    }
    if(g_ui_trace) perf_t5 = perf_now();

    if(g_current_screen < SCR_COUNT && g_pages[g_current_screen].tick) {
        g_pages[g_current_screen].tick();
    }
    if(g_ui_trace) perf_t6 = perf_now();

    if(g_current_screen != SCR_NOW_PLAYING) ui_mini_update();
    if(g_ui_trace) {
        uint32_t perf_t7 = perf_now();
        uint32_t dt = perf_t7 - perf_t0;
        if(dt > 8000) {
            UI_PERF_LOG("tick slow: %u us (clock=%u status=%u menu=%u dev=%u ptick=%u page=%u mini=%u)",
                         (unsigned)dt,
                         (unsigned)(perf_t1 - perf_t0),
                         (unsigned)(perf_t2 - perf_t1),
                         (unsigned)(perf_t3 - perf_t2),
                         (unsigned)(perf_t4 - perf_t3),
                         (unsigned)(perf_t5 - perf_t4),
                         (unsigned)(perf_t6 - perf_t5),
                         (unsigned)(perf_t7 - perf_t6));
        }
    }
}

static void settings_time_blink_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    settings_time_blink_tick();
}

static void ui_show_screen(screen_id_t id)
{
    if(g_afc_sync_mode) return;
    if(id < 0 || id >= SCR_COUNT) return;

    if(g_ctx_menu_open) context_menu_close();

    if(id == g_current_screen) {
        if(g_pages[id].same_key) g_pages[id].same_key();
        return;
    }

    if(id != SCR_BLUETOOTH) g_ble_return_screen = SCR_COUNT;
    g_current_screen = id;
    ui_exit_edit();
    focus_clear();
    lv_obj_clean(g_page);
    ui_set_hint("");
    ui_layout_page_area();
    uint32_t trace_start = lv_tick_get();
    g_pages[id].create(g_page);
    if(g_ui_trace || getenv("IAP2TAB5_TRACE")) {
        fprintf(stderr, "[trace] create screen %d in %u ms\n", (int)id,
                (unsigned)(lv_tick_get() - trace_start));
    }
    ui_menu_update();
}

static void ui_rebuild_page(void)
{
    ui_exit_edit();
    focus_clear();
    lv_obj_clean(g_page);
    ui_set_hint("");
    uint32_t trace_start = lv_tick_get();
    g_pages[g_current_screen].create(g_page);
    if(g_ui_trace || getenv("IAP2TAB5_TRACE")) {
        fprintf(stderr, "[trace] rebuild screen %d in %u ms\n",
                (int)g_current_screen, (unsigned)(lv_tick_get() - trace_start));
    }
    ui_menu_update();
}

static void ui_back(void)
{
    if(g_edit_mode) {
        ui_exit_edit();
        return;
    }
    ui_feedback_emit(UI_FEEDBACK_BACK);
    if(g_pages[g_current_screen].back && g_pages[g_current_screen].back()) {
        return;
    }
    screen_id_t fallback = SCR_NOW_PLAYING;
    if(g_current_screen == SCR_BLUETOOTH && g_ble_return_screen != SCR_COUNT) {
        fallback = g_ble_return_screen;
    }
    if(g_current_screen != fallback) {
        ui_show_screen(fallback);
    }
}

void ui_attach_touch(lv_indev_t * indev)
{
    g_touch_indev = indev;
    if(g_touch_indev) lv_indev_enable(g_touch_indev, g_touch_enabled);
}

void ui_set_touch_enabled(bool enabled)
{
    g_touch_enabled = enabled;
    if(g_touch_indev) lv_indev_enable(g_touch_indev, enabled);
}

bool ui_touch_enabled(void)
{
    return g_touch_enabled;
}

static void ui_handle_media_key(ui_key_t key)
{
    switch(key) {
        case UI_KEY_7:
            player_play_pause();
            break;
        case UI_KEY_PREV_TRACK:
            player_previous();
            break;
        case UI_KEY_NEXT_TRACK:
            player_next();
            break;
        case UI_KEY_VOLUME_UP:
        case UI_KEY_VOLUME_DOWN: {
            player_snapshot_t snap;
            player_get_snapshot(&snap);
            int v = snap.volume +
                    (key == UI_KEY_VOLUME_UP ? 5 : -5);
            if(v < 0) v = 0;
            if(v > 100) v = 100;
            player_set_volume(v);
            break;
        }
        case UI_KEY_MUTE:
            player_mute_toggle();
            break;
        case UI_KEY_STAR:
            player_toggle_star();
            break;
        default:
            return;
    }
    if(g_current_screen < SCR_COUNT && g_pages[g_current_screen].tick) {
        g_pages[g_current_screen].tick();
    }
}

void ui_handle_key(ui_key_t key)
{
    if(g_afc_sync_mode) {
        switch(key) {
            case UI_KEY_UP:
                focus_move(-1);
                break;
            case UI_KEY_DOWN:
                focus_move(1);
                break;
            case UI_KEY_ENTER:
                focus_activate();
                break;
            case UI_KEY_BACK:
            case UI_KEY_6:
                if(g_afc_action_cb != NULL) {
                    g_afc_action_cb(UI_AFC_ACTION_IPOD_MODE);
                }
                break;
            default:
                break;
        }
        return;
    }

    if(g_ctx_menu_open) {
        /* Media keys are global hardware keys and stay available while the
         * menu is open. Everything else is modal. */
        if(key == UI_KEY_7 || key == UI_KEY_PREV_TRACK ||
           key == UI_KEY_NEXT_TRACK || key == UI_KEY_VOLUME_UP ||
           key == UI_KEY_VOLUME_DOWN || key == UI_KEY_MUTE ||
           key == UI_KEY_STAR) {
            ui_handle_media_key(key);
        }
        else {
            context_menu_key(key);
        }
        return;
    }
    if(key == UI_KEY_CONTEXT) {
        /* The Bluetooth page uses CONTEXT to unpair the focused device. */
        if(g_current_screen < SCR_COUNT && g_pages[g_current_screen].key &&
           g_pages[g_current_screen].key(key)) {
            return;
        }
        context_menu_open();
        return;
    }

    if(g_current_screen < SCR_COUNT && g_pages[g_current_screen].key &&
       g_pages[g_current_screen].key(key)) {
        return;
    }

    switch(key) {
        case UI_KEY_UP:
            focus_move(-1);
            break;
        case UI_KEY_DOWN:
            focus_move(1);
            break;
        case UI_KEY_ENTER:
            focus_activate();
            break;
        case UI_KEY_1:
        case UI_KEY_2:
        case UI_KEY_3:
        case UI_KEY_4:
        case UI_KEY_5:
            ui_show_screen((screen_id_t)(key - UI_KEY_1));
            break;
        case UI_KEY_6:
            ui_back();
            break;
        case UI_KEY_7:
        case UI_KEY_PREV_TRACK:
        case UI_KEY_NEXT_TRACK:
        case UI_KEY_VOLUME_UP:
        case UI_KEY_VOLUME_DOWN:
        case UI_KEY_MUTE:
        case UI_KEY_STAR:
            ui_handle_media_key(key);
            break;
        case UI_KEY_BACK:
            ui_back();
            break;
        case UI_KEY_PAGE_UP:
            /* Page Up/Down belong to lists now. On Now Playing, Up still
             * opens the queue because there is no list to page. */
            if(ui_list_page(-1)) break;
            if(g_current_screen == SCR_NOW_PLAYING) {
                np_toggle_queue();
            }
            break;
        case UI_KEY_PAGE_DOWN:
            ui_list_page(1);
            break;
        case UI_KEY_SCREEN_PREV: {
            int target = (int)g_current_screen - 1;
            if(target == SCR_BLUETOOTH) target--;
            if(target >= 0) ui_show_screen((screen_id_t)target);
            break;
        }
        case UI_KEY_SCREEN_NEXT: {
            int target = (int)g_current_screen + 1;
            if(target == SCR_BLUETOOTH) target++;
            if(target < SCR_COUNT) ui_show_screen((screen_id_t)target);
            break;
        }
        default:
            break;
    }
}

void ui_select_screen(int index)
{
    if(g_afc_sync_mode) return;
    if(index < 0 || index >= SCR_COUNT) return;
    ui_show_screen((screen_id_t)index);
}

/* -------------------------------------------------------------------------
 * Now Playing
 * ------------------------------------------------------------------------- */
static lv_obj_t * np_title;
static lv_obj_t * np_meta;
static lv_obj_t * np_time_cur;
static lv_obj_t * np_time_dur;
static lv_obj_t * np_progress;
static lv_obj_t * np_play_icon;
static lv_obj_t * np_shuffle_btn;
static lv_obj_t * np_repeat_btn;
static lv_obj_t * np_shuffle_icon;
static lv_obj_t * np_repeat_icon;
static lv_obj_t * np_star_icon;
static lv_obj_t * np_cover_widget;
static uint32_t g_np_cover_rev;
static uint64_t g_np_cover_track_id;
static uint64_t g_np_cover_album_id;
#define NP_QUEUE_ROW_H        54
#define NP_QUEUE_VISIBLE_ROWS  11
#define NP_QUEUE_LIST_H       (NP_QUEUE_VISIBLE_ROWS * NP_QUEUE_ROW_H + \
                              (NP_QUEUE_VISIBLE_ROWS - 1) * LIST_ROW_GAP)

/* Golden-ratio Now Playing layout: the info column is 1.618x the cover
 * width, and the two main vertical gaps are distributed around 1.618:1. */
#define NP_COVER_SIZE         360
#define NP_INFO_W             582
#define NP_TOP_W             (NP_COVER_SIZE + 20 + NP_INFO_W)
#define NP_GAP_PROGRESS       48
#define NP_GAP_MODES          30
#define NP_BOTTOM_SPACER      60

static lv_obj_t * np_queue_list;
static lv_obj_t * np_queue_count;
static bool np_queue_visible = false;
static int np_queue_focus_start;
static uint64_t np_last_track;
static int np_last_queue_count = -1;

typedef enum {
    NP_BG_THEME = 0,
    NP_BG_BLURRED_COVER,
    NP_BG_COVER_GRADIENT,
} np_bg_mode_t;

static np_bg_mode_t g_np_bg_mode = NP_BG_THEME;

static void np_prev_action(void * user)
{
    LV_UNUSED(user);
    player_previous();
    if(g_pages[SCR_NOW_PLAYING].tick) g_pages[SCR_NOW_PLAYING].tick();
}

static void np_play_action(void * user)
{
    LV_UNUSED(user);
    player_play_pause();
    if(g_pages[SCR_NOW_PLAYING].tick) g_pages[SCR_NOW_PLAYING].tick();
}

static void np_next_action(void * user)
{
    LV_UNUSED(user);
    player_next();
    if(g_pages[SCR_NOW_PLAYING].tick) g_pages[SCR_NOW_PLAYING].tick();
}

static void np_shuffle_action(void * user)
{
    LV_UNUSED(user);
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    player_set_shuffle(!snapshot.shuffle);
    if(g_pages[SCR_NOW_PLAYING].tick) g_pages[SCR_NOW_PLAYING].tick();
}

static void np_repeat_action(void * user)
{
    LV_UNUSED(user);
    player_cycle_repeat();
    if(g_pages[SCR_NOW_PLAYING].tick) g_pages[SCR_NOW_PLAYING].tick();
}

static void np_prev_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    np_prev_action(NULL);
}

static void np_play_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    np_play_action(NULL);
}

static void np_next_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    np_next_action(NULL);
}

static void np_shuffle_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    np_shuffle_action(NULL);
}

static void np_repeat_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    np_repeat_action(NULL);
}

static void np_refresh_ui(void);

static void np_star_click_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    player_toggle_star();
    np_refresh_ui();
}

static void np_queue_action(void * user)
{
    int index = (int)(intptr_t)user;
    player_queue_jump(index);
    ui_rebuild_page();
    if(g_focus_count > 0) focus_select(np_queue_focus_start);
}

static void np_refresh_ui(void)
{
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    bool has_track = snapshot.track.title[0] != '\0';
    lv_label_set_text(np_title, has_track ? snapshot.track.title : "No track");
    char meta_text[192] = "";
    if(has_track) {
        const char * artist = snapshot.track.artist;
        const char * album = snapshot.track.album;
        if(artist[0] != '\0' && album[0] != '\0') {
            snprintf(meta_text, sizeof(meta_text), "%s - %s", artist, album);
        }
        else if(artist[0] != '\0') {
            snprintf(meta_text, sizeof(meta_text), "%s", artist);
        }
        else if(album[0] != '\0') {
            snprintf(meta_text, sizeof(meta_text), "%s", album);
        }
    }
    if(meta_text[0] != '\0') {
        lv_label_set_text(np_meta, meta_text);
        lv_obj_set_hidden(np_meta, false);
    }
    else {
        lv_label_set_text(np_meta, "");
        lv_obj_set_hidden(np_meta, true);
    }

    /* iAP2 artwork arrives after the metadata.  If the album is already in
     * the AFC library, show its cached .cov immediately; when the live iAP2
     * image arrives, replace it.  A track change to another album must also
     * invalidate the previous cover even if the art revision has not moved. */
    if(snapshot.real_device && np_cover_widget != NULL) {
        uint32_t art_rev = player_artwork_revision();
        bool track_changed = (snapshot.track.id != g_np_cover_track_id);
        if(art_rev != g_np_cover_rev || track_changed) {
            g_np_cover_rev = art_rev;
            g_np_cover_track_id = snapshot.track.id;
            player_album_t album;
            uint64_t album_id = 0;
            bool has_album = np_resolve_current_album(&snapshot, &album,
                                                       &album_id);
            g_np_cover_album_id = album_id;
            const void * src = iap2_cover_src();
            bool from_cache = false;
            if(src == NULL && has_album) {
                src = album_cover_src(&album);
                from_cache = src != NULL;
            }
            lv_image_set_src(np_cover_widget, src);
            lv_obj_set_style_bg_color(np_cover_widget,
                lv_color_hex(src ? (from_cache ? album.color : COL_PANEL)
                                 : COL_PANEL_ALT), 0);
            lv_obj_set_style_bg_opa(np_cover_widget,
                                    src ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
            UI_PERF_LOG("np_art update track=%016llx album=%llu iap2=%d cache=%d",
                        (unsigned long long)snapshot.track.id,
                        (unsigned long long)album_id,
                        src != NULL && !from_cache, from_cache);
        }
    }

    char current[16];
    char total[16];
    format_time(snapshot.elapsed_sec, current, sizeof(current));
    format_time(snapshot.duration_sec, total, sizeof(total));
    lv_label_set_text(np_time_cur, current);
    lv_label_set_text(np_time_dur, total);

    lv_bar_set_range(np_progress, 0, snapshot.duration_sec ? (int32_t)snapshot.duration_sec : 1);
    lv_bar_set_value(np_progress, (int32_t)snapshot.elapsed_sec, LV_ANIM_OFF);

    lv_label_set_text(np_play_icon,
                      snapshot.status == PLAYER_PLAYING ? ICON_PAUSE : ICON_PLAY);
    lv_obj_set_style_text_color(np_shuffle_icon,
                                lv_color_hex(snapshot.shuffle ? COL_ACCENT : COL_MUTED), 0);
    lv_obj_set_style_text_color(np_repeat_icon,
                                lv_color_hex(snapshot.repeat != PLAYER_REPEAT_OFF
                                             ? COL_ACCENT : COL_MUTED), 0);
    lv_obj_set_style_bg_color(np_shuffle_btn, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_bg_opa(np_shuffle_btn,
                            snapshot.shuffle ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(np_repeat_btn, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_bg_opa(np_repeat_btn,
                            snapshot.repeat != PLAYER_REPEAT_OFF
                            ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_label_set_text(np_repeat_icon,
                      snapshot.repeat == PLAYER_REPEAT_ONE ? ICON_REPEAT_ONE : ICON_REPEAT);
    lv_label_set_text(np_star_icon,
                      snapshot.starred ? ICON_STAR_FILL : ICON_STAR);
    lv_obj_set_style_text_color(np_star_icon,
                                lv_color_hex(snapshot.starred ? COL_ACCENT : COL_MUTED), 0);

    if(np_queue_count) {
        int index = player_queue_index();
        int count = player_queue_count();
        if(index >= 0 && count > 0) {
            lv_label_set_text_fmt(np_queue_count, "%d/%d", index + 1, count);
        }
        else {
            lv_label_set_text(np_queue_count, "0/0");
        }
    }
}

static void np_tick(void)
{
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    bool track_changed = (snapshot.track.id != np_last_track);
    bool queue_changed = (player_queue_count() != np_last_queue_count);
    np_last_track = snapshot.track.id;
    np_last_queue_count = player_queue_count();

    if(track_changed || queue_changed) {
        ui_rebuild_page();
        return;
    }
    np_refresh_ui();
}

static void np_same_key(void)
{
    if(g_focus_count > 0 && np_queue_focus_start < g_focus_count) {
        focus_select(np_queue_focus_start);
    }
    ui_set_hint("UP/DOWN Select   ENTER Play/Jump   7 Play/Pause");
}

static bool np_back(void)
{
    if(np_queue_visible && g_focus_count > 0) focus_select(0);
    return true;
}

static void np_queue_row(lv_obj_t * parent, int queue_index, bool current)
{
    player_track_t track;
    if(!player_queue_at(queue_index, &track)) return;

    lv_obj_t * row = make_row(parent, NP_QUEUE_ROW_H);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t * marker = make_label(row, "", FONT_NUM, COL_MUTED);
    lv_obj_set_width(marker, 30);
    if(current) {
        lv_label_set_text(marker, ICON_PLAY);
        lv_obj_set_style_text_font(marker, &bootstrap_icons_24, 0);
        lv_obj_set_style_text_color(marker, lv_color_hex(COL_ACCENT), 0);
    }
    else {
        char number[16];
        snprintf(number, sizeof(number), "%02d", queue_index + 1);
        lv_label_set_text(marker, number);
    }

    lv_obj_t * text = lv_obj_create(row);
    lv_obj_set_height(text, 46);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text, 0, 0);
    lv_obj_set_style_pad_all(text, 0, 0);
    lv_obj_set_style_pad_row(text, 0, 0);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(text, false);

    lv_obj_t * title = make_label(text, track.title, FONT_QUEUE,
                                  current ? COL_ACCENT : COL_TEXT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_height(title, 26);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t * artist = make_label(text, track.artist, FONT_QUEUE_SUB, COL_MUTED);
    lv_obj_set_width(artist, LV_PCT(100));
    lv_obj_set_height(artist, 20);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);

    char duration[16];
    format_time(track.duration_sec, duration, sizeof(duration));
    lv_obj_t * length = make_label(row, duration, FONT_NUM, COL_MUTED);
    lv_obj_set_width(length, 52);
    lv_obj_set_style_text_align(length, LV_TEXT_ALIGN_RIGHT, 0);

    focus_add_clickable(row, np_queue_action, NULL, (void *)(intptr_t)queue_index, true);
}

static bool find_album(uint64_t album_id, player_album_t * out)
{
    for(int i = 0; i < player_album_count(); i++) {
        if(player_album_get(i, out) && out->id == album_id) return true;
    }
    return false;
}

/* Resolve the local album for the current iAP2 track.  The normal path is
 * the album id computed from the iAP2 album/artist tags.  If that fails, use
 * the iAP2 track id (or title+album metadata) to find the local track and
 * take its album id.  This is only called when the track or artwork changes. */
static bool np_resolve_current_album(const player_snapshot_t * snapshot,
                                     player_album_t * album,
                                     uint64_t * album_id)
{
    if(snapshot == NULL || album == NULL) return false;
    if(album_id) *album_id = 0;

    uint64_t id = snapshot->track.album_id;
    if(id != 0 && find_album(id, album)) {
        if(album_id) *album_id = id;
        return true;
    }

    if(player_album_for_track(snapshot->track.id, snapshot->track.title,
                              snapshot->track.album, &id) &&
       find_album(id, album)) {
        if(album_id) *album_id = id;
        return true;
    }
    return false;
}

#define NP_SAMPLE_SIZE       32
#define NP_PALETTE_COLORS     5
#define NP_MESH_W           1280
#define NP_MESH_H           720

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t luma;
} np_sample_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} np_rgb_t;

static np_sample_t np_samples[NP_SAMPLE_SIZE * NP_SAMPLE_SIZE];
static np_rgb_t np_palette[NP_PALETTE_COLORS];

static lv_image_dsc_t g_np_mesh_dsc;
static uint8_t * g_np_mesh_buf;
static char g_np_mesh_cover[192];

static int np_sample_cmp(const void * a, const void * b)
{
    const np_sample_t * sa = a;
    const np_sample_t * sb = b;
    return (int)sa->luma - (int)sb->luma;
}

static bool np_extract_palette(const char * cover_path)
{
    lv_image_header_t header;
    if(lv_image_decoder_get_info(cover_path, &header) != LV_RESULT_OK) return false;

    uint32_t buf_size = lv_canvas_buf_size(NP_SAMPLE_SIZE, NP_SAMPLE_SIZE,
                                           32, LV_DRAW_BUF_STRIDE_ALIGN);
    uint8_t * sample_buf = lv_malloc(buf_size);
    if(sample_buf == NULL) return false;

    lv_obj_t * canvas = lv_canvas_create(lv_screen_active());
    lv_obj_set_size(canvas, NP_SAMPLE_SIZE, NP_SAMPLE_SIZE);
    lv_obj_set_hidden(canvas, true);
    lv_canvas_set_buffer(canvas, sample_buf, NP_SAMPLE_SIZE, NP_SAMPLE_SIZE,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_image_dsc_t draw_dsc;
    lv_draw_image_dsc_init(&draw_dsc);
    draw_dsc.base.layer = &layer;
    draw_dsc.src = cover_path;
    draw_dsc.scale_x = (NP_SAMPLE_SIZE * LV_SCALE_NONE) / header.w;
    draw_dsc.scale_y = (NP_SAMPLE_SIZE * LV_SCALE_NONE) / header.h;
    draw_dsc.pivot.x = 0;
    draw_dsc.pivot.y = 0;
    draw_dsc.antialias = 1;
    lv_area_t image_area = {0, 0, header.w - 1, header.h - 1};
    lv_draw_image(&layer, &draw_dsc, &image_area);
    lv_canvas_finish_layer(canvas, &layer);

    const lv_image_dsc_t * canvas_img = lv_canvas_get_image(canvas);
    const uint8_t * pixels = canvas_img->data;
    uint32_t stride = canvas_img->header.stride;

    int count = 0;
    for(int y = 0; y < NP_SAMPLE_SIZE; y++) {
        const lv_color32_t * row = (const lv_color32_t *)(pixels + y * stride);
        for(int x = 0; x < NP_SAMPLE_SIZE; x++) {
            lv_color32_t c = row[x];
            if(c.alpha < 200) continue;
            uint8_t luma = (uint8_t)((77 * c.red + 150 * c.green + 29 * c.blue) >> 8);
            if(luma < 20 || luma > 235) continue;
            np_samples[count].r = c.red;
            np_samples[count].g = c.green;
            np_samples[count].b = c.blue;
            np_samples[count].luma = luma;
            count++;
        }
    }

    lv_obj_delete(canvas);
    lv_free(sample_buf);

    if(count < NP_PALETTE_COLORS * 3) return false;

    qsort(np_samples, count, sizeof(np_sample_t), np_sample_cmp);

    int centers[NP_PALETTE_COLORS][3];
    for(int k = 0; k < NP_PALETTE_COLORS; k++) {
        int index = (count - 1) * k / (NP_PALETTE_COLORS - 1);
        centers[k][0] = np_samples[index].r;
        centers[k][1] = np_samples[index].g;
        centers[k][2] = np_samples[index].b;
    }

    for(int iteration = 0; iteration < 10; iteration++) {
        uint32_t sum_r[NP_PALETTE_COLORS] = {0};
        uint32_t sum_g[NP_PALETTE_COLORS] = {0};
        uint32_t sum_b[NP_PALETTE_COLORS] = {0};
        uint32_t used[NP_PALETTE_COLORS] = {0};

        for(int i = 0; i < count; i++) {
            int best = 0;
            int32_t best_dist = INT32_MAX;
            for(int k = 0; k < NP_PALETTE_COLORS; k++) {
                int dr = (int)np_samples[i].r - centers[k][0];
                int dg = (int)np_samples[i].g - centers[k][1];
                int db = (int)np_samples[i].b - centers[k][2];
                int32_t dist = dr * dr + dg * dg + db * db;
                if(dist < best_dist) {
                    best_dist = dist;
                    best = k;
                }
            }
            sum_r[best] += np_samples[i].r;
            sum_g[best] += np_samples[i].g;
            sum_b[best] += np_samples[i].b;
            used[best]++;
        }

        bool changed = false;
        for(int k = 0; k < NP_PALETTE_COLORS; k++) {
            if(used[k] == 0) continue;
            int r = (int)(sum_r[k] / used[k]);
            int g = (int)(sum_g[k] / used[k]);
            int b = (int)(sum_b[k] / used[k]);
            if(r != centers[k][0] || g != centers[k][1] || b != centers[k][2]) {
                centers[k][0] = r;
                centers[k][1] = g;
                centers[k][2] = b;
                changed = true;
            }
        }
        if(!changed) break;
    }

    for(int i = 0; i < NP_PALETTE_COLORS - 1; i++) {
        int best = i;
        int best_luma = centers[i][0] * 77 + centers[i][1] * 150 + centers[i][2] * 29;
        for(int j = i + 1; j < NP_PALETTE_COLORS; j++) {
            int luma = centers[j][0] * 77 + centers[j][1] * 150 + centers[j][2] * 29;
            if(luma < best_luma) {
                best = j;
                best_luma = luma;
            }
        }
        if(best != i) {
            for(int c = 0; c < 3; c++) {
                int tmp = centers[i][c];
                centers[i][c] = centers[best][c];
                centers[best][c] = tmp;
            }
        }
    }

    for(int k = 0; k < NP_PALETTE_COLORS; k++) {
        np_palette[k].r = (uint8_t)centers[k][0];
        np_palette[k].g = (uint8_t)centers[k][1];
        np_palette[k].b = (uint8_t)centers[k][2];
    }
    return true;
}

static void np_generate_mesh(uint16_t * out, uint32_t stride_px)
{
    /* 3x3 control grid, using the 5 palette colors in a balanced layout.
     * The grid is evaluated as a quadratic Bezier patch. */
    static const uint8_t grid[3][3] = {
        {0, 3, 1},
        {2, 4, 3},
        {1, 2, 0},
    };

    for(int y = 0; y < NP_MESH_H; y++) {
        float ty = (float)y / (NP_MESH_H - 1);
        float by0 = (1.0f - ty) * (1.0f - ty);
        float by1 = 2.0f * ty * (1.0f - ty);
        float by2 = ty * ty;

        for(int x = 0; x < NP_MESH_W; x++) {
            float tx = (float)x / (NP_MESH_W - 1);
            float bx0 = (1.0f - tx) * (1.0f - tx);
            float bx1 = 2.0f * tx * (1.0f - tx);
            float bx2 = tx * tx;
            float wx[3] = {bx0, bx1, bx2};
            float wy[3] = {by0, by1, by2};

            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            for(int i = 0; i < 3; i++) {
                for(int j = 0; j < 3; j++) {
                    float w = wy[i] * wx[j];
                    const np_rgb_t * c = &np_palette[grid[i][j]];
                    r += c->r * w;
                    g += c->g * w;
                    b += c->b * w;
                }
            }

            if(r < 0.0f) r = 0.0f;
            if(g < 0.0f) g = 0.0f;
            if(b < 0.0f) b = 0.0f;
            if(r > 255.0f) r = 255.0f;
            if(g > 255.0f) g = 255.0f;
            if(b > 255.0f) b = 255.0f;
            out[y * stride_px + x] =
                lv_color_to_u16(lv_color_make((uint8_t)(r + 0.5f),
                                              (uint8_t)(g + 0.5f),
                                              (uint8_t)(b + 0.5f)));
        }
    }
}

static bool np_mesh_prepare(const char * cover_path)
{
    if(g_np_mesh_buf != NULL && strcmp(g_np_mesh_cover, cover_path) == 0) {
        return true;
    }

    if(g_np_mesh_buf != NULL) {
        lv_free(g_np_mesh_buf);
        g_np_mesh_buf = NULL;
    }
    g_np_mesh_cover[0] = '\0';

    if(!np_extract_palette(cover_path)) return false;

    /* Full-resolution mesh (1280x720 RGB565 = 1.8 MB) so the backdrop is
     * drawn 1:1 instead of being rescaled on every refresh. */
    uint32_t mesh_stride = lv_draw_buf_width_to_stride(NP_MESH_W,
                                                       LV_COLOR_FORMAT_RGB565);
    uint8_t * mesh_buf = ui_alloc_aligned((size_t)mesh_stride * NP_MESH_H);
    if(mesh_buf == NULL) return false;
    np_generate_mesh((uint16_t *)mesh_buf, mesh_stride / 2);

    lv_memzero(&g_np_mesh_dsc, sizeof(g_np_mesh_dsc));
    g_np_mesh_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_np_mesh_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    g_np_mesh_dsc.header.w = NP_MESH_W;
    g_np_mesh_dsc.header.h = NP_MESH_H;
    g_np_mesh_dsc.header.stride = mesh_stride;
    g_np_mesh_dsc.data_size = mesh_stride * NP_MESH_H;
    g_np_mesh_dsc.data = mesh_buf;

    g_np_mesh_buf = mesh_buf;
    snprintf(g_np_mesh_cover, sizeof(g_np_mesh_cover), "%s", cover_path);
    return true;
}

static void np_toggle_queue(void)
{
    if(player_queue_count() <= 0) {
        ui_set_hint("Queue is empty");
        return;
    }
    np_queue_visible = !np_queue_visible;
    ui_rebuild_page();
}

static lv_obj_t * np_vertical_gap(lv_obj_t * parent, int height)
{
    lv_obj_t * gap = lv_obj_create(parent);
    lv_obj_set_size(gap, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gap, 0, 0);
    lv_obj_set_style_pad_all(gap, 0, 0);
    lv_obj_set_scrollable(gap, false);
    return gap;
}

/* Queue-open layout: cover on top, then title + star, artist - album,
 * progress, and one transport row (shuffle / prev / play / next / repeat). */
static void np_build_vertical_left(lv_obj_t * left, const void * cover_src,
                                   uint32_t cover_color)
{
    np_vertical_gap(left, 28);

    np_cover_widget = make_cover(left, cover_src, cover_color, NP_COVER_SIZE);

    np_vertical_gap(left, 16);

    lv_obj_t * title_row = lv_obj_create(left);
    lv_obj_set_size(title_row, NP_INFO_W, 56);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 16, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(title_row, false);

    np_title = make_label(title_row, "No track", FONT_H2, COL_TEXT);
    lv_obj_set_flex_grow(np_title, 1);
    lv_obj_set_height(np_title, 44);
    lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t * star = make_flat_icon_button(title_row, ICON_STAR, 56, 56,
                                            COL_MUTED, &bootstrap_icons_32);
    np_star_icon = lv_obj_get_child(star, 0);
    lv_obj_add_event_cb(star, np_star_click_cb, LV_EVENT_CLICKED, NULL);

    np_vertical_gap(left, 8);

    lv_obj_t * meta_row = lv_obj_create(left);
    lv_obj_set_size(meta_row, NP_INFO_W, 40);
    lv_obj_set_style_bg_opa(meta_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta_row, 0, 0);
    lv_obj_set_style_pad_all(meta_row, 0, 0);
    lv_obj_set_flex_flow(meta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(meta_row, false);

    np_meta = make_label(meta_row, "", FONT_H2, COL_MUTED);
    lv_obj_set_width(np_meta, LV_PCT(100));
    lv_obj_set_height(np_meta, 40);
    lv_label_set_long_mode(np_meta, LV_LABEL_LONG_MODE_DOTS);

    np_vertical_gap(left, 12);

    lv_obj_t * progress_row = lv_obj_create(left);
    lv_obj_set_size(progress_row, NP_INFO_W, 28);
    lv_obj_set_style_bg_opa(progress_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_row, 0, 0);
    lv_obj_set_style_pad_all(progress_row, 0, 0);
    lv_obj_set_style_pad_column(progress_row, 8, 0);
    lv_obj_set_flex_flow(progress_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(progress_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(progress_row, false);

    np_time_cur = make_label(progress_row, "0:00", FONT_NUM, COL_MUTED);
    lv_obj_set_width(np_time_cur, 72);
    lv_obj_set_style_text_align(np_time_cur, LV_TEXT_ALIGN_LEFT, 0);
    np_progress = lv_bar_create(progress_row);
    lv_obj_set_height(np_progress, 8);
    lv_obj_set_flex_grow(np_progress, 1);
    lv_obj_set_style_bg_color(np_progress, lv_color_hex(0x22303F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(np_progress, lv_color_hex(COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(np_progress, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(np_progress, 0, LV_PART_INDICATOR);
    np_time_dur = make_label(progress_row, "0:00", FONT_NUM, COL_MUTED);
    lv_obj_set_width(np_time_dur, 72);
    lv_obj_set_style_text_align(np_time_dur, LV_TEXT_ALIGN_RIGHT, 0);

    np_vertical_gap(left, 16);

    lv_obj_t * controls = lv_obj_create(left);
    lv_obj_set_size(controls, NP_INFO_W, 84);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(controls, false);

    lv_obj_t * shuffle = make_flat_icon_button(controls, ICON_SHUFFLE, 56, 56,
                                                COL_MUTED, &bootstrap_icons_32);
    np_shuffle_btn = shuffle;
    np_shuffle_icon = lv_obj_get_child(shuffle, 0);
    lv_obj_add_event_cb(shuffle, np_shuffle_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * prev = make_flat_icon_button(controls, ICON_PREV, 84, 70,
                                            COL_TEXT, &bootstrap_icons_48);
    lv_obj_t * play = make_flat_icon_button(controls, ICON_PLAY, 108, 80,
                                            COL_ACCENT, &bootstrap_icons_48);
    lv_obj_t * next = make_flat_icon_button(controls, ICON_NEXT, 84, 70,
                                            COL_TEXT, &bootstrap_icons_48);
    np_play_icon = lv_obj_get_child(play, 0);
    lv_obj_add_event_cb(prev, np_prev_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(play, np_play_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next, np_next_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * repeat = make_flat_icon_button(controls, ICON_REPEAT, 56, 56,
                                               COL_MUTED, &bootstrap_icons_32);
    np_repeat_btn = repeat;
    np_repeat_icon = lv_obj_get_child(repeat, 0);
    lv_obj_add_event_cb(repeat, np_repeat_click_cb, LV_EVENT_CLICKED, NULL);

    np_vertical_gap(left, 32);
}

static void np_create(lv_obj_t * parent)
{
    np_queue_count = NULL;
    np_queue_list = NULL;
    np_cover_widget = NULL;
    if(player_queue_count() <= 0) np_queue_visible = false;

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    np_last_track = snapshot.track.id;
    np_last_queue_count = player_queue_count();

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(root, false);

    player_album_t now_album;
    uint64_t now_album_id = 0;
    bool has_now_album = np_resolve_current_album(&snapshot, &now_album,
                                                  &now_album_id);
    const lv_image_dsc_t * iap2_cover =
        (snapshot.real_device && snapshot.track.title[0] != '\0')
            ? iap2_cover_src() : NULL;
    const void * cached_cover =
        has_now_album ? album_cover_src(&now_album) : NULL;
    const void * now_cover =
        iap2_cover != NULL ? (const void *)iap2_cover : cached_cover;
    bool real_cover = iap2_cover != NULL || cached_cover != NULL;
    bool has_cover = now_cover != NULL;
    uint32_t cover_color = now_cover
                               ? (has_now_album ? now_album.color : COL_PANEL)
                               : COL_PANEL_ALT;
    UI_PERF_LOG("np_art create track=%016llx album=%llu iap2=%d cache=%d",
                (unsigned long long)snapshot.track.id,
                (unsigned long long)now_album_id,
                iap2_cover != NULL, cached_cover != NULL);

    /* The mesh/backdrop variants decode the cover at arbitrary scales.  Keep
     * them for the bundled PNG library; real covers are raw RGB565 and are
     * shown 1:1 in the cover widget. iAP2 artwork also stays 1:1. */
    bool mesh_bg = !snapshot.real_device && !real_cover &&
                   (g_np_bg_mode == NP_BG_COVER_GRADIENT && has_cover);
    if(mesh_bg) mesh_bg = np_mesh_prepare(now_album.cover);

    bool blurred_bg = !snapshot.real_device && !mesh_bg && !real_cover &&
                      has_cover &&
                      (g_np_bg_mode == NP_BG_BLURRED_COVER ||
                       g_np_bg_mode == NP_BG_COVER_GRADIENT);

    if(mesh_bg || blurred_bg) {
        lv_obj_t * backdrop = lv_image_create(root);
        lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(backdrop, 0, 0);
        lv_obj_set_ignore_layout(backdrop, true);
        lv_obj_set_clickable(backdrop, false);
        if(mesh_bg) {
            lv_image_set_src(backdrop, &g_np_mesh_dsc);
            /* Mesh is generated at exactly 1280x720. TOP_LEFT selects the
             * plain 1:1 blit path instead of the software transform path. */
            lv_image_set_inner_align(backdrop, LV_IMAGE_ALIGN_TOP_LEFT);
        }
        else {
            lv_image_set_src(backdrop, now_cover);
            lv_image_set_inner_align(backdrop, LV_IMAGE_ALIGN_COVER);
            lv_obj_set_style_blur_radius(backdrop, 80, 0);
            lv_obj_set_style_blur_quality(backdrop, LV_BLUR_QUALITY_AUTO, 0);
        }

        /* Keep the colors recognizable but leave enough contrast for the
         * title, metadata and transport controls. */
        lv_obj_t * scrim = lv_obj_create(root);
        lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(scrim, 0, 0);
        lv_obj_set_ignore_layout(scrim, true);
        lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
        lv_obj_set_style_border_width(scrim, 0, 0);
        lv_obj_set_style_radius(scrim, 0, 0);
        lv_obj_set_style_pad_all(scrim, 0, 0);
        lv_obj_set_clickable(scrim, false);
        lv_obj_set_scrollable(scrim, false);
    }

    /* Left: artwork + metadata + transport */
    lv_obj_t * left = make_panel(root);
    if(mesh_bg || blurred_bg) {
        lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    }
    if(np_queue_visible) {
        lv_obj_set_size(left, 700, LV_PCT(100));
    }
    else {
        lv_obj_set_size(left, LV_PCT(100), LV_PCT(100));
    }
    lv_obj_set_style_pad_all(left, 20, 0);
    lv_obj_set_style_pad_row(left, 0, 0);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    if(np_queue_visible) {
        np_build_vertical_left(left, now_cover, cover_color);
    }
    else {

    lv_obj_t * top_spacer = lv_obj_create(left);
    lv_obj_set_width(top_spacer, LV_PCT(100));
    lv_obj_set_height(top_spacer, 1);
    lv_obj_set_flex_grow(top_spacer, 1);
    lv_obj_set_style_bg_opa(top_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_spacer, 0, 0);
    lv_obj_set_style_pad_all(top_spacer, 0, 0);
    lv_obj_set_scrollable(top_spacer, false);

    lv_obj_t * top = lv_obj_create(left);
    lv_obj_set_size(top, np_queue_visible ? LV_PCT(100) : NP_TOP_W,
                    NP_COVER_SIZE);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_pad_column(top, 20, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(top, false);

    np_cover_widget = make_cover(top, now_cover, cover_color, NP_COVER_SIZE);

    lv_obj_t * info = lv_obj_create(top);
    lv_obj_set_height(info, NP_COVER_SIZE);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info, 0, 0);
    lv_obj_set_style_pad_ver(info, 6, 0);
    lv_obj_set_scrollable(info, false);

    /* Keep the title and metadata together as one text group, and place the
     * group's optical center near the golden-section point (38.2%) of the
     * cover height instead of pinning the title to the top edge. */
    lv_obj_t * text_top_gap = lv_obj_create(info);
    lv_obj_set_size(text_top_gap, LV_PCT(100), 82);
    lv_obj_set_style_bg_opa(text_top_gap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_top_gap, 0, 0);
    lv_obj_set_style_pad_all(text_top_gap, 0, 0);
    lv_obj_set_scrollable(text_top_gap, false);

    np_title = make_label(info, "No track", FONT_NP_TITLE, COL_TEXT);
    lv_obj_set_width(np_title, LV_PCT(100));
    lv_obj_set_height(np_title, 50);
    lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t * title_meta_gap = lv_obj_create(info);
    lv_obj_set_size(title_meta_gap, LV_PCT(100), 12);
    lv_obj_set_style_bg_opa(title_meta_gap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_meta_gap, 0, 0);
    lv_obj_set_style_pad_all(title_meta_gap, 0, 0);
    lv_obj_set_scrollable(title_meta_gap, false);

    np_meta = make_label(info, "", np_queue_visible ? FONT_H2 : FONT_NP_ARTIST,
                         COL_MUTED);
    lv_obj_set_width(np_meta, LV_PCT(100));
    lv_obj_set_height(np_meta, 40);
    lv_label_set_long_mode(np_meta, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t * meta_controls_gap = lv_obj_create(info);
    lv_obj_set_size(meta_controls_gap, LV_PCT(100), 80);
    lv_obj_set_style_bg_opa(meta_controls_gap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta_controls_gap, 0, 0);
    lv_obj_set_style_pad_all(meta_controls_gap, 0, 0);
    lv_obj_set_scrollable(meta_controls_gap, false);

    lv_obj_t * controls = lv_obj_create(info);
    lv_obj_set_size(controls, LV_PCT(100), 84);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    /* Queue view narrows the info column to 280 px: keep the enlarged
     * transport buttons inside it. */
    lv_obj_set_style_pad_column(controls, np_queue_visible ? 0 : 10, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(controls, false);

    lv_obj_t * prev = make_flat_icon_button(controls, ICON_PREV, 84, 70,
                                            COL_TEXT, &bootstrap_icons_48);
    lv_obj_t * play = make_flat_icon_button(controls, ICON_PLAY, 108, 80,
                                            COL_ACCENT, &bootstrap_icons_48);
    lv_obj_t * next = make_flat_icon_button(controls, ICON_NEXT, 84, 70,
                                            COL_TEXT, &bootstrap_icons_48);
    np_play_icon = lv_obj_get_child(play, 0);
    lv_obj_add_event_cb(prev, np_prev_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(play, np_play_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next, np_next_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * gap_before_progress = lv_obj_create(left);
    lv_obj_set_size(gap_before_progress, LV_PCT(100), NP_GAP_PROGRESS);
    lv_obj_set_style_bg_opa(gap_before_progress, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gap_before_progress, 0, 0);
    lv_obj_set_style_pad_all(gap_before_progress, 0, 0);
    lv_obj_set_scrollable(gap_before_progress, false);

    lv_obj_t * progress_row = lv_obj_create(left);
    lv_obj_set_size(progress_row, np_queue_visible ? LV_PCT(100) : NP_TOP_W, 28);
    lv_obj_set_style_bg_opa(progress_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_row, 0, 0);
    lv_obj_set_style_pad_all(progress_row, 0, 0);
    lv_obj_set_style_pad_column(progress_row, 8, 0);
    lv_obj_set_flex_flow(progress_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(progress_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(progress_row, false);

    np_time_cur = make_label(progress_row, "0:00", FONT_NP_TIME, COL_MUTED);
    lv_obj_set_width(np_time_cur, 72);
    lv_obj_set_style_text_align(np_time_cur, LV_TEXT_ALIGN_LEFT, 0);
    np_progress = lv_bar_create(progress_row);
    lv_obj_set_height(np_progress, 8);
    lv_obj_set_flex_grow(np_progress, 1);
    lv_obj_set_style_bg_color(np_progress, lv_color_hex(0x22303F), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(np_progress, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(np_progress, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(np_progress, 0, LV_PART_INDICATOR);
    np_time_dur = make_label(progress_row, "0:00", FONT_NP_TIME, COL_MUTED);
    lv_obj_set_width(np_time_dur, 72);
    lv_obj_set_style_text_align(np_time_dur, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t * gap_before_modes = lv_obj_create(left);
    lv_obj_set_size(gap_before_modes, LV_PCT(100), NP_GAP_MODES);
    lv_obj_set_style_bg_opa(gap_before_modes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gap_before_modes, 0, 0);
    lv_obj_set_style_pad_all(gap_before_modes, 0, 0);
    lv_obj_set_scrollable(gap_before_modes, false);

    lv_obj_t * modes = lv_obj_create(left);
    lv_obj_set_size(modes, np_queue_visible ? LV_PCT(100) : NP_TOP_W, 56);
    lv_obj_set_style_bg_opa(modes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modes, 0, 0);
    lv_obj_set_style_pad_all(modes, 0, 0);
    lv_obj_set_style_pad_column(modes, 40, 0);
    lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modes, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(modes, false);

    lv_obj_t * shuffle = make_flat_icon_button(modes, ICON_SHUFFLE, 56, 56,
                                                COL_MUTED, &bootstrap_icons_32);
    lv_obj_t * repeat = make_flat_icon_button(modes, ICON_REPEAT, 56, 56,
                                               COL_MUTED, &bootstrap_icons_32);
    lv_obj_t * star = make_flat_icon_button(modes, ICON_STAR, 56, 56,
                                            COL_MUTED, &bootstrap_icons_32);
    np_shuffle_btn = shuffle;
    np_repeat_btn = repeat;
    np_shuffle_icon = lv_obj_get_child(shuffle, 0);
    np_repeat_icon = lv_obj_get_child(repeat, 0);
    np_star_icon = lv_obj_get_child(star, 0);
    lv_obj_add_event_cb(shuffle, np_shuffle_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(repeat, np_repeat_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(star, np_star_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * bottom_spacer = lv_obj_create(left);
    lv_obj_set_width(bottom_spacer, LV_PCT(100));
    lv_obj_set_height(bottom_spacer, NP_BOTTOM_SPACER);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);
    lv_obj_set_style_pad_all(bottom_spacer, 0, 0);
    lv_obj_set_scrollable(bottom_spacer, false);

    }

    np_queue_focus_start = g_focus_count;
    if(np_queue_visible) {
        /* Right: queue */
        lv_obj_t * right = make_panel(root);
        lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
        lv_obj_set_flex_grow(right, 1);
        lv_obj_set_height(right, LV_PCT(100));
        lv_obj_set_style_pad_all(right, 18, 0);

        lv_obj_t * queue_head = lv_obj_create(right);
        lv_obj_set_size(queue_head, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(queue_head, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(queue_head, 0, 0);
        lv_obj_set_style_pad_all(queue_head, 0, 0);
        lv_obj_set_flex_flow(queue_head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(queue_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(queue_head, false);
        make_label(queue_head, "Up Next", FONT_H2, COL_TEXT);
        np_queue_count = make_label(queue_head, "0/0", FONT_NUM,
                                    COL_MUTED);

        np_queue_list = make_scroll(right);
        lv_obj_set_flex_grow(np_queue_list, 0);
        lv_obj_set_height(np_queue_list, NP_QUEUE_LIST_H);
        lv_obj_set_style_pad_right(np_queue_list, 14, 0);
        int total = player_queue_count();
        int start = player_queue_index();
        if(start < 0) start = 0;
        for(int i = start; i < total && i < start + 12; i++) {
            np_queue_row(np_queue_list, i, i == start);
        }
    }

    if(np_queue_visible && g_focus_count > 0) focus_select(np_queue_focus_start);
    g_np_cover_rev = player_artwork_revision();
    g_np_cover_track_id = snapshot.track.id;
    g_np_cover_album_id = now_album_id;
    np_refresh_ui();
    ui_set_hint("UP/DOWN Select   ENTER Play/Jump   7 Play/Pause");
}

/* -------------------------------------------------------------------------
 * Albums
 * ------------------------------------------------------------------------- */
#define ALBUMS_ROW_H       77
#define ALBUMS_VISIBLE_ROWS 7
#define ALBUMS_LIST_H      (ALBUMS_VISIBLE_ROWS * ALBUMS_ROW_H + \
                            (ALBUMS_VISIBLE_ROWS - 1) * LIST_ROW_GAP)
#define ALBUMS_STRIDE      (ALBUMS_ROW_H + LIST_ROW_GAP)
#define ALBUMS_POOL        8
#define ALBUM_TRACK_ROW_H  52
#define ALBUM_HEADER_H     (34 + ALBUM_TRACK_ROW_H)
#define ALBUM_TRACK_ROWS   9
#define ALBUM_TRACK_LIST_H (ALBUM_TRACK_ROWS * ALBUM_TRACK_ROW_H)
#define ALBUM_DETAIL_MAX_TRACKS 18

static int albums_selected;
static bool albums_detail_open;
static lv_obj_t * albums_right_content;
static lv_obj_t * albums_left_list;
static lv_obj_t * albums_count_label;
static int albums_left_scroll_y;
static lv_obj_t * albums_rows[ALBUMS_POOL];
static lv_obj_t * albums_row_covers[ALBUMS_POOL];
static lv_obj_t * albums_row_titles[ALBUMS_POOL];
static lv_obj_t * albums_row_artists[ALBUMS_POOL];
static uint32_t albums_swipe_guard;
static uint64_t albums_row_album_ids[ALBUMS_POOL];
static int albums_row_indices[ALBUMS_POOL] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static bool albums_row_visible[ALBUMS_POOL];
static bool albums_row_selected[ALBUMS_POOL];
static int albums_view_top;
static int albums_view_h;
static int albums_first_index = -1;
static bool albums_detail_dirty;
static lv_timer_t * albums_detail_timer;

static void albums_update_count(void)
{
    if(albums_count_label == NULL) return;
    int count = player_album_count();
    if(count <= 0) {
        lv_label_set_text(albums_count_label, "0/0");
        return;
    }
    lv_label_set_text_fmt(albums_count_label, "%d/%d", albums_selected + 1, count);
}

static void albums_rebuild(void)
{
    if(albums_left_list) {
        albums_left_scroll_y = lv_obj_get_scroll_y(albums_left_list);
    }
    ui_rebuild_page();
}

static void albums_render_detail(lv_obj_t * parent, bool interactive);
static void albums_select(int index);
static void albums_scroll_to_selected(void);

static void albums_apply_row_style(lv_obj_t * row, int index)
{
    bool selected = (index == albums_selected);
    bool focused = selected && !albums_detail_open;
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(focused ? COL_FOCUS
                                                       : COL_ACCENT_DARK), 0);
    lv_obj_set_style_border_width(row, focused ? 3 : 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
}

static void albums_update_selection(void)
{
    for(int slot = 0; slot < ALBUMS_POOL; slot++) {
        int index = albums_row_indices[slot];
        bool selected = (index >= 0 && index == albums_selected);
        if(albums_row_selected[slot] != selected) {
            albums_row_selected[slot] = selected;
            albums_apply_row_style(albums_rows[slot], index);
        }
    }
}

static void albums_fill_row(int slot, int index)
{
    lv_obj_t * row = albums_rows[slot];
    player_album_t album;
    if(index < 0 || index >= player_album_count() ||
       !player_album_get(index, &album)) {
        lv_obj_set_hidden(row, true);
        return;
    }

    lv_obj_set_hidden(row, false);
    lv_obj_set_pos(row, 0, index * ALBUMS_STRIDE);
    lv_obj_set_user_data(row, (void *)(intptr_t)index);

    /* Only decode covers that are actually inside the viewport. The pool has
     * a few rows above/below the visible area; loading those covers just
     * wastes SPIFFS reads and PSRAM. */
    int top = albums_view_top;
    int view_h = albums_view_h;
    int row_top = index * ALBUMS_STRIDE;
    bool visible = row_top < top + view_h && row_top + ALBUMS_ROW_H > top;

    bool same = albums_row_album_ids[slot] == album.id &&
                albums_row_indices[slot] == index;
    if(!same) {
        lv_label_set_text(albums_row_titles[slot], album.title);
        lv_label_set_text(albums_row_artists[slot], album.artist);
        lv_obj_set_style_bg_color(albums_row_covers[slot],
                                  lv_color_hex(album.color ? album.color : COL_PANEL), 0);
        albums_row_album_ids[slot] = album.id;
        albums_row_indices[slot] = index;
        albums_row_visible[slot] = !visible; /* force the cover update below */
    }
    if(visible != albums_row_visible[slot]) {
        lv_image_set_src(albums_row_covers[slot],
                         visible ? thumb_for_album(&album, 54) : NULL);
        albums_row_visible[slot] = visible;
    }
    albums_apply_row_style(row, index);
    albums_row_selected[slot] = (index == albums_selected);
}

static void albums_refresh_rows(void)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    if(albums_left_list == NULL) return;
    int count = player_album_count();
    if(count <= 0) {
        for(int i = 0; i < ALBUMS_POOL; i++) {
            lv_obj_set_hidden(albums_rows[i], true);
            albums_row_indices[i] = -1;
        }
        perf_log_us("albums_refresh", perf_t0);
        return;
    }

    int top = lv_obj_get_scroll_top(albums_left_list);
    if(top < 0) top = 0;
    albums_view_top = top;
    albums_view_h = lv_obj_get_height(albums_left_list);
    int first = top / ALBUMS_STRIDE;
    if(first > 0) first--;
    if(first > count - 1) first = count - 1;
    albums_first_index = first;

    int end = first + ALBUMS_POOL;
    if(end > count) end = count;

    /* Keep the existing widgets for the overlapping window and only fill the
     * slots that scrolled out.  Rebuilding all 10 rows on every one-row move
     * cost ~80 ms because each row re-laid out text and artwork. */
    for(int slot = 0; slot < ALBUMS_POOL; slot++) {
        int index = albums_row_indices[slot];
        if(index < first || index >= end) {
            albums_row_indices[slot] = -1;
        }
    }
    for(int index = first; index < end; index++) {
        bool present = false;
        for(int slot = 0; slot < ALBUMS_POOL; slot++) {
            if(albums_row_indices[slot] == index) {
                present = true;
                break;
            }
        }
        if(present) continue;
        int free_slot = -1;
        for(int slot = 0; slot < ALBUMS_POOL; slot++) {
            if(albums_row_indices[slot] < 0) {
                free_slot = slot;
                break;
            }
        }
        if(free_slot < 0) break;
        albums_fill_row(free_slot, index);
    }
    for(int slot = 0; slot < ALBUMS_POOL; slot++) {
        if(albums_row_indices[slot] >= 0) {
            albums_apply_row_style(albums_rows[slot], albums_row_indices[slot]);
        }
        else {
            lv_obj_set_hidden(albums_rows[slot], true);
        }
    }
    perf_log_us("albums_refresh", perf_t0);
}

static void albums_scroll_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if(albums_left_list == NULL) return;
    int top = lv_obj_get_scroll_top(albums_left_list);
    if(top < 0) top = 0;
    int first = top / ALBUMS_STRIDE;
    if(first > 0) first--;
    if(first == albums_first_index) return;
    albums_refresh_rows();
}

static void albums_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(albums_detail_dirty && albums_right_content != NULL) {
        albums_detail_dirty = false;
        uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
        albums_render_detail(albums_right_content, albums_detail_open);
        perf_log_us("albums_detail", perf_t0);
    }
}

static void albums_schedule_detail(void)
{
    albums_detail_dirty = true;
    if(albums_detail_timer == NULL) {
        albums_detail_timer = lv_timer_create(albums_detail_timer_cb, 140, NULL);
        if(albums_detail_timer == NULL) return;
        lv_timer_pause(albums_detail_timer);
    }
    lv_timer_reset(albums_detail_timer);
    lv_timer_resume(albums_detail_timer);
}

static void albums_select_visual(int index)
{
    int count = player_album_count();
    if(count <= 0) return;
    if(index < 0) index = 0;
    if(index >= count) index = count - 1;
    albums_selected = index;
    albums_update_count();
    albums_scroll_to_selected();
}

static void albums_swipe_page(int direction)
{
    if(albums_left_list == NULL) return;
    int rows = lv_obj_get_height(albums_left_list) / ALBUMS_STRIDE;
    if(rows < 2) rows = 2;
    int page = (rows - 1) * ALBUMS_STRIDE;
    int top = lv_obj_get_scroll_top(albums_left_list);
    if(top < 0) top = 0;
    lv_obj_scroll_to_y(albums_left_list, top + direction * page, LV_ANIM_OFF);

    int actual = lv_obj_get_scroll_top(albums_left_list);
    if(actual < 0) actual = 0;
    albums_select_visual(actual / ALBUMS_STRIDE);
    albums_schedule_detail();
    albums_swipe_guard = lv_tick_get();
}

static void albums_scroll_to_selected(void)
{
    if(albums_left_list == NULL) return;
    int count = player_album_count();
    if(count <= 0) return;
    if(albums_selected < 0) albums_selected = 0;
    if(albums_selected >= count) albums_selected = count - 1;

    int top = lv_obj_get_scroll_top(albums_left_list);
    if(top < 0) top = 0;
    int view = lv_obj_get_height(albums_left_list);
    int row_top = albums_selected * ALBUMS_STRIDE;
    int new_top = top;
    if(row_top < top) new_top = row_top;
    else if(row_top + ALBUMS_ROW_H > top + view) {
        new_top = row_top + ALBUMS_ROW_H - view;
    }
    int max_top = count * ALBUMS_STRIDE - LIST_ROW_GAP - view;
    if(max_top < 0) max_top = 0;
    if(new_top < 0) new_top = 0;
    if(new_top > max_top) new_top = max_top;
    if(new_top != top) {
        lv_obj_scroll_to_y(albums_left_list, new_top, LV_ANIM_OFF);
    }
    int actual = lv_obj_get_scroll_top(albums_left_list);
    if(actual < 0) actual = 0;
    int first = actual / ALBUMS_STRIDE;
    if(first > 0) first--;
    if(first != albums_first_index) {
        albums_refresh_rows();
    }
    else {
        albums_update_selection();
    }
}

static void albums_select(int index)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    int count = player_album_count();
    if(count <= 0) return;
    if(index < 0) index = 0;
    if(index >= count) index = count - 1;
    albums_selected = index;
    albums_update_count();
    albums_scroll_to_selected();
    if(albums_right_content) albums_schedule_detail();
    perf_log_us("albums_select", perf_t0);
}

static void albums_open(void * user)
{
    int index = (int)(intptr_t)user;
    player_album_t album;
    if(!player_album_get(index, &album)) return;
    albums_selected = index;
    albums_update_count();
    albums_detail_open = true;
    albums_rebuild();
    if(g_focus_count > 0) focus_select(0);
}

static void albums_track_action(void * user)
{
    int index = (int)(intptr_t)user;
    player_album_t album;
    if(!player_album_get(albums_selected, &album)) return;
    player_track_t track;
    if(player_album_track_get(album.id, index, &track)) {
        player_play_track(track.id);
    }
}

static void albums_open_async(void * arg)
{
    albums_open(arg);
}

static void albums_row_touch_cb(lv_event_t * event)
{
    if(albums_swipe_guard != 0 &&
       (uint32_t)(lv_tick_get() - albums_swipe_guard) < 1000) {
        return;
    }
    lv_obj_t * row = lv_event_get_current_target(event);
    ui_call_async(albums_open_async, lv_obj_get_user_data(row));
}

static bool albums_key(ui_key_t key)
{
    if(albums_detail_open) return false;

    switch(key) {
        case UI_KEY_UP:
            albums_select(albums_selected - 1);
            return true;
        case UI_KEY_DOWN:
            albums_select(albums_selected + 1);
            return true;
        case UI_KEY_ENTER:
            albums_open((void *)(intptr_t)albums_selected);
            return true;
        default:
            return false;
    }
}

static void albums_track_async(void * arg)
{
    albums_track_action(arg);
}

static void albums_track_touch_cb(lv_event_t * event)
{
    ui_call_async(albums_track_async, lv_event_get_user_data(event));
}

static void albums_play_all_action(void * user)
{
    LV_UNUSED(user);
    player_album_t album;
    if(!player_album_get(albums_selected, &album)) return;
    player_play_album(album.id);
}

static void albums_play_async(void * arg)
{
    albums_play_all_action(arg);
}

static void albums_play_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(albums_play_async, NULL);
}

static bool albums_back(void)
{
    if(albums_detail_open) {
        albums_detail_open = false;
        albums_rebuild();
        return true;
    }
    return false;
}

static void albums_same_key(void)
{
    if(albums_detail_open) {
        albums_detail_open = false;
        albums_rebuild();
        return;
    }
    albums_left_scroll_y = 0;
    if(albums_left_list) lv_obj_scroll_to_y(albums_left_list, 0, LV_ANIM_OFF);
    albums_select(0);
}

static void albums_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);

    player_album_t album;
    if(!player_album_get(albums_selected, &album)) return;

    lv_obj_t * detail_head = lv_obj_create(parent);
    /* Left page header (34) + one track row (52) = 86 px. */
    lv_obj_set_size(detail_head, LV_PCT(100), ALBUM_HEADER_H);
    lv_obj_set_style_bg_opa(detail_head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_head, 0, 0);
    lv_obj_set_style_pad_all(detail_head, 0, 0);
    lv_obj_set_flex_flow(detail_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(detail_head, 16, 0);
    lv_obj_set_flex_align(detail_head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(detail_head, false);

    const void * detail_cover_src = detail_thumb_for_album(&album);
    make_cover(detail_head, detail_cover_src, album.color, 80);

    lv_obj_t * detail_text = lv_obj_create(detail_head);
    lv_obj_set_size(detail_text, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(detail_text, 1);
    lv_obj_set_style_bg_opa(detail_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_text, 0, 0);
    lv_obj_set_style_pad_all(detail_text, 0, 0);
    lv_obj_set_style_pad_row(detail_text, 3, 0);
    lv_obj_set_flex_flow(detail_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(detail_text, false);

    lv_obj_t * title_row = lv_obj_create(detail_text);
    lv_obj_set_size(title_row, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 10, 0);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(title_row, false);
    lv_obj_t * title = make_label(title_row, album.title, FONT_DETAIL_TITLE,
                                  COL_TEXT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);

    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "%s  |  %u  |  %u tracks",
             album.artist, album.year, album.track_count);
    make_label(detail_text, subtitle, FONT_CAPTION, COL_MUTED);

    lv_obj_t * play_row = make_row(parent, 48);
    make_icon(play_row, ICON_PLAY_OUTLINE, COL_ACCENT);
    make_label(play_row, "Play album", FONT_H2, COL_TEXT);
    if(interactive) {
        focus_add_clickable(play_row, albums_play_all_action, NULL, NULL, true);
    }
    else {
        touch_add_clickable(play_row, albums_play_touch_cb, NULL);
    }

    lv_obj_t * track_list = make_scroll(parent);
    lv_obj_set_flex_grow(track_list, 0);
    lv_obj_set_height(track_list, ALBUM_TRACK_LIST_H);
    lv_obj_set_style_pad_row(track_list, 0, 0);
    lv_obj_set_style_pad_right(track_list, 14, 0);
    int track_count = player_album_track_count(album.id);
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    int max_tracks = interactive ? ALBUM_DETAIL_MAX_TRACKS : 3;
    if(max_tracks > track_count) max_tracks = track_count;
    for(int i = 0; i < max_tracks; i++) {
        player_track_t track;
        if(!player_album_track_get(album.id, i, &track)) continue;
        bool current = (track.id == snapshot.track.id);

        lv_obj_t * row = make_row(track_list, ALBUM_TRACK_ROW_H);
        lv_obj_t * number = make_label(row, "", FONT_NUM, COL_MUTED);
        lv_obj_set_width(number, 30);
        if(current) {
            lv_label_set_text(number, ICON_PLAY);
            lv_obj_set_style_text_font(number, &bootstrap_icons_24, 0);
            lv_obj_set_style_text_color(number, lv_color_hex(COL_ACCENT), 0);
        }
        else {
            char number_text[8];
            snprintf(number_text, sizeof(number_text), "%02u", track.track_no);
            lv_label_set_text(number, number_text);
        }
        lv_obj_t * title = make_label(row, track.title, FONT_H2,
                                      current ? COL_ACCENT : COL_TEXT);
        lv_obj_set_flex_grow(title, 1);
        char duration[16];
        format_time(track.duration_sec, duration, sizeof(duration));
        make_label(row, duration, FONT_NUM, COL_MUTED);
        if(interactive) {
            focus_add_clickable(row, albums_track_action, NULL,
                                (void *)(intptr_t)i, true);
        }
        else {
            touch_add_clickable(row, albums_track_touch_cb, (void *)(intptr_t)i);
        }
    }
    if(interactive && max_tracks < track_count) {
        lv_obj_t * more = make_label(track_list, "More tracks not shown",
                                     FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(more, 32);
    }
}

static void albums_create(lv_obj_t * parent)
{
    albums_right_content = NULL;
    albums_left_list = NULL;
    albums_count_label = NULL;
    albums_first_index = -1;
    for(int i = 0; i < ALBUMS_POOL; i++) {
        albums_row_album_ids[i] = 0;
        albums_row_indices[i] = -1;
        albums_row_visible[i] = false;
        albums_row_selected[i] = false;
    }
    albums_detail_dirty = false;
    if(albums_detail_timer) lv_timer_pause(albums_detail_timer);

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(root, false);

    lv_obj_t * left = make_panel(root);
    lv_obj_set_size(left, SIDE_PANEL_W, LV_PCT(100));
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_t * left_content = make_panel_content(left, 18);
    lv_obj_set_style_pad_row(left_content, 8, 0);

    lv_obj_t * head = lv_obj_create(left_content);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, "Albums", FONT_PAGE_TITLE, COL_TEXT);
    albums_count_label = make_label(head, "0/0", FONT_NUM, COL_MUTED);
    albums_update_count();

    lv_obj_t * list = lv_obj_create(left_content);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, ALBUMS_LIST_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, 0, LV_PART_SCROLLBAR);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_elastic(list, true);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_chain(list, false);
    lv_obj_set_gesture_bubble(list, false);
    lv_obj_set_clickable(list, true);
    lv_obj_add_event_cb(list, albums_scroll_cb, LV_EVENT_SCROLL, NULL);
    albums_left_list = list;

    int count = player_album_count();
    if(albums_selected >= count) albums_selected = 0;

    int content_h = count > 0 ? count * ALBUMS_STRIDE - LIST_ROW_GAP : ALBUMS_ROW_H;
    lv_obj_t * spacer = lv_obj_create(list);
    lv_obj_set_size(spacer, LV_PCT(100), content_h);
    lv_obj_set_pos(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_set_clickable(spacer, false);

    for(int i = 0; i < ALBUMS_POOL; i++) {
        lv_obj_t * row = make_row(list, ALBUMS_ROW_H);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_clickable(row, true);
        lv_obj_set_gesture_bubble(row, false);
        enable_event_bubble(row);
        lv_obj_add_event_cb(row, albums_row_touch_cb, LV_EVENT_CLICKED, NULL);

        albums_row_covers[i] = make_cover(row, NULL, COL_PANEL, 54);

        lv_obj_t * text = lv_obj_create(row);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text, 0, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_style_pad_row(text, 2, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_scrollable(text, false);
        lv_obj_set_scroll_chain(text, false);
        lv_obj_set_event_bubble(text, true);

        albums_row_titles[i] = make_label(text, "", FONT_H2, COL_TEXT);
        albums_row_artists[i] = make_label(text, "", FONT_CAPTION, COL_MUTED);
        lv_obj_set_hidden(row, true);
        albums_rows[i] = row;
    }

    lv_obj_t * right = make_panel(root);
    lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_all(right, 0, 0);
    albums_right_content = make_panel_content(right, 18);
    lv_obj_set_style_pad_row(albums_right_content, 0, 0);
    albums_render_detail(albums_right_content, albums_detail_open);

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, albums_left_scroll_y, LV_ANIM_OFF);
    albums_refresh_rows();

    if(albums_detail_open) {
        focus_select(0);
        ui_set_hint("UP/DOWN Select   ENTER Play   7 Play/Pause");
    }
    else {
        ui_set_hint("UP/DOWN Select   ENTER Open   7 Play/Pause");
    }
}

/* -------------------------------------------------------------------------
 * Artists
 * ------------------------------------------------------------------------- */
#define ARTISTS_ROW_H       56
#define ARTISTS_VISIBLE_ROWS 10
#define ARTISTS_LIST_H      (ARTISTS_VISIBLE_ROWS * ARTISTS_ROW_H)
#define ARTIST_ALBUM_HEADER_H 56
#define ARTIST_TRACK_ROW_H    56
#define ARTIST_ALBUM_MAX      4
#define ARTIST_TRACKS_PER_ALBUM 6
#define ARTIST_DETAIL_MAX_ROWS  24
#define ARTISTS_POOL          11

static int artists_selected;
static bool artists_detail_open;
static bool artists_detail_dirty;
static lv_timer_t * artists_detail_timer;
static lv_obj_t * artists_right_content;
static lv_obj_t * artists_left_list;
static lv_obj_t * artists_count_label;
static int artists_left_scroll_y;
static lv_obj_t * artists_rows[ARTISTS_POOL];
static lv_obj_t * artists_names[ARTISTS_POOL];
static lv_obj_t * artists_metas[ARTISTS_POOL];
static uint64_t artists_row_ids[ARTISTS_POOL];
static int artists_row_indices[ARTISTS_POOL] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int artists_first_index = -1;

static void artists_update_count(void)
{
    if(artists_count_label == NULL) return;
    int count = player_artist_count();
    if(count <= 0) {
        lv_label_set_text(artists_count_label, "0/0");
        return;
    }
    lv_label_set_text_fmt(artists_count_label, "%d/%d", artists_selected + 1, count);
}

static void artists_render_detail(lv_obj_t * parent, bool interactive);
static void artists_open(void * user);
static void artists_select(int index);
static void artists_scroll_to_selected(void);
static void artists_apply_row_style(lv_obj_t * row, int index)
{
    bool selected = (index == artists_selected);
    bool focused = selected && !artists_detail_open;
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(focused ? COL_FOCUS
                                                       : COL_ACCENT_DARK), 0);
    lv_obj_set_style_border_width(row, focused ? 3 : 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
}

static void artists_fill_row(int slot, int index)
{
    lv_obj_t * row = artists_rows[slot];
    player_artist_t artist;
    if(index < 0 || index >= player_artist_count() ||
       !player_artist_get(index, &artist)) {
        lv_obj_set_hidden(row, true);
        artists_row_indices[slot] = -1;
        return;
    }
    lv_obj_set_hidden(row, false);
    lv_obj_set_pos(row, 0, index * ARTISTS_ROW_H);
    lv_obj_set_user_data(row, (void *)(intptr_t)index);
    if(artists_row_ids[slot] != artist.id ||
       artists_row_indices[slot] != index) {
        lv_label_set_text(artists_names[slot], artist.name);
        char albums_text[32];
        snprintf(albums_text, sizeof(albums_text),
                 artist.album_count == 1 ? "%u album" : "%u albums",
                 artist.album_count);
        lv_label_set_text(artists_metas[slot], albums_text);
        artists_row_ids[slot] = artist.id;
        artists_row_indices[slot] = index;
    }
    artists_apply_row_style(row, index);
}

static void artists_refresh_rows(void)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    if(artists_left_list == NULL) return;
    int count = player_artist_count();
    if(count <= 0) {
        for(int i = 0; i < ARTISTS_POOL; i++) {
            lv_obj_set_hidden(artists_rows[i], true);
            artists_row_indices[i] = -1;
        }
        perf_log_us("artists_refresh", perf_t0);
        return;
    }
    int top = lv_obj_get_scroll_top(artists_left_list);
    if(top < 0) top = 0;
    int first = top / ARTISTS_ROW_H;
    if(first > 0) first--;
    if(first > count - 1) first = count - 1;
    artists_first_index = first;

    int end = first + ARTISTS_POOL;
    if(end > count) end = count;
    for(int slot = 0; slot < ARTISTS_POOL; slot++) {
        int index = artists_row_indices[slot];
        if(index < first || index >= end) artists_row_indices[slot] = -1;
    }
    for(int index = first; index < end; index++) {
        bool present = false;
        for(int slot = 0; slot < ARTISTS_POOL; slot++) {
            if(artists_row_indices[slot] == index) { present = true; break; }
        }
        if(present) continue;
        int free_slot = -1;
        for(int slot = 0; slot < ARTISTS_POOL; slot++) {
            if(artists_row_indices[slot] < 0) { free_slot = slot; break; }
        }
        if(free_slot < 0) break;
        artists_fill_row(free_slot, index);
    }
    for(int slot = 0; slot < ARTISTS_POOL; slot++) {
        if(artists_row_indices[slot] >= 0) {
            artists_apply_row_style(artists_rows[slot], artists_row_indices[slot]);
        }
        else {
            lv_obj_set_hidden(artists_rows[slot], true);
        }
    }
    perf_log_us("artists_refresh", perf_t0);
}

static void artists_scroll_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if(artists_left_list == NULL) return;
    int top = lv_obj_get_scroll_top(artists_left_list);
    if(top < 0) top = 0;
    int first = top / ARTISTS_ROW_H;
    if(first > 0) first--;
    if(first == artists_first_index) return;
    artists_refresh_rows();
}

static void artists_scroll_to_selected(void)
{
    if(artists_left_list == NULL) return;
    int count = player_artist_count();
    int top = lv_obj_get_scroll_top(artists_left_list);
    if(top < 0) top = 0;
    int view = lv_obj_get_height(artists_left_list);
    int row_top = artists_selected * ARTISTS_ROW_H;
    int new_top = top;
    if(row_top < top) new_top = row_top;
    else if(row_top + ARTISTS_ROW_H > top + view) {
        new_top = row_top + ARTISTS_ROW_H - view;
    }
    int max_top = count * ARTISTS_ROW_H - view;
    if(max_top < 0) max_top = 0;
    if(new_top < 0) new_top = 0;
    if(new_top > max_top) new_top = max_top;
    if(new_top != top) {
        lv_obj_scroll_to_y(artists_left_list, new_top, LV_ANIM_OFF);
    }
    artists_refresh_rows();
}

static void artists_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(artists_detail_dirty && artists_right_content != NULL) {
        artists_detail_dirty = false;
        uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
        artists_render_detail(artists_right_content, artists_detail_open);
        perf_log_us("artists_detail", perf_t0);
    }
}

static void artists_schedule_detail(void)
{
    artists_detail_dirty = true;
    if(artists_detail_timer == NULL) {
        artists_detail_timer = lv_timer_create(artists_detail_timer_cb, 140,
                                               NULL);
        if(artists_detail_timer == NULL) return;
        lv_timer_pause(artists_detail_timer);
    }
    lv_timer_reset(artists_detail_timer);
    lv_timer_resume(artists_detail_timer);
}

static void artists_select(int index)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    int count = player_artist_count();
    if(count <= 0) return;
    if(index < 0) index = 0;
    if(index >= count) index = count - 1;
    if(index == artists_selected) {
        artists_scroll_to_selected();
    }
    else {
        artists_selected = index;
        artists_update_count();
        artists_scroll_to_selected();
        if(artists_right_content) artists_schedule_detail();
    }
    perf_log_us("artists_select", perf_t0);
}

static bool artists_key(ui_key_t key)
{
    if(artists_detail_open) return false;
    switch(key) {
        case UI_KEY_UP:
            artists_select(artists_selected - 1);
            return true;
        case UI_KEY_DOWN:
            artists_select(artists_selected + 1);
            return true;
        case UI_KEY_ENTER:
            artists_open((void *)(intptr_t)artists_selected);
            return true;
        default:
            return false;
    }
}

static void artists_rebuild(void)
{
    if(artists_left_list) {
        artists_left_scroll_y = lv_obj_get_scroll_y(artists_left_list);
    }
    ui_rebuild_page();
}

static void artists_open(void * user)
{
    int index = (int)(intptr_t)user;
    player_artist_t artist;
    if(!player_artist_get(index, &artist)) return;
    artists_selected = index;
    artists_update_count();
    artists_detail_open = true;
    artists_rebuild();
    if(g_focus_count > 0) focus_select(0);
}

static void artists_album_action(void * user)
{
    int album_index = (int)(intptr_t)user;
    player_album_t album;
    if(!player_album_get(album_index, &album)) return;
    player_play_album(album.id);
}

static void artists_track_action(void * user)
{
    int track_index = (int)(intptr_t)user;
    player_track_t track;
    if(track_index >= 0 && track_index < player_song_count() &&
       player_song_get(track_index, &track)) {
        player_play_track(track.id);
    }
}

static void artists_open_async(void * arg)
{
    artists_open(arg);
}

static void artists_album_async(void * arg)
{
    artists_album_action(arg);
}

static void artists_track_async(void * arg)
{
    artists_track_action(arg);
}

static void artists_row_touch_cb(lv_event_t * event)
{
    lv_obj_t * row = lv_event_get_current_target(event);
    ui_call_async(artists_open_async, lv_obj_get_user_data(row));
}

static void artists_album_touch_cb(lv_event_t * event)
{
    ui_call_async(artists_album_async, lv_event_get_user_data(event));
}

static void artists_track_touch_cb(lv_event_t * event)
{
    ui_call_async(artists_track_async, lv_event_get_user_data(event));
}

static bool artists_back(void)
{
    if(artists_detail_open) {
        artists_detail_open = false;
        artists_rebuild();
        return true;
    }
    return false;
}

static void artists_same_key(void)
{
    if(artists_detail_open) {
        artists_detail_open = false;
        artists_rebuild();
        return;
    }
    artists_left_scroll_y = 0;
    if(artists_left_list) lv_obj_scroll_to_y(artists_left_list, 0, LV_ANIM_OFF);
    artists_select(0);
}

static void artists_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);

    player_artist_t artist;
    if(!player_artist_get(artists_selected, &artist)) return;

    /* Header row: name + stats + Back share the same 56 px rhythm as the
     * left list, leaving the full 9-row viewport below. */
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, artist.name, FONT_DETAIL_TITLE, COL_TEXT);
    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "%u songs  |  %u albums",
             artist.song_count, artist.album_count);
    make_label(head, subtitle, FONT_CAPTION, COL_MUTED);

    lv_obj_t * spacer = lv_obj_create(head);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);

    if(!interactive) {
        make_label(parent, "Press ENTER to open artist", FONT_CAPTION,
                   COL_MUTED);
        return;
    }

    lv_obj_t * album_list = make_scroll(parent);
    /* 10 rows x 56 px = 560 px viewport. */
    lv_obj_set_flex_grow(album_list, 0);
    lv_obj_set_height(album_list, 560);
    lv_obj_set_style_pad_row(album_list, 0, 0);
    lv_obj_set_style_pad_right(album_list, 14, 0);
    /* Collect a bounded set of albums/tracks in one pass.  The previous
     * version scanned every artist song for every album, which made prolific
     * artists (hundreds of tracks) block the UI for several seconds. */
    uint64_t album_ids[ARTIST_ALBUM_MAX] = {0};
    int album_track_indices[ARTIST_ALBUM_MAX][ARTIST_TRACKS_PER_ALBUM];
    int album_track_counts[ARTIST_ALBUM_MAX] = {0};
    int album_total = 0;
    int track_rows = 0;

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    int songs = player_song_count();
    for(int i = 0; i < songs && track_rows < ARTIST_DETAIL_MAX_ROWS; i++) {
        player_track_t track;
        if(!player_song_get(i, &track) || track.artist_id != artist.id) continue;

        int slot = -1;
        for(int a = 0; a < album_total; a++) {
            if(album_ids[a] == track.album_id) {
                slot = a;
                break;
            }
        }
        if(slot < 0) {
            if(album_total >= ARTIST_ALBUM_MAX) continue;
            slot = album_total++;
            album_ids[slot] = track.album_id;
        }
        if(album_track_counts[slot] < ARTIST_TRACKS_PER_ALBUM) {
            album_track_indices[slot][album_track_counts[slot]++] = i;
            track_rows++;
        }
    }

    for(int a = 0; a < album_total; a++) {
        player_album_t album;
        int album_index = -1;
        for(int i = 0; i < player_album_count(); i++) {
            player_album_t candidate;
            if(player_album_get(i, &candidate) && candidate.id == album_ids[a]) {
                album = candidate;
                album_index = i;
                break;
            }
        }
        if(album_index < 0) continue;

        lv_obj_t * header = make_row(album_list, ARTIST_ALBUM_HEADER_H);
        make_cover(header, thumb_for_album(&album, 34), album.color, 34);

        lv_obj_t * album_title = make_label(header, album.title, FONT_H2, COL_TEXT);
        lv_obj_set_flex_grow(album_title, 1);
        lv_obj_set_height(album_title, 34);
        lv_label_set_long_mode(album_title, LV_LABEL_LONG_MODE_DOTS);

        char meta[64];
        snprintf(meta, sizeof(meta), "%u  |  %u tracks",
                 album.year, album.track_count);
        lv_obj_t * album_meta = make_label(header, meta, FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(album_meta, 22);
        make_icon(header, ICON_PLAY, COL_MUTED);

        if(interactive) {
            focus_add_clickable(header, artists_album_action, NULL,
                                (void *)(intptr_t)album_index, true);
        }
        else {
            touch_add_clickable(header, artists_album_touch_cb,
                                (void *)(intptr_t)album_index);
        }

        for(int j = 0; j < album_track_counts[a]; j++) {
            int track_index = album_track_indices[a][j];
            player_track_t track;
            if(!player_song_get(track_index, &track)) continue;
            bool current = (track.id == snapshot.track.id);

            lv_obj_t * row = make_row(album_list, ARTIST_TRACK_ROW_H);
            lv_obj_t * marker = make_label(row, "", FONT_NUM, COL_MUTED);
            lv_obj_set_width(marker, 30);
            if(current) {
                lv_label_set_text(marker, ICON_PLAY);
                lv_obj_set_style_text_font(marker, &bootstrap_icons_24, 0);
                lv_obj_set_style_text_color(marker, lv_color_hex(COL_ACCENT), 0);
            }
            else {
                char number[8];
                snprintf(number, sizeof(number), "%02u", track.track_no);
                lv_label_set_text(marker, number);
            }

            lv_obj_t * title = make_label(row, track.title, FONT_H2,
                                          current ? COL_ACCENT : COL_TEXT);
            lv_obj_set_flex_grow(title, 1);
            lv_obj_set_height(title, 34);
            lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);

            char duration[16];
            format_time(track.duration_sec, duration, sizeof(duration));
            make_label(row, duration, FONT_NUM, COL_MUTED);

            if(interactive) {
                focus_add_clickable(row, artists_track_action, NULL,
                                    (void *)(intptr_t)track_index, true);
            }
            else {
                touch_add_clickable(row, artists_track_touch_cb,
                                    (void *)(intptr_t)track_index);
            }
        }
    }

    if(track_rows >= ARTIST_DETAIL_MAX_ROWS) {
        lv_obj_t * more = make_label(album_list, "More tracks not shown",
                                     FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(more, 32);
    }
}

static void artists_create(lv_obj_t * parent)
{
    if(artists_detail_timer) lv_timer_pause(artists_detail_timer);
    artists_detail_dirty = false;
    artists_right_content = NULL;
    artists_left_list = NULL;
    artists_count_label = NULL;
    artists_first_index = -1;
    for(int i = 0; i < ARTISTS_POOL; i++) {
        artists_row_ids[i] = 0;
        artists_row_indices[i] = -1;
    }

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(root, false);

    lv_obj_t * left = make_panel(root);
    lv_obj_set_size(left, SIDE_PANEL_W, LV_PCT(100));
    lv_obj_set_style_pad_all(left, 18, 0);
    lv_obj_set_style_pad_row(left, 8, 0);

    lv_obj_t * head = lv_obj_create(left);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, "Artists", FONT_PAGE_TITLE, COL_TEXT);
    artists_count_label = make_label(head, "0/0", FONT_NUM, COL_MUTED);
    artists_update_count();

    lv_obj_t * list = make_scroll(left);
    /* The pooled rows are positioned absolutely over a spacer, so the list
     * itself must not run a flex layout (the spacer would push every row
     * below the viewport). */
    lv_obj_set_layout(list, LV_LAYOUT_NONE);
    lv_obj_set_flex_grow(list, 0);
    lv_obj_set_height(list, ARTISTS_LIST_H);
    lv_obj_set_style_pad_row(list, 0, 0);
    artists_left_list = list;
    int count = player_artist_count();
    if(artists_selected >= count) artists_selected = 0;

    lv_obj_t * spacer = lv_obj_create(list);
    lv_obj_set_size(spacer, LV_PCT(100), count > 0 ? count * ARTISTS_ROW_H : ARTISTS_ROW_H);
    lv_obj_set_pos(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_set_clickable(spacer, false);

    artists_first_index = -1;
    for(int i = 0; i < ARTISTS_POOL; i++) {
        lv_obj_t * row = make_row(list, ARTISTS_ROW_H);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_clickable(row, true);
        lv_obj_set_gesture_bubble(row, false);
        enable_event_bubble(row);
        lv_obj_add_event_cb(row, artists_row_touch_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * text = lv_obj_create(row);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text, 0, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_style_pad_column(text, 10, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(text, false);
        artists_names[i] = make_label(text, "", FONT_H2, COL_TEXT);
        lv_obj_set_width(artists_names[i], 320);
        lv_label_set_long_mode(artists_names[i], LV_LABEL_LONG_MODE_DOTS);
        artists_metas[i] = make_label(text, "", FONT_CAPTION, COL_MUTED);
        lv_obj_set_hidden(row, true);
        artists_rows[i] = row;
    }
    lv_obj_add_event_cb(list, artists_scroll_cb, LV_EVENT_SCROLL, NULL);
    artists_refresh_rows();

    lv_obj_t * right = make_panel(root);
    lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_all(right, 0, 0);
    artists_right_content = make_panel_content(right, 18);
    lv_obj_set_style_pad_row(artists_right_content, 8, 0);
    artists_render_detail(artists_right_content, artists_detail_open);

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, artists_left_scroll_y, LV_ANIM_OFF);
    artists_refresh_rows();

    if(artists_detail_open) {
        focus_select(0);
        ui_set_hint("UP/DOWN Select   ENTER Play   7 Play/Pause");
    }
    else {
        ui_set_hint("UP/DOWN Select   ENTER Open   7 Play/Pause");
    }
}

/* -------------------------------------------------------------------------
 * Context menu
 *
 * A small modal popover used by the knob/Enter long-press action. Items are
 * grouped under section headers. The menu owns its own focus index instead of
 * touching the page focus chain, so closing it can never lose the underlying
 * page's selection.
 * ------------------------------------------------------------------------- */
#define CTX_MENU_WIDTH      440
#define CTX_MENU_GROUP_H    38
#define CTX_MENU_ROW_H      64
#define CTX_MENU_MAX_ITEMS  8

#define COL_CTX_PANEL       0x242424
#define COL_CTX_GROUP       0x1B1B1B
#define COL_CTX_BORDER      0x4A4A4A

typedef struct {
    lv_obj_t * row;
    lv_obj_t * icon;
    lv_obj_t * label;
    lv_obj_t * value;
    focus_activate_fn action;
    bool active;
} context_menu_item_t;

static lv_obj_t * g_ctx_menu;
static context_menu_item_t g_ctx_menu_items[CTX_MENU_MAX_ITEMS];
static int g_ctx_menu_item_count;
static int g_ctx_menu_focus;
static int g_ctx_star_item = -1;
static int g_ctx_shuffle_item = -1;
static int g_ctx_repeat_item = -1;

static int album_index_by_id(uint64_t album_id)
{
    player_album_t album;
    for(int i = 0; i < player_album_count(); i++) {
        if(player_album_get(i, &album) && album.id == album_id) return i;
    }
    return -1;
}

static int artist_index_by_id(uint64_t artist_id)
{
    player_artist_t artist;
    for(int i = 0; i < player_artist_count(); i++) {
        if(player_artist_get(i, &artist) && artist.id == artist_id) return i;
    }
    return -1;
}

static void context_menu_focus_set(int index)
{
    if(g_ctx_menu_item_count <= 0) return;
    if(index < 0) index = 0;
    if(index >= g_ctx_menu_item_count) index = g_ctx_menu_item_count - 1;

    g_ctx_menu_focus = index;
    for(int i = 0; i < g_ctx_menu_item_count; i++) {
        context_menu_item_t * item = &g_ctx_menu_items[i];
        bool focused = (i == index);
        lv_obj_set_style_bg_opa(item->row, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(item->row, lv_color_hex(COL_FOCUS), 0);
        lv_obj_set_style_border_width(item->row, focused ? 3 : 0, 0);
        lv_obj_set_style_border_side(item->row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(item->row, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_color(item->icon,
                                    lv_color_hex(focused ? COL_TEXT
                                                         : (item->active ? COL_ACCENT
                                                                         : COL_MUTED)), 0);
        if(item->value) {
            lv_obj_set_style_text_color(item->value,
                                        lv_color_hex(item->active ? COL_ACCENT
                                                                  : COL_MUTED), 0);
        }
    }
}

static void context_menu_item_click_cb(lv_event_t * event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if(index < 0 || index >= g_ctx_menu_item_count) return;
    g_ctx_menu_focus = index;
    context_menu_item_t * item = &g_ctx_menu_items[index];
    if(item->action) ui_call_async(item->action, NULL);
}

static void context_menu_activate(void)
{
    if(g_ctx_menu_focus < 0 || g_ctx_menu_focus >= g_ctx_menu_item_count) return;
    context_menu_item_t * item = &g_ctx_menu_items[g_ctx_menu_focus];
    if(item->action) item->action(NULL);
}

static void context_menu_group(lv_obj_t * parent, const char * title)
{
    lv_obj_t * group = lv_obj_create(parent);
    lv_obj_set_size(group, LV_PCT(100), CTX_MENU_GROUP_H);
    lv_obj_set_style_bg_color(group, lv_color_hex(COL_CTX_GROUP), 0);
    lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(group, 1, 0);
    lv_obj_set_style_border_side(group, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(group, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_radius(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_left(group, 20, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(group, false);
    make_label(group, title, FONT_GROUP, COL_MUTED);
}

static int context_menu_item(lv_obj_t * parent, const char * icon,
                             const char * text, const char * value,
                             bool active, focus_activate_fn action)
{
    if(g_ctx_menu_item_count >= CTX_MENU_MAX_ITEMS) return -1;
    int index = g_ctx_menu_item_count++;
    context_menu_item_t * item = &g_ctx_menu_items[index];
    memset(item, 0, sizeof(*item));
    item->active = active;

    item->row = lv_obj_create(parent);
    lv_obj_set_size(item->row, LV_PCT(100), CTX_MENU_ROW_H);
    lv_obj_set_style_bg_opa(item->row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(item->row, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_border_width(item->row, 0, 0);
    lv_obj_set_style_border_side(item->row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(item->row, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_radius(item->row, 0, 0);
    lv_obj_set_style_pad_all(item->row, 0, 0);
    lv_obj_set_style_pad_left(item->row, 20, 0);
    lv_obj_set_style_pad_right(item->row, 20, 0);
    lv_obj_set_style_pad_column(item->row, 16, 0);
    lv_obj_set_flex_flow(item->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item->row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(item->row, false);
    lv_obj_set_clickable(item->row, true);
    lv_obj_add_event_cb(item->row, context_menu_item_click_cb,
                        LV_EVENT_CLICKED, (void *)(intptr_t)index);

    item->icon = make_label(item->row, icon, &bootstrap_icons_32, COL_MUTED);
    lv_obj_set_width(item->icon, 34);
    lv_obj_set_style_text_align(item->icon, LV_TEXT_ALIGN_CENTER, 0);
    item->label = make_label(item->row, text, FONT_MENU, COL_TEXT);
    lv_obj_set_flex_grow(item->label, 1);
    if(value) {
        item->value = make_label(item->row, value, FONT_GROUP, COL_MUTED);
        lv_obj_set_width(item->value, 64);
        lv_obj_set_style_text_align(item->value, LV_TEXT_ALIGN_RIGHT, 0);
    }
    item->action = action;
    return index;
}

static void context_menu_update_item(int index, const char * icon,
                                     const char * text, const char * value,
                                     bool active)
{
    if(index < 0 || index >= g_ctx_menu_item_count) return;
    context_menu_item_t * item = &g_ctx_menu_items[index];
    if(icon) lv_label_set_text(item->icon, icon);
    if(text) lv_label_set_text(item->label, text);
    if(item->value && value) lv_label_set_text(item->value, value);
    item->active = active;
    context_menu_focus_set(g_ctx_menu_focus);
}

static void context_menu_go_album(void * user)
{
    LV_UNUSED(user);
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    int index = album_index_by_id(snapshot.track.album_id);
    if(index < 0) return;
    albums_selected = index;
    albums_detail_open = true;
    context_menu_close();
    ui_show_screen(SCR_ALBUMS);
}

static void context_menu_go_artist(void * user)
{
    LV_UNUSED(user);
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    int index = artist_index_by_id(snapshot.track.artist_id);
    if(index < 0) return;
    artists_selected = index;
    artists_detail_open = true;
    context_menu_close();
    ui_show_screen(SCR_ARTISTS);
}

static void context_menu_star(void * user)
{
    LV_UNUSED(user);
    player_toggle_star();
    np_refresh_ui();

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    context_menu_update_item(g_ctx_star_item,
                             snapshot.starred ? ICON_STAR_FILL : ICON_STAR,
                             snapshot.starred ? "Unstar" : "Star",
                             NULL, snapshot.starred);
}

static void context_menu_shuffle(void * user)
{
    LV_UNUSED(user);
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    player_set_shuffle(!snapshot.shuffle);
    np_refresh_ui();

    player_get_snapshot(&snapshot);
    context_menu_update_item(g_ctx_shuffle_item, ICON_SHUFFLE, "Shuffle",
                             snapshot.shuffle ? "On" : "Off",
                             snapshot.shuffle);
}

static void context_menu_repeat(void * user)
{
    LV_UNUSED(user);
    player_cycle_repeat();
    np_refresh_ui();

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    const char * icon = ICON_REPEAT;
    const char * value = "Off";
    if(snapshot.repeat == PLAYER_REPEAT_ALL) value = "All";
    else if(snapshot.repeat == PLAYER_REPEAT_ONE) {
        icon = ICON_REPEAT_ONE;
        value = "One";
    }
    context_menu_update_item(g_ctx_repeat_item, icon, "Repeat", value,
                             snapshot.repeat != PLAYER_REPEAT_OFF);
}

static void context_menu_backdrop_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    context_menu_close();
}

static void context_menu_open(void)
{
    if(g_ctx_menu_open || g_current_screen != SCR_NOW_PLAYING) return;

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    g_ctx_menu_open = true;
    g_ctx_menu_item_count = 0;
    g_ctx_menu_focus = 0;

    g_ctx_menu = lv_obj_create(g_page);
    lv_obj_set_size(g_ctx_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_ctx_menu, 0, 0);
    lv_obj_set_style_bg_color(g_ctx_menu, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_ctx_menu, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_ctx_menu, 0, 0);
    lv_obj_set_style_radius(g_ctx_menu, 0, 0);
    lv_obj_set_style_pad_all(g_ctx_menu, 0, 0);
    lv_obj_set_scrollable(g_ctx_menu, false);
    lv_obj_set_clickable(g_ctx_menu, true);
    lv_obj_add_event_cb(g_ctx_menu, context_menu_backdrop_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t * panel = lv_obj_create(g_ctx_menu);
    lv_obj_set_size(panel, CTX_MENU_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_align(panel, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_CTX_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_CTX_BORDER), 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(panel, false);

    const char * repeat_icon = ICON_REPEAT;
    const char * repeat_value = "Off";
    if(snapshot.repeat == PLAYER_REPEAT_ALL) repeat_value = "All";
    else if(snapshot.repeat == PLAYER_REPEAT_ONE) {
        repeat_icon = ICON_REPEAT_ONE;
        repeat_value = "One";
    }

    context_menu_group(panel, "GoTo");
    context_menu_item(panel, ICON_DISC, "Go to album", NULL, false,
                      context_menu_go_album);
    context_menu_item(panel, ICON_PERSON, "Go to artist", NULL, false,
                      context_menu_go_artist);

    context_menu_group(panel, "Star");
    g_ctx_star_item = context_menu_item(panel,
                                        snapshot.starred ? ICON_STAR_FILL : ICON_STAR,
                                        snapshot.starred ? "Unstar" : "Star",
                                        NULL, snapshot.starred,
                                        context_menu_star);

    context_menu_group(panel, "Playback");
    g_ctx_shuffle_item = context_menu_item(panel, ICON_SHUFFLE, "Shuffle",
                                           snapshot.shuffle ? "On" : "Off",
                                           snapshot.shuffle,
                                           context_menu_shuffle);
    g_ctx_repeat_item = context_menu_item(panel, repeat_icon, "Repeat",
                                          repeat_value,
                                          snapshot.repeat != PLAYER_REPEAT_OFF,
                                          context_menu_repeat);

    context_menu_focus_set(0);
}

static void context_menu_close(void)
{
    if(!g_ctx_menu_open) return;
    g_ctx_menu_open = false;
    if(g_ctx_menu) {
        lv_obj_delete(g_ctx_menu);
        g_ctx_menu = NULL;
    }
    for(int i = 0; i < g_ctx_menu_item_count; i++) {
        g_ctx_menu_items[i].row = NULL;
        g_ctx_menu_items[i].icon = NULL;
        g_ctx_menu_items[i].label = NULL;
        g_ctx_menu_items[i].action = NULL;
    }
    g_ctx_menu_item_count = 0;
    g_ctx_menu_focus = 0;
    g_ctx_star_item = -1;
    g_ctx_shuffle_item = -1;
    g_ctx_repeat_item = -1;
}

static bool context_menu_key(ui_key_t key)
{
    switch(key) {
        case UI_KEY_UP:
            context_menu_focus_set(g_ctx_menu_focus - 1);
            break;
        case UI_KEY_DOWN:
            context_menu_focus_set(g_ctx_menu_focus + 1);
            break;
        case UI_KEY_ENTER:
            context_menu_activate();
            break;
        case UI_KEY_BACK:
        case UI_KEY_CONTEXT:
            context_menu_close();
            break;
        default:
            break;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Songs
 *
 * The song list is virtualized: only a small pool of row widgets exists and
 * it is reused for the visible slice. Creating one widget per song would
 * freeze the UI for seconds with a 500+ track library.
 * ------------------------------------------------------------------------- */
#define SONGS_ROW_H        56
#define SONGS_VISIBLE_ROWS 10
#define SONGS_LIST_H       (SONGS_VISIBLE_ROWS * SONGS_ROW_H)
#define SONGS_POOL         12

static int songs_selected;
static lv_obj_t * songs_list;
static lv_obj_t * songs_count_label;
static lv_obj_t * songs_rows[SONGS_POOL];
static lv_obj_t * songs_markers[SONGS_POOL];
static lv_obj_t * songs_titles[SONGS_POOL];
static lv_obj_t * songs_artists[SONGS_POOL];
static lv_obj_t * songs_albums[SONGS_POOL];
static lv_obj_t * songs_lengths[SONGS_POOL];
static int songs_start_index = -1;
static uint64_t songs_row_ids[SONGS_POOL];
static int songs_row_indices[SONGS_POOL] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static bool songs_row_current[SONGS_POOL];
static bool songs_row_selected[SONGS_POOL];
static uint64_t songs_last_track;
static uint32_t songs_swipe_guard;

static void songs_apply_selection(lv_obj_t * row, bool selected)
{
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
}

static void songs_set_marker(int slot, bool current, int index)
{
    if(current) {
        lv_label_set_text(songs_markers[slot], ICON_PLAY);
        lv_obj_set_style_text_font(songs_markers[slot], &bootstrap_icons_24, 0);
        lv_obj_set_style_text_color(songs_markers[slot], lv_color_hex(COL_ACCENT), 0);
    }
    else {
        char number[16];
        snprintf(number, sizeof(number), "%02d", index + 1);
        lv_label_set_text(songs_markers[slot], number);
        lv_obj_set_style_text_font(songs_markers[slot], FONT_NUM, 0);
        lv_obj_set_style_text_color(songs_markers[slot], lv_color_hex(COL_MUTED), 0);
    }
}

static void songs_fill_row(int slot, int index,
                           const player_snapshot_t * snapshot)
{
    lv_obj_t * row = songs_rows[slot];
    player_track_t track;
    if(index < 0 || index >= player_song_count() ||
       !player_song_get(index, &track)) {
        lv_obj_set_hidden(row, true);
        songs_row_ids[slot] = 0;
        songs_row_indices[slot] = -1;
        return;
    }

    lv_obj_set_hidden(row, false);
    lv_obj_set_pos(row, 0, index * SONGS_ROW_H);
    lv_obj_set_user_data(row, (void *)(intptr_t)index);

    if(songs_row_indices[slot] == index && songs_row_ids[slot] == track.id) {
        bool current = (track.id == snapshot->track.id);
        if(songs_row_current[slot] != current) {
            songs_row_current[slot] = current;
            songs_set_marker(slot, current, index);
        }
        songs_apply_selection(row, index == songs_selected);
        songs_row_selected[slot] = (index == songs_selected);
        return;
    }

    bool current = (track.id == snapshot->track.id);
    if(true) {
        songs_set_marker(slot, current, index);

        lv_label_set_text(songs_titles[slot], track.title);
        lv_obj_set_style_text_color(songs_titles[slot],
                                    lv_color_hex(current ? COL_ACCENT : COL_TEXT), 0);
        lv_label_set_text(songs_artists[slot], track.artist);
        lv_label_set_text(songs_albums[slot], track.album);
        char duration[16];
        format_time(track.duration_sec, duration, sizeof(duration));
        lv_label_set_text(songs_lengths[slot], duration);
        songs_row_ids[slot] = track.id;
        songs_row_indices[slot] = index;
        songs_row_current[slot] = current;
    }

    songs_apply_selection(row, index == songs_selected);
    songs_row_selected[slot] = (index == songs_selected);
}

static void songs_update_selection(void)
{
    for(int i = 0; i < SONGS_POOL; i++) {
        int index = songs_row_indices[i];
        bool selected = (index >= 0 && index == songs_selected);
        if(songs_row_selected[i] != selected) {
            songs_row_selected[i] = selected;
            songs_apply_selection(songs_rows[i], selected);
        }
    }
}

static void songs_refresh_rows(void)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    if(songs_list == NULL) return;
    int count = player_song_count();
    if(count <= 0) {
        for(int i = 0; i < SONGS_POOL; i++) {
            lv_obj_set_hidden(songs_rows[i], true);
            songs_row_indices[i] = -1;
        }
        perf_log_us("songs_refresh", perf_t0);
        return;
    }
    if(songs_selected < 0) songs_selected = 0;
    if(songs_selected >= count) songs_selected = count - 1;

    int top = lv_obj_get_scroll_top(songs_list);
    if(top < 0) top = 0;
    int start = top / SONGS_ROW_H;
    if(start > 0) start--;
    if(start > count - 1) start = count - 1;
    songs_start_index = start;

    int end = start + SONGS_POOL;
    if(end > count) end = count;
    for(int slot = 0; slot < SONGS_POOL; slot++) {
        int index = songs_row_indices[slot];
        if(index < start || index >= end) songs_row_indices[slot] = -1;
    }

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    for(int index = start; index < end; index++) {
        bool present = false;
        for(int slot = 0; slot < SONGS_POOL; slot++) {
            if(songs_row_indices[slot] == index) { present = true; break; }
        }
        if(present) continue;
        int free_slot = -1;
        for(int slot = 0; slot < SONGS_POOL; slot++) {
            if(songs_row_indices[slot] < 0) { free_slot = slot; break; }
        }
        if(free_slot < 0) break;
        songs_fill_row(free_slot, index, &snapshot);
    }
    for(int slot = 0; slot < SONGS_POOL; slot++) {
        if(songs_row_indices[slot] < 0) lv_obj_set_hidden(songs_rows[slot], true);
    }
    songs_update_selection();
    perf_log_us("songs_refresh", perf_t0);
}

static void songs_scroll_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if(songs_list == NULL) return;
    int top = lv_obj_get_scroll_top(songs_list);
    if(top < 0) top = 0;
    int start = top / SONGS_ROW_H;
    if(start > 0) start--;
    if(start == songs_start_index) return;
    songs_refresh_rows();
}

static void songs_select(int index);

static void songs_swipe_page(int direction)
{
    if(songs_list == NULL) return;
    int rows = lv_obj_get_height(songs_list) / SONGS_ROW_H;
    if(rows < 2) rows = 2;
    int page = (rows - 1) * SONGS_ROW_H;
    int top = lv_obj_get_scroll_top(songs_list);
    if(top < 0) top = 0;
    lv_obj_scroll_to_y(songs_list, top + direction * page, LV_ANIM_OFF);

    int actual = lv_obj_get_scroll_top(songs_list);
    if(actual < 0) actual = 0;
    songs_select(actual / SONGS_ROW_H);
    songs_swipe_guard = lv_tick_get();
}

/* Page Up/Down pages the visible list first; on other screens it keeps the
 * previous screen-navigation behaviour. Also useful for testing paging with a
 * USB keyboard or the serial debug keys. */
static bool ui_list_page(int direction)
{
    if(g_current_screen == SCR_ALBUMS) {
        albums_swipe_page(direction);
        return true;
    }
    if(g_current_screen == SCR_SONGS) {
        songs_swipe_page(direction);
        return true;
    }
    return false;
}

static void songs_scroll_to_selected(void)
{
    if(songs_list == NULL) return;
    int count = player_song_count();
    if(count <= 0) return;
    if(songs_selected < 0) songs_selected = 0;
    if(songs_selected >= count) songs_selected = count - 1;

    int top = lv_obj_get_scroll_top(songs_list);
    int view = lv_obj_get_height(songs_list);
    int row_top = songs_selected * SONGS_ROW_H;
    int new_top = top;
    if(row_top < top) new_top = row_top;
    else if(row_top + SONGS_ROW_H > top + view) {
        new_top = row_top + SONGS_ROW_H - view;
    }
    if(new_top < 0) new_top = 0;
    if(new_top != top) {
        lv_obj_scroll_to_y(songs_list, new_top, LV_ANIM_OFF);
    }
    int actual = lv_obj_get_scroll_top(songs_list);
    if(actual < 0) actual = 0;
    int start = actual / SONGS_ROW_H;
    if(start > 0) start--;
    if(start != songs_start_index) {
        songs_refresh_rows();
    }
    else {
        songs_update_selection();
    }
}

static void songs_update_count(void)
{
    if(songs_count_label == NULL) return;
    int count = player_song_count();
    if(count <= 0) {
        lv_label_set_text(songs_count_label, "0/0");
        return;
    }
    int position = songs_selected + 1;
    if(position < 1) position = 1;
    if(position > count) position = count;
    lv_label_set_text_fmt(songs_count_label, "%d/%d", position, count);
}

static void songs_select(int index)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    int count = player_song_count();
    if(count <= 0) return;
    if(index < 0) index = 0;
    if(index >= count) index = count - 1;
    songs_selected = index;
    songs_update_count();
    songs_scroll_to_selected();
    perf_log_us("songs_select", perf_t0);
}

static void songs_activate(void)
{
    if(player_song_count() <= 0) return;
    player_track_t track;
    if(player_song_get(songs_selected, &track)) {
        player_play_track(track.id);
    }
    songs_refresh_rows();
}

static void songs_activate_async(void * arg)
{
    LV_UNUSED(arg);
    songs_activate();
    songs_scroll_to_selected();
}

static void songs_row_click_cb(lv_event_t * event)
{
    if(songs_swipe_guard != 0 &&
       (uint32_t)(lv_tick_get() - songs_swipe_guard) < 1000) {
        return;
    }
    lv_obj_t * row = lv_event_get_current_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(row);
    songs_selected = index;
    songs_update_count();
    ui_call_async(songs_activate_async, NULL);
}

static bool songs_key(ui_key_t key)
{
    switch(key) {
        case UI_KEY_UP:
            songs_select(songs_selected - 1);
            return true;
        case UI_KEY_DOWN:
            songs_select(songs_selected + 1);
            return true;
        case UI_KEY_ENTER:
            songs_activate();
            return true;
        default:
            return false;
    }
}

static void songs_tick(void)
{
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    if(snapshot.track.id != songs_last_track) {
        songs_last_track = snapshot.track.id;
        songs_refresh_rows();
    }
}

static bool songs_back(void)
{
    return false;
}

static void songs_same_key(void)
{
    songs_select(0);
}

static void songs_create(lv_obj_t * parent)
{
    songs_count_label = NULL;
    lv_obj_t * panel = make_panel(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);

    lv_obj_t * head = lv_obj_create(panel);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, "Songs", FONT_PAGE_TITLE, COL_TEXT);
    songs_count_label = make_label(head, "0/0", FONT_NUM, COL_MUTED);

    songs_list = lv_obj_create(panel);
    lv_obj_set_width(songs_list, LV_PCT(100));
    lv_obj_set_flex_grow(songs_list, 0);
    lv_obj_set_height(songs_list, SONGS_LIST_H);
    lv_obj_set_style_bg_opa(songs_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(songs_list, 0, 0);
    lv_obj_set_style_pad_all(songs_list, 0, 0);
    lv_obj_set_style_pad_right(songs_list, 14, 0);
    lv_obj_set_scroll_dir(songs_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(songs_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_radius(songs_list, 0, LV_PART_SCROLLBAR);
    lv_obj_set_scroll_elastic(songs_list, true);
    lv_obj_set_scrollable(songs_list, true);
    lv_obj_set_scroll_chain(songs_list, false);
    lv_obj_set_gesture_bubble(songs_list, false);
    lv_obj_set_clickable(songs_list, true);
    lv_obj_add_event_cb(songs_list, songs_scroll_cb, LV_EVENT_SCROLL, NULL);

    int count = player_song_count();
    lv_obj_t * spacer = lv_obj_create(songs_list);
    lv_obj_set_size(spacer, LV_PCT(100), count > 0 ? count * SONGS_ROW_H : SONGS_ROW_H);
    lv_obj_set_pos(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_set_clickable(spacer, false);

    for(int i = 0; i < SONGS_POOL; i++) {
        lv_obj_t * row = make_row(songs_list, SONGS_ROW_H);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_clickable(row, true);
        lv_obj_set_gesture_bubble(row, false);
        enable_event_bubble(row);
        lv_obj_add_event_cb(row, songs_row_click_cb, LV_EVENT_CLICKED, NULL);

        songs_markers[i] = make_label(row, "", FONT_NUM, COL_MUTED);
        lv_obj_set_width(songs_markers[i], 30);
        songs_titles[i] = make_label(row, "", FONT_H2, COL_TEXT);
        lv_obj_set_flex_grow(songs_titles[i], 1);
        lv_obj_set_height(songs_titles[i], 34);
        lv_label_set_long_mode(songs_titles[i], LV_LABEL_LONG_MODE_DOTS);
        songs_artists[i] = make_label(row, "", FONT_CAPTION, COL_MUTED);
        lv_obj_set_width(songs_artists[i], 230);
        lv_obj_set_height(songs_artists[i], 22);
        lv_label_set_long_mode(songs_artists[i], LV_LABEL_LONG_MODE_DOTS);
        songs_albums[i] = make_label(row, "", FONT_CAPTION, COL_DIM);
        lv_obj_set_width(songs_albums[i], 250);
        lv_obj_set_height(songs_albums[i], 22);
        lv_label_set_long_mode(songs_albums[i], LV_LABEL_LONG_MODE_DOTS);
        songs_lengths[i] = make_label(row, "", FONT_NUM, COL_MUTED);
        lv_obj_set_width(songs_lengths[i], 56);
        lv_obj_set_style_text_align(songs_lengths[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_hidden(row, true);

        songs_rows[i] = row;
    }

    lv_obj_update_layout(panel);
    songs_start_index = -1;
    for(int i = 0; i < SONGS_POOL; i++) {
        songs_row_ids[i] = 0;
        songs_row_indices[i] = -1;
        songs_row_current[i] = false;
        songs_row_selected[i] = false;
    }
    songs_refresh_rows();
    songs_scroll_to_selected();
    songs_update_count();

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    songs_last_track = snapshot.track.id;
    ui_set_hint("UP/DOWN Select   ENTER Play   7 Play/Pause");
}

/* -------------------------------------------------------------------------
 * Playlists
 * ------------------------------------------------------------------------- */
#define PLAYLISTS_ROW_H        56
#define PLAYLISTS_VISIBLE_ROWS 10
#define PLAYLISTS_LIST_H       (PLAYLISTS_VISIBLE_ROWS * PLAYLISTS_ROW_H)
#define PLAYLIST_TRACK_ROW_H   56
#define PLAYLIST_TRACK_ROWS    9
#define PLAYLIST_TRACK_LIST_H  (PLAYLIST_TRACK_ROWS * PLAYLIST_TRACK_ROW_H)
#define PLAYLIST_DETAIL_MAX_TRACKS 24

static int playlists_selected;
static bool playlists_detail_open;
static bool playlists_detail_dirty;
static lv_timer_t * playlists_detail_timer;
static lv_obj_t * playlists_left_list;
static lv_obj_t * playlists_right_content;
static lv_obj_t * playlists_count_label;
static int playlists_left_scroll_y;

static void playlists_update_count(void)
{
    if(playlists_count_label == NULL) return;
    int count = player_playlist_count();
    if(count <= 0) {
        lv_label_set_text(playlists_count_label, "0/0");
        return;
    }
    lv_label_set_text_fmt(playlists_count_label, "%d/%d",
                          playlists_selected + 1, count);
}

static void playlists_render_detail(lv_obj_t * parent, bool interactive);

static void playlists_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(playlists_detail_dirty && playlists_right_content != NULL) {
        playlists_detail_dirty = false;
        uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
        playlists_render_detail(playlists_right_content, false);
        perf_log_us("playlists_detail", perf_t0);
    }
}

static void playlists_schedule_detail(void)
{
    playlists_detail_dirty = true;
    if(playlists_detail_timer == NULL) {
        playlists_detail_timer = lv_timer_create(playlists_detail_timer_cb,
                                                 140, NULL);
        if(playlists_detail_timer == NULL) return;
        lv_timer_pause(playlists_detail_timer);
    }
    lv_timer_reset(playlists_detail_timer);
    lv_timer_resume(playlists_detail_timer);
}

static void playlists_row_focus(void * user)
{
    int index = (int)(intptr_t)user;
    if(index == playlists_selected) return;
    playlists_selected = index;
    playlists_update_count();
    if(playlists_right_content) playlists_schedule_detail();
}

static void playlists_rebuild(void)
{
    if(playlists_left_list) {
        playlists_left_scroll_y = lv_obj_get_scroll_y(playlists_left_list);
    }
    ui_rebuild_page();
}

static void playlists_open(void * user)
{
    int index = (int)(intptr_t)user;
    player_playlist_t playlist;
    if(!player_playlist_get(index, &playlist)) return;
    playlists_selected = index;
    playlists_update_count();
    playlists_detail_open = true;
    playlists_rebuild();
    if(g_focus_count > 0) focus_select(0);
}

static void playlists_play_action(void * user)
{
    LV_UNUSED(user);
    player_play_playlist(playlists_selected);
}

static void playlists_track_action(void * user)
{
    int index = (int)(intptr_t)user;
    player_track_t track;
    if(player_playlist_track_get(playlists_selected, index, &track)) {
        player_play_track(track.id);
    }
}

static void playlists_open_async(void * arg)
{
    playlists_open(arg);
}

static void playlists_play_async(void * arg)
{
    playlists_play_action(arg);
}

static void playlists_track_async(void * arg)
{
    playlists_track_action(arg);
}

static void playlists_row_touch_cb(lv_event_t * event)
{
    ui_call_async(playlists_open_async, lv_event_get_user_data(event));
}

static void playlists_play_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(playlists_play_async, NULL);
}

static void playlists_track_touch_cb(lv_event_t * event)
{
    ui_call_async(playlists_track_async, lv_event_get_user_data(event));
}

static bool playlists_back(void)
{
    if(playlists_detail_open) {
        playlists_detail_open = false;
        playlists_rebuild();
        return true;
    }
    return false;
}

static void playlists_same_key(void)
{
    if(playlists_detail_open) {
        playlists_detail_open = false;
        playlists_rebuild();
        return;
    }
    playlists_selected = 0;
    playlists_left_scroll_y = 0;
    playlists_update_count();
    if(g_focus_count > 0) focus_select(0);
}

static void playlists_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);

    player_playlist_t playlist;
    if(!player_playlist_get(playlists_selected, &playlist)) return;

    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    lv_obj_t * title = make_label(head, playlist.name, FONT_DETAIL_TITLE,
                                  COL_TEXT);
    lv_obj_set_flex_grow(title, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);

    char meta[64];
    const char * noun = playlist.track_count == 1 ? "track" : "tracks";
    if(playlist.local) {
        snprintf(meta, sizeof(meta), "%u %s  |  Local",
                 playlist.track_count, noun);
    }
    else {
        snprintf(meta, sizeof(meta), "%u %s", playlist.track_count, noun);
    }
    make_label(head, meta, FONT_CAPTION, COL_MUTED);

    lv_obj_t * spacer = lv_obj_create(head);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);


    lv_obj_t * play_row = make_row(parent, 48);
    make_icon(play_row, ICON_PLAY_OUTLINE, COL_ACCENT);
    make_label(play_row, "Play playlist", FONT_H2, COL_TEXT);
    if(interactive) {
        focus_add_clickable(play_row, playlists_play_action, NULL, NULL, true);
    }
    else {
        touch_add_clickable(play_row, playlists_play_touch_cb, NULL);
    }

    lv_obj_t * track_list = make_scroll(parent);
    lv_obj_set_flex_grow(track_list, 0);
    lv_obj_set_height(track_list, PLAYLIST_TRACK_LIST_H);
    lv_obj_set_style_pad_row(track_list, 0, 0);
    lv_obj_set_style_pad_right(track_list, 14, 0);

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    int count = player_playlist_track_count(playlists_selected);
    int max_tracks = interactive ? PLAYLIST_DETAIL_MAX_TRACKS : 3;
    int shown = 0;
    for(int i = 0; i < count && shown < max_tracks; i++) {
        player_track_t track;
        if(!player_playlist_track_get(playlists_selected, i, &track)) continue;
        bool current = (track.id == snapshot.track.id);

        lv_obj_t * row = make_row(track_list, PLAYLIST_TRACK_ROW_H);
        lv_obj_t * marker = make_label(row, "", FONT_NUM, COL_MUTED);
        lv_obj_set_width(marker, 30);
        if(current) {
            lv_label_set_text(marker, ICON_PLAY);
            lv_obj_set_style_text_font(marker, &bootstrap_icons_24, 0);
            lv_obj_set_style_text_color(marker, lv_color_hex(COL_ACCENT), 0);
        }
        else {
            char number[16];
            snprintf(number, sizeof(number), "%02d", i + 1);
            lv_label_set_text(marker, number);
        }

        lv_obj_t * track_title = make_label(row, track.title, FONT_H2,
                                            current ? COL_ACCENT : COL_TEXT);
        lv_obj_set_flex_grow(track_title, 1);
        lv_obj_set_height(track_title, 34);
        lv_label_set_long_mode(track_title, LV_LABEL_LONG_MODE_DOTS);

        char duration[16];
        format_time(track.duration_sec, duration, sizeof(duration));
        make_label(row, duration, FONT_NUM, COL_MUTED);

        if(interactive) {
            focus_add_clickable(row, playlists_track_action, NULL,
                                (void *)(intptr_t)i, true);
        }
        else {
            touch_add_clickable(row, playlists_track_touch_cb,
                                (void *)(intptr_t)i);
        }
        shown++;
    }
    if(interactive && shown < count) {
        lv_obj_t * more = make_label(track_list, "More tracks not shown",
                                     FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(more, 32);
    }
}

static void playlists_create(lv_obj_t * parent)
{
    if(playlists_detail_timer) lv_timer_pause(playlists_detail_timer);
    playlists_detail_dirty = false;
    playlists_right_content = NULL;
    playlists_left_list = NULL;
    playlists_count_label = NULL;

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(root, false);

    lv_obj_t * left = make_panel(root);
    lv_obj_set_size(left, SIDE_PANEL_W, LV_PCT(100));
    lv_obj_set_style_pad_all(left, 18, 0);
    lv_obj_set_style_pad_row(left, 8, 0);

    lv_obj_t * head = lv_obj_create(left);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, "Playlists", FONT_PAGE_TITLE, COL_TEXT);
    playlists_count_label = make_label(head, "0/0", FONT_NUM, COL_MUTED);

    lv_obj_t * list = make_scroll(left);
    lv_obj_set_flex_grow(list, 0);
    lv_obj_set_height(list, PLAYLISTS_LIST_H);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_style_pad_right(list, 14, 0);
    playlists_left_list = list;

    int count = player_playlist_count();
    if(playlists_selected >= count) playlists_selected = 0;
    for(int i = 0; i < count; i++) {
        player_playlist_t playlist;
        if(!player_playlist_get(i, &playlist)) continue;

        lv_obj_t * row = make_row(list, PLAYLISTS_ROW_H);
        lv_obj_t * text = lv_obj_create(row);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text, 0, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_style_pad_column(text, 10, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(text, false);

        lv_obj_t * name = make_label(text, playlist.name, FONT_H2, COL_TEXT);
        lv_point_t name_size;
        lv_text_get_size(&name_size, playlist.name, FONT_H2, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t name_w = name_size.x > 300 ? 300 : name_size.x;
        lv_obj_set_width(name, name_w);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

        char meta[32];
        const char * noun = playlist.track_count == 1 ? "track" : "tracks";
        snprintf(meta, sizeof(meta), "%u %s", playlist.track_count, noun);
        make_label(text, meta, FONT_CAPTION, COL_MUTED);

        if(i == playlists_selected) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT_DARK), 0);
        }

        if(!playlists_detail_open) {
            int focus_index = focus_add_clickable(row, playlists_open, NULL,
                                                  (void *)(intptr_t)i, true);
            focus_set_on_focus(focus_index, playlists_row_focus);
        }
        else {
            touch_add_clickable(row, playlists_row_touch_cb,
                                (void *)(intptr_t)i);
        }
    }

    lv_obj_t * right = make_panel(root);
    lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_all(right, 0, 0);
    playlists_right_content = make_panel_content(right, 18);
    lv_obj_set_style_pad_row(playlists_right_content, 8, 0);
    playlists_render_detail(playlists_right_content, playlists_detail_open);

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, playlists_left_scroll_y, LV_ANIM_OFF);

    playlists_update_count();
    if(playlists_detail_open) {
        focus_select(0);
        ui_set_hint("UP/DOWN Select   ENTER Play   7 Play/Pause");
    }
    else {
        focus_select(playlists_selected);
        ui_set_hint("UP/DOWN Select   ENTER Open   7 Play/Pause");
    }
}

/* -------------------------------------------------------------------------
 * Bluetooth Remote
 * ------------------------------------------------------------------------- */
static uint32_t ble_ui_rev;
static int ble_restore_focus = -1;
static int ble_long_code = -1;

static void ble_rebuild(void)
{
    ui_rebuild_page();
}

static void ble_pair_action(void * user)
{
    LV_UNUSED(user);
    ble_remote_state_t state = BLE_REMOTE_OFF;
    ble_remote_status(&state, NULL, 0);
    if(state == BLE_REMOTE_SCANNING) {
        ble_remote_scan_stop();
    }
    else {
        ble_remote_scan_start();
    }
    ble_remote_status(&state, NULL, 0);
    LV_LOG_USER("BLE pair action: state=%d", (int)state);
    ble_rebuild();
}

static void ble_row_action(void * user)
{
    int code = (int)(intptr_t)user;
    LV_LOG_USER("BLE row action: code=%d", code);
    if(ble_long_code >= 0 && code == ble_long_code) {
        ble_long_code = -1;
        return;
    }
    if(code == 0) {
        ble_pair_action(NULL);
        return;
    }

    ble_remote_device_t dev;
    bool ok = false;
    if(code >= 2000) {
        ok = ble_remote_paired_get(code - 2000, &dev);
    }
    else if(code >= 1000) {
        ok = ble_remote_scan_get(code - 1000, &dev);
    }
    if(ok) ble_remote_connect(dev.addr);
    ble_rebuild();
}

static void ble_long_async(void * arg)
{
    int code = (int)(intptr_t)arg;
    if(code < 2000) return;
    ble_remote_device_t dev;
    if(ble_remote_paired_get(code - 2000, &dev)) {
        ble_long_code = code;
        ble_remote_unpair(dev.addr);
        ble_restore_focus = g_focus_index;
        ui_rebuild_page();
    }
}

static void ble_long_cb(lv_event_t * event)
{
    ui_call_async(ble_long_async, lv_event_get_user_data(event));
}

static void ble_create(lv_obj_t * parent)
{
    ble_ui_rev = ble_remote_revision();
    ble_long_code = -1;

    ble_remote_state_t state = BLE_REMOTE_OFF;
    char status[96];
    ble_remote_status(&state, status, sizeof(status));
    bool scanning = state == BLE_REMOTE_SCANNING;

    lv_obj_t * root = make_panel(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 22, 0);
    lv_obj_set_style_pad_row(root, 12, 0);

    lv_obj_t * head = lv_obj_create(root);
    lv_obj_set_size(head, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, "Bluetooth Remote", FONT_PAGE_TITLE, COL_TEXT);
    make_label(head, status, FONT_H2, scanning ? COL_ACCENT : COL_MUTED);

    lv_obj_t * list = make_scroll(root);
    lv_obj_set_style_pad_row(list, 4, 0);

    lv_obj_t * pair_row = make_row(list, 64);
    make_label(pair_row, ICON_BLUETOOTH, &bootstrap_icons_32, COL_ACCENT);
    make_label(pair_row, scanning ? "Stop Scan" : "Pair New Devices",
               FONT_H2, COL_TEXT);
    lv_obj_t * pair_spacer = lv_obj_create(pair_row);
    lv_obj_set_size(pair_spacer, 1, 1);
    lv_obj_set_flex_grow(pair_spacer, 1);
    lv_obj_set_style_bg_opa(pair_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_spacer, 0, 0);
    lv_obj_set_style_pad_all(pair_spacer, 0, 0);
    lv_obj_set_scrollable(pair_spacer, false);
    make_label(pair_row, scanning ? "Scanning..." : "Scan",
               FONT_CAPTION, COL_MUTED);
    focus_add_clickable(pair_row, ble_row_action, NULL, (void *)0, true);

    make_label(list, scanning ? "Nearby Devices" : "Paired Devices",
               FONT_CAPTION, COL_MUTED);

    int count = scanning ? ble_remote_scan_count()
                         : ble_remote_paired_count();
    if(count <= 0) {
        make_label(list,
                   scanning ? "Searching for D100H..." :
                              "No paired devices. Use Pair New Devices.",
                   FONT_H2, COL_MUTED);
    }

    for(int i = 0; i < count; i++) {
        ble_remote_device_t dev;
        bool ok = scanning ? ble_remote_scan_get(i, &dev)
                           : ble_remote_paired_get(i, &dev);
        if(!ok) continue;

        lv_obj_t * row = make_row(list, 64);
        make_label(row, ICON_BLUETOOTH, &bootstrap_icons_24,
                   scanning ? COL_MUTED : COL_ACCENT);

        lv_obj_t * text = lv_obj_create(row);
        lv_obj_set_height(text, 52);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text, 0, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_style_pad_row(text, 1, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scrollable(text, false);
        make_label(text, dev.name[0] ? dev.name : dev.addr,
                   FONT_H2, COL_TEXT);
        make_label(text, dev.addr, FONT_CAPTION, COL_MUTED);

        if(scanning) {
            focus_add_clickable(row, ble_row_action, NULL,
                                (void *)(intptr_t)(1000 + i), true);
        }
        else {
            focus_add_clickable(row, ble_row_action, NULL,
                                (void *)(intptr_t)(2000 + i), true);
            lv_obj_add_event_cb(row, ble_long_cb, LV_EVENT_LONG_PRESSED,
                                (void *)(intptr_t)(2000 + i));
        }
    }

    ui_set_hint(scanning ? "ENTER Connect   CONTEXT Stop Scan" :
                           "ENTER Connect   Long-press Unpair");

    if(ble_restore_focus >= 0 && ble_restore_focus < g_focus_count) {
        focus_select(ble_restore_focus);
    }
    else {
        focus_select(0);
    }
    ble_restore_focus = -1;
}

static bool ble_key(ui_key_t key)
{
    ble_remote_state_t state = BLE_REMOTE_OFF;
    ble_remote_status(&state, NULL, 0);

    if(key == UI_KEY_CONTEXT) {
        if(state == BLE_REMOTE_SCANNING) {
            ble_remote_scan_stop();
            ble_restore_focus = g_focus_index;
            ui_rebuild_page();
            return true;
        }
        if(g_focus_index > 0) {
            ble_remote_device_t dev;
            if(ble_remote_paired_get(g_focus_index - 1, &dev)) {
                ble_remote_unpair(dev.addr);
                ble_restore_focus = g_focus_index;
                ui_rebuild_page();
                return true;
            }
        }
    }
    else if(key == UI_KEY_BACK && state == BLE_REMOTE_SCANNING) {
        ble_remote_scan_stop();
        ble_restore_focus = g_focus_index;
        ui_rebuild_page();
        return true;
    }
    return false;
}

static bool ble_back(void)
{
    ble_remote_state_t state = BLE_REMOTE_OFF;
    ble_remote_status(&state, NULL, 0);
    if(state == BLE_REMOTE_SCANNING) {
        ble_remote_scan_stop();
        ble_restore_focus = g_focus_index;
        ui_rebuild_page();
        return true;
    }
    return false;
}

static void ble_same_key(void)
{
    if(g_focus_count > 0) focus_select(0);
}

static void ble_tick(void)
{
    uint32_t rev = ble_remote_revision();
    if(rev != ble_ui_rev) {
        ble_ui_rev = rev;
        ble_restore_focus = g_focus_index;
        ui_rebuild_page();
    }
}

/* -------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------- */
enum {
    SETTING_GENERAL = 0,
    SETTING_BLUETOOTH,
    SETTING_THEME,
    SETTING_NOW_PLAYING_BG,
    SETTING_DEVICE,
    SETTING_USB,
    SETTING_AFC,
    SETTING_BACKEND,
    SETTING_ABOUT,
    SETTING_COUNT,
};

static int settings_selected;
static bool settings_detail_open;
static lv_obj_t * settings_right_content;
static lv_obj_t * settings_count_label;
static lv_obj_t * settings_detail_time_format_value;
static lv_obj_t * settings_detail_set_time_value;
static lv_obj_t * settings_time_hour_value;
static lv_obj_t * settings_time_minute_value;
static lv_obj_t * settings_time_ampm_value;
static bool settings_time_open;
static bool settings_time_editing;
static bool settings_time_blink_on;
static bool settings_detail_dirty;
static lv_timer_t * settings_detail_timer;
enum {
    SETTINGS_TIME_NONE = 0,
    SETTINGS_TIME_HOUR,
    SETTINGS_TIME_MINUTE,
    SETTINGS_TIME_AMPM,
};
static int settings_time_segment;
static lv_obj_t * settings_detail_touch_value;
static lv_obj_t * settings_detail_menubar_value;
static lv_obj_t * settings_detail_brightness_value;
static lv_obj_t * settings_detail_sort_value;
static lv_obj_t * settings_detail_order_value;
static lv_obj_t * settings_detail_np_bg_theme_check;
static lv_obj_t * settings_detail_np_bg_blur_check;
static lv_obj_t * settings_detail_np_bg_mesh_check;
static lv_obj_t * settings_detail_screen_value;
static lv_obj_t * settings_detail_wifi_mode_value;
static lv_obj_t * settings_detail_wifi_status_value;
static lv_obj_t * settings_detail_sound_value;
static lv_obj_t * settings_detail_volume_value;
static int settings_brightness = 70;

typedef enum {
    USB_MODE_HOST = 0,
    USB_MODE_OTG,
} usb_mode_t;

static usb_mode_t g_usb_mode = USB_MODE_HOST;

enum {
    SORT_FIELD_ADD_DATE = 0,
    SORT_FIELD_NAME,
    SORT_FIELD_YEAR,
    SORT_FIELD_COUNT,
};
static int g_sort_field = SORT_FIELD_ADD_DATE;
static bool g_sort_desc = true;

void ui_apply_saved_settings(const ui_system_config_t * cfg)
{
    if(cfg == NULL) return;

    g_system_config = *cfg;

    g_clock_12h = cfg->clock_12h != 0;
    if(cfg->sort_field >= 0 && cfg->sort_field < SORT_FIELD_COUNT) {
        g_sort_field = cfg->sort_field;
    }
    g_sort_desc = cfg->sort_desc != 0;
    if(cfg->np_bg_mode >= NP_BG_THEME && cfg->np_bg_mode <= NP_BG_COVER_GRADIENT) {
        g_np_bg_mode = (np_bg_mode_t)cfg->np_bg_mode;
    }
    settings_brightness = cfg->brightness;
    if(settings_brightness < 0) settings_brightness = 0;
    if(settings_brightness > 100) settings_brightness = 100;
    g_usb_mode = cfg->usb_mode == 1 ? USB_MODE_OTG : USB_MODE_HOST;

    ui_set_menubar_left(cfg->menubar_left != 0);
    ui_set_touch_enabled(cfg->touch_enabled != 0);
}

static void settings_rebuild(void);

static const char * const settings_names[SETTING_COUNT] = {
    "General",
    "Bluetooth",
    "Theme",
    "Now Playing Background",
    "Device",
    "USB-C",
    "Apple File Conduit Sync",
    "Backend",
    "About",
};

static int settings_initial_focus(int index)
{
    if(index == SETTING_NOW_PLAYING_BG) {
        return (int)g_np_bg_mode;
    }
    if(index == SETTING_USB) {
        return (int)g_usb_mode;
    }
    return 0;
}

static void settings_update_count(void)
{
    if(settings_count_label == NULL) return;
    lv_label_set_text_fmt(settings_count_label, "%d/%d",
                          settings_selected + 1, SETTING_COUNT);
}

static void settings_current_time(int * hour, int * minute)
{
    time_t now = time(NULL) + g_clock_offset_min * 60;
    struct tm local;
    localtime_r(&now, &local);
    if(hour) *hour = local.tm_hour;
    if(minute) *minute = local.tm_min;
}

static void settings_format_time(char * buffer, size_t size)
{
    int hour;
    int minute;
    settings_current_time(&hour, &minute);
    if(g_clock_12h) {
        int hour12 = hour % 12;
        if(hour12 == 0) hour12 = 12;
        snprintf(buffer, size, "%d:%02d %s", hour12, minute,
                 hour < 12 ? "AM" : "PM");
    }
    else {
        snprintf(buffer, size, "%02d:%02d", hour, minute);
    }
}

static void settings_update_time_labels(void)
{
    if(settings_detail_set_time_value) {
        char text[16];
        settings_format_time(text, sizeof(text));
        lv_label_set_text(settings_detail_set_time_value, text);
    }
    if(settings_time_hour_value || settings_time_minute_value) {
        int hour;
        int minute;
        settings_current_time(&hour, &minute);
        if(settings_time_hour_value) {
            char text[8];
            if(g_clock_12h) {
                int hour12 = hour % 12;
                if(hour12 == 0) hour12 = 12;
                snprintf(text, sizeof(text), "%d", hour12);
            }
            else {
                snprintf(text, sizeof(text), "%02d", hour);
            }
            lv_label_set_text(settings_time_hour_value, text);
        }
        if(settings_time_minute_value) {
            char text[8];
            snprintf(text, sizeof(text), "%02d", minute);
            lv_label_set_text(settings_time_minute_value, text);
        }
        if(settings_time_ampm_value) {
            lv_label_set_text(settings_time_ampm_value,
                              hour < 12 ? "AM" : "PM");
        }
    }
}

static void settings_update_time_format_labels(void)
{
    if(settings_detail_time_format_value) {
        lv_label_set_text(settings_detail_time_format_value,
                          g_clock_12h ? "12H" : "24H");
    }
    settings_update_time_labels();
}

static void settings_clock_adjust_minutes(int delta)
{
    g_clock_offset_min += delta;
    settings_update_time_labels();
    update_clock();
}

static void settings_update_touch_labels(void)
{
    const char * text = ui_touch_enabled() ? "On" : "Off";
    if(settings_detail_touch_value) {
        lv_label_set_text(settings_detail_touch_value, text);
    }
}

static void settings_update_menubar_labels(void)
{
    const char * text = ui_menubar_left() ? "Left" : "Right";
    if(settings_detail_menubar_value) {
        lv_label_set_text(settings_detail_menubar_value, text);
    }
}

static void settings_update_brightness_labels(void)
{
    char text[8];
    snprintf(text, sizeof(text), "%d%%", settings_brightness);
    if(settings_detail_brightness_value) {
        lv_label_set_text(settings_detail_brightness_value, text);
    }
}

static void settings_update_system_labels(void)
{
    if(settings_detail_screen_value) {
        lv_label_set_text(settings_detail_screen_value,
                          g_system_config.screen_flip == 180 ? "180 deg" : "0 deg");
    }
    if(settings_detail_wifi_mode_value) {
        lv_label_set_text(settings_detail_wifi_mode_value,
                          g_system_config.wifi_mode == 1 ? "AP" : "Station");
    }
    if(settings_detail_wifi_status_value) {
        char text[128];
        snprintf(text, sizeof(text), "%s  %s", g_system_config.wifi_status,
                 g_system_config.wifi_ip);
        lv_label_set_text(settings_detail_wifi_status_value, text);
    }
    if(settings_detail_sound_value) {
        lv_label_set_text(settings_detail_sound_value,
                          g_system_config.sound_enabled ? "On" : "Off");
    }
    if(settings_detail_volume_value) {
        char text[8];
        snprintf(text, sizeof(text), "%d%%", g_system_config.volume);
        lv_label_set_text(settings_detail_volume_value, text);
    }
}

static void settings_system_changed(void)
{
    g_system_config.clock_12h = g_clock_12h ? 1 : 0;
    g_system_config.sort_field = g_sort_field;
    g_system_config.sort_desc = g_sort_desc ? 1 : 0;
    g_system_config.menubar_left = ui_menubar_left() ? 1 : 0;
    g_system_config.touch_enabled = ui_touch_enabled() ? 1 : 0;
    g_system_config.np_bg_mode = (int)g_np_bg_mode;
    g_system_config.brightness = settings_brightness;
    g_system_config.usb_mode = (g_usb_mode == USB_MODE_OTG) ? 1 : 0;

    settings_update_system_labels();
    if(g_system_config_changed_cb != NULL) {
        g_system_config_changed_cb(&g_system_config);
    }
}

static void settings_screen_action(void * user)
{
    (void)user;
    g_system_config.screen_flip = g_system_config.screen_flip == 180 ? 0 : 180;
    settings_system_changed();
}

static void settings_wifi_mode_action(void * user)
{
    (void)user;
    g_system_config.wifi_mode = g_system_config.wifi_mode == 1 ? 0 : 1;
    settings_system_changed();
}

static void settings_sound_action(void * user)
{
    (void)user;
    g_system_config.sound_enabled = !g_system_config.sound_enabled;
    settings_system_changed();
}

static void settings_volume_adjust(int delta, void * user)
{
    (void)user;
    g_system_config.volume += delta * 5;
    if(g_system_config.volume < 0) g_system_config.volume = 0;
    if(g_system_config.volume > 100) g_system_config.volume = 100;
    settings_system_changed();
}

static void settings_volume_action(void * user)
{
    (void)user;
    ui_enter_edit("UP/DOWN Adjust   ENTER Confirm   6 Cancel");
}

static void settings_update_np_bg_checks(void)
{
    if(settings_detail_np_bg_theme_check) {
        lv_label_set_text(settings_detail_np_bg_theme_check,
                          g_np_bg_mode == NP_BG_THEME ? ICON_CHECK : "");
    }
    if(settings_detail_np_bg_blur_check) {
        lv_label_set_text(settings_detail_np_bg_blur_check,
                          g_np_bg_mode == NP_BG_BLURRED_COVER ? ICON_CHECK : "");
    }
    if(settings_detail_np_bg_mesh_check) {
        lv_label_set_text(settings_detail_np_bg_mesh_check,
                          g_np_bg_mode == NP_BG_COVER_GRADIENT ? ICON_CHECK : "");
    }
}

static void settings_np_bg_action(void * user)
{
    int mode = (int)(intptr_t)user;
    if(mode < NP_BG_THEME || mode > NP_BG_COVER_GRADIENT) return;
    g_np_bg_mode = (np_bg_mode_t)mode;
    settings_update_np_bg_checks();
    settings_system_changed();
}

static const char * settings_sort_field_text(void)
{
    switch(g_sort_field) {
        case SORT_FIELD_NAME: return "Name";
        case SORT_FIELD_YEAR: return "Year";
        default: return "Add date";
    }
}

static const char * settings_sort_order_text(void)
{
    return g_sort_desc ? "Desc" : "Asc";
}

static void settings_update_sort_labels(void)
{
    if(settings_detail_sort_value) {
        lv_label_set_text(settings_detail_sort_value,
                          settings_sort_field_text());
    }
    if(settings_detail_order_value) {
        lv_label_set_text(settings_detail_order_value,
                          settings_sort_order_text());
    }
}

static void settings_time_format_action(void * user)
{
    LV_UNUSED(user);
    g_clock_12h = !g_clock_12h;
    settings_update_time_format_labels();
    update_clock();
}

static void settings_time_apply_blink(void);

static void settings_time_segment_focus_cb(void * user)
{
    settings_time_segment = (int)(intptr_t)user;
    settings_time_editing = false;
    settings_time_blink_on = true;
    settings_time_apply_blink();
}

static void settings_time_start_edit(void * user)
{
    settings_time_segment = (int)(intptr_t)user;
    settings_time_editing = true;
    settings_time_blink_on = true;
    settings_time_apply_blink();
}

static void settings_time_adjust(int direction)
{
    if(settings_time_segment == SETTINGS_TIME_HOUR) {
        settings_clock_adjust_minutes(direction * 60);
    }
    else if(settings_time_segment == SETTINGS_TIME_MINUTE) {
        settings_clock_adjust_minutes(direction);
    }
    else if(settings_time_segment == SETTINGS_TIME_AMPM) {
        int hour;
        int minute;
        settings_current_time(&hour, &minute);
        bool is_pm = hour >= 12;
        bool target_pm = direction < 0; /* up = AM, down = PM */
        if(target_pm != is_pm) {
            settings_clock_adjust_minutes(target_pm ? 12 * 60 : -12 * 60);
        }
    }
}

static bool settings_key(ui_key_t key)
{
    if(!settings_time_open) return false;
    if(!settings_time_editing) {
        if(key == UI_KEY_ENTER) {
            settings_time_start_edit((void *)(intptr_t)settings_time_segment);
            return true;
        }
        return false; /* Up/Down move focus between Back/Hour/Minute/AM-PM */
    }
    switch(key) {
        case UI_KEY_UP:
            settings_time_adjust(1);
            return true;
        case UI_KEY_DOWN:
            settings_time_adjust(-1);
            return true;
        case UI_KEY_ENTER:
            settings_time_editing = false;
            settings_time_apply_blink();
            return true;
        default:
            return false;
    }
}

static void settings_time_up_cb(lv_event_t * event)
{
    if(settings_time_editing) {
        settings_time_adjust(1);
    }
    else {
        focus_move(-1);
    }
    lv_event_stop_bubbling(event);
}

static void settings_time_down_cb(lv_event_t * event)
{
    if(settings_time_editing) {
        settings_time_adjust(-1);
    }
    else {
        focus_move(1);
    }
    lv_event_stop_bubbling(event);
}

static void settings_time_apply_blink(void)
{
    bool edit = settings_time_editing;
    if(settings_time_hour_value) {
        bool on = settings_time_segment == SETTINGS_TIME_HOUR &&
                  (!edit || settings_time_blink_on);
        lv_obj_set_style_bg_opa(settings_time_hour_value,
                                on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if(settings_time_minute_value) {
        bool on = settings_time_segment == SETTINGS_TIME_MINUTE &&
                  (!edit || settings_time_blink_on);
        lv_obj_set_style_bg_opa(settings_time_minute_value,
                                on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if(settings_time_ampm_value) {
        bool on = settings_time_segment == SETTINGS_TIME_AMPM &&
                  (!edit || settings_time_blink_on);
        lv_obj_set_style_bg_opa(settings_time_ampm_value,
                                on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void settings_time_blink_tick(void)
{
    if(g_current_screen != SCR_SETTINGS || !settings_time_open ||
       !settings_time_editing || settings_time_segment == SETTINGS_TIME_NONE) {
        return;
    }
    settings_time_blink_on = !settings_time_blink_on;
    settings_time_apply_blink();
}

static lv_obj_t * settings_time_touch_button(lv_obj_t * parent,
                                             const char * icon,
                                             lv_event_cb_t callback)
{
    lv_obj_t * button = lv_obj_create(parent);
    lv_obj_set_size(button, 64, 56);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_ACCENT_DARK), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_clickable(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    make_label(button, icon, &bootstrap_icons_32, COL_MINI_FG);
    return button;
}

static void settings_render_detail(lv_obj_t * parent, bool interactive);
static void settings_render_time_editor(lv_obj_t * parent, bool interactive)
{
    LV_UNUSED(interactive);
    settings_detail_set_time_value = NULL;
    settings_time_hour_value = NULL;
    settings_time_minute_value = NULL;
    settings_time_ampm_value = NULL;

    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    lv_obj_t * title = make_label(head, "Set time", FONT_DETAIL_TITLE, COL_TEXT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    make_label(parent, "UP/DOWN select  ENTER edit/confirm  DOWN next",
               FONT_CAPTION, COL_MUTED);

    lv_obj_t * buttons = lv_obj_create(parent);
    lv_obj_set_size(buttons, LV_PCT(100), 64);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(buttons, 0, 0);
    lv_obj_set_style_pad_all(buttons, 0, 0);
    lv_obj_set_style_pad_column(buttons, 16, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(buttons, false);
    settings_time_touch_button(buttons, ICON_CHEVRON_UP, settings_time_up_cb);
    settings_time_touch_button(buttons, ICON_CHEVRON_DOWN, settings_time_down_cb);

    lv_obj_t * time_wrap = lv_obj_create(parent);
    lv_obj_set_size(time_wrap, LV_PCT(100), 88);
    lv_obj_set_style_bg_opa(time_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_wrap, 0, 0);
    lv_obj_set_style_pad_all(time_wrap, 0, 0);
    lv_obj_set_flex_flow(time_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(time_wrap, false);

    lv_obj_t * time_box = lv_obj_create(time_wrap);
    lv_obj_set_size(time_box, LV_SIZE_CONTENT, 80);
    lv_obj_set_style_bg_opa(time_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_box, 2, 0);
    lv_obj_set_style_border_color(time_box, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_opa(time_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(time_box, 0, 0);
    lv_obj_set_style_pad_all(time_box, 0, 0);
    lv_obj_set_style_pad_column(time_box, 14, 0);
    lv_obj_set_flex_flow(time_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(time_box, false);

    settings_time_hour_value = make_label(time_box, "00", FONT_CLOCK_LG, COL_TEXT);
    lv_obj_set_size(settings_time_hour_value, 110, 76);
    lv_obj_set_style_pad_top(settings_time_hour_value, 16, 0);
    lv_obj_set_style_text_align(settings_time_hour_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(settings_time_hour_value, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_bg_opa(settings_time_hour_value, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(settings_time_hour_value, 0, 0);
    int hour_focus = focus_add_clickable(settings_time_hour_value,
                                         settings_time_start_edit, NULL,
                                         (void *)(intptr_t)SETTINGS_TIME_HOUR,
                                         false);
    focus_set_silent(hour_focus, true);
    focus_set_on_focus(hour_focus, settings_time_segment_focus_cb);

    make_label(time_box, ":", FONT_CLOCK_LG, COL_TEXT);

    settings_time_minute_value = make_label(time_box, "00", FONT_CLOCK_LG, COL_TEXT);
    lv_obj_set_size(settings_time_minute_value, 110, 76);
    lv_obj_set_style_pad_top(settings_time_minute_value, 16, 0);
    lv_obj_set_style_text_align(settings_time_minute_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(settings_time_minute_value, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_bg_opa(settings_time_minute_value, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(settings_time_minute_value, 0, 0);
    int minute_focus = focus_add_clickable(settings_time_minute_value,
                                           settings_time_start_edit, NULL,
                                           (void *)(intptr_t)SETTINGS_TIME_MINUTE,
                                           false);
    focus_set_silent(minute_focus, true);
    focus_set_on_focus(minute_focus, settings_time_segment_focus_cb);

    if(g_clock_12h) {
        settings_time_ampm_value = make_label(time_box, "AM", FONT_CLOCK, COL_TEXT);
        lv_obj_set_size(settings_time_ampm_value, 80, 76);
        lv_obj_set_style_pad_top(settings_time_ampm_value, 21, 0);
        lv_obj_set_style_text_align(settings_time_ampm_value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(settings_time_ampm_value, lv_color_hex(COL_FOCUS), 0);
        lv_obj_set_style_bg_opa(settings_time_ampm_value, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(settings_time_ampm_value, 0, 0);
        int ampm_focus = focus_add_clickable(settings_time_ampm_value,
                                             settings_time_start_edit, NULL,
                                             (void *)(intptr_t)SETTINGS_TIME_AMPM,
                                             false);
        focus_set_silent(ampm_focus, true);
        focus_set_on_focus(ampm_focus, settings_time_segment_focus_cb);
    }

    settings_update_time_labels();
    settings_time_apply_blink();
}

static void settings_open_time_action(void * user)
{
    LV_UNUSED(user);
    settings_detail_open = true;
    settings_time_open = true;
    settings_time_editing = false;
    settings_time_segment = SETTINGS_TIME_NONE;
    settings_time_blink_on = true;
    settings_rebuild();
    if(g_focus_count > 0) focus_select(0);
}

static void settings_open_time_async(void * arg)
{
    LV_UNUSED(arg);
    settings_open_time_action(NULL);
}

static void settings_open_time_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(settings_open_time_async, NULL);
}

static void settings_touch_action(void * user)
{
    LV_UNUSED(user);
    ui_set_touch_enabled(!ui_touch_enabled());
    settings_update_touch_labels();
    settings_system_changed();
}

static void settings_menubar_action(void * user)
{
    LV_UNUSED(user);
    ui_set_menubar_left(!ui_menubar_left());
    settings_update_menubar_labels();
    settings_system_changed();
}

static void settings_brightness_adjust(int delta, void * user)
{
    LV_UNUSED(user);
    settings_brightness += delta * 5;
    if(settings_brightness < 0) settings_brightness = 0;
    if(settings_brightness > 100) settings_brightness = 100;
    settings_update_brightness_labels();
    settings_system_changed();
}

static void settings_brightness_action(void * user)
{
    LV_UNUSED(user);
    ui_enter_edit("UP/DOWN Adjust   ENTER Confirm   6 Cancel");
}

static void settings_sort_field_action(void * user)
{
    LV_UNUSED(user);
    g_sort_field = (g_sort_field + 1) % SORT_FIELD_COUNT;
    settings_update_sort_labels();
    settings_system_changed();
}

static void settings_sort_order_action(void * user)
{
    LV_UNUSED(user);
    g_sort_desc = !g_sort_desc;
    settings_update_sort_labels();
    settings_system_changed();
}

static lv_obj_t * settings_make_value_row(lv_obj_t * parent, const char * label,
                                          const char * value, uint32_t color,
                                          int height, const lv_font_t * label_font,
                                          const lv_font_t * value_font,
                                          lv_obj_t ** value_out)
{
    lv_obj_t * row = make_row(parent, height);
    lv_obj_set_style_pad_left(row, 16, 0);
    make_label(row, label, label_font, COL_TEXT);
    lv_obj_t * spacer = lv_obj_create(row);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_t * value_label = make_label(row, value, value_font, color);
    if(value_out) *value_out = value_label;
    return row;
}

static lv_obj_t * settings_make_choice_row(lv_obj_t * parent, const char * label,
                                           bool selected,
                                           lv_obj_t ** check_out)
{
    lv_obj_t * row = make_row(parent, 64);
    lv_obj_set_style_pad_left(row, 16, 0);
    make_label(row, label, FONT_H2, COL_TEXT);
    lv_obj_t * spacer = lv_obj_create(row);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);

    lv_obj_t * check = make_label(row, selected ? ICON_CHECK : "",
                                  &bootstrap_icons_24, COL_ACCENT);
    lv_obj_set_width(check, 32);
    lv_obj_set_style_text_align(check, LV_TEXT_ALIGN_CENTER, 0);
    if(check_out) *check_out = check;
    return row;
}

static void settings_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(settings_detail_dirty && settings_right_content != NULL) {
        settings_detail_dirty = false;
        settings_render_detail(settings_right_content, false);
    }
}

static void settings_schedule_detail(void)
{
    settings_detail_dirty = true;
    if(settings_detail_timer == NULL) {
        settings_detail_timer = lv_timer_create(settings_detail_timer_cb, 140,
                                                NULL);
        if(settings_detail_timer == NULL) return;
        lv_timer_pause(settings_detail_timer);
    }
    lv_timer_reset(settings_detail_timer);
    lv_timer_resume(settings_detail_timer);
}

static void settings_row_focus(void * user)
{
    int index = (int)(intptr_t)user;
    if(index == settings_selected) return;
    settings_selected = index;
    settings_time_open = false;
    settings_update_count();
    if(settings_right_content) settings_schedule_detail();
}

static void settings_rebuild(void)
{
    ui_rebuild_page();
}

static void settings_open(void * user)
{
    int index = (int)(intptr_t)user;
    if(index < 0 || index >= SETTING_COUNT) return;
    settings_selected = index;
    settings_time_open = false;
    settings_update_count();
    settings_detail_open = true;
    settings_rebuild();
    if(g_focus_count > 0) {
        focus_select(settings_initial_focus(index));
    }
}

static bool settings_back(void)
{
    if(settings_time_open) {
        settings_time_open = false;
        settings_rebuild();
        if(g_focus_count > 0) focus_select(1);
        return true;
    }
    if(settings_detail_open) {
        settings_detail_open = false;
        settings_rebuild();
        return true;
    }
    return false;
}

static void settings_same_key(void)
{
    if(settings_time_open) {
        settings_time_open = false;
        settings_rebuild();
        if(g_focus_count > 0) focus_select(1);
        return;
    }
    if(settings_detail_open) {
        settings_detail_open = false;
        settings_rebuild();
        return;
    }
    settings_selected = 0;
    settings_update_count();
    if(g_focus_count > 0) focus_select(0);
}

static void settings_open_async(void * arg)
{
    settings_open(arg);
}

static void settings_row_touch_cb(lv_event_t * event)
{
    ui_call_async(settings_open_async, lv_event_get_user_data(event));
}

static void settings_ble_open_action(void * user)
{
    LV_UNUSED(user);
    g_ble_return_screen = SCR_SETTINGS;
    ui_show_screen(SCR_BLUETOOTH);
}

static void settings_ble_open_async(void * arg)
{
    LV_UNUSED(arg);
    settings_ble_open_action(NULL);
}

static void settings_ble_open_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(settings_ble_open_async, NULL);
}

static void settings_time_format_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_time_format_action(NULL);
}

static void settings_touch_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_touch_action(NULL);
}

static void settings_menubar_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_menubar_action(NULL);
}

static void settings_brightness_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_brightness_action(NULL);
}

static void settings_screen_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_screen_action(NULL);
}

static void settings_wifi_mode_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_wifi_mode_action(NULL);
}

static void settings_sound_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_sound_action(NULL);
}

static void settings_volume_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_volume_action(NULL);
}

static void settings_np_bg_touch_cb(lv_event_t * event)
{
    settings_np_bg_action(lv_event_get_user_data(event));
}

static void settings_sort_field_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_sort_field_action(NULL);
}

static void settings_sort_order_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_sort_order_action(NULL);
}

static void settings_format_bytes(uint64_t bytes, char * out, size_t out_len)
{
    if(bytes >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(out, out_len, "%.1f GB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    else if(bytes >= (uint64_t)1024 * 1024) {
        snprintf(out, out_len, "%.1f MB",
                 (double)bytes / (1024.0 * 1024.0));
    }
    else if(bytes >= 1024) {
        snprintf(out, out_len, "%.1f KB", (double)bytes / 1024.0);
    }
    else {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    }
}

static void settings_afc_read_status(ui_afc_status_t * out)
{
    memset(out, 0, sizeof(*out));
    if(g_afc_status_cb != NULL) g_afc_status_cb(out);
}

static void settings_afc_action(void * user)
{
    if(g_afc_action_cb != NULL) {
        g_afc_action_cb((int)(intptr_t)user);
    }
}

static void settings_afc_action_async(void * arg)
{
    settings_afc_action(arg);
}

static void settings_afc_touch_cb(lv_event_t * event)
{
    ui_call_async(settings_afc_action_async, lv_event_get_user_data(event));
}

static lv_obj_t * settings_afc_action_row(lv_obj_t * parent, const char * label,
                                          const char * value, int action,
                                          bool interactive)
{
    lv_obj_t * row = settings_make_value_row(parent, label, value, COL_ACCENT,
                                             60, FONT_H2, FONT_CAPTION, NULL);
    if(interactive) {
        focus_add_clickable(row, settings_afc_action, NULL,
                            (void *)(intptr_t)action, true);
    }
    else {
        touch_add_clickable(row, settings_afc_touch_cb,
                            (void *)(intptr_t)action);
    }
    return row;
}

static void settings_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);

    settings_detail_time_format_value = NULL;
    settings_detail_set_time_value = NULL;
    settings_detail_screen_value = NULL;
    settings_detail_wifi_mode_value = NULL;
    settings_detail_wifi_status_value = NULL;
    settings_detail_sound_value = NULL;
    settings_detail_volume_value = NULL;
    settings_time_hour_value = NULL;
    settings_time_minute_value = NULL;
    settings_detail_touch_value = NULL;
    settings_detail_menubar_value = NULL;
    settings_detail_brightness_value = NULL;
    settings_detail_sort_value = NULL;
    settings_detail_order_value = NULL;
    settings_detail_np_bg_theme_check = NULL;
    settings_detail_np_bg_blur_check = NULL;
    settings_detail_np_bg_mesh_check = NULL;

    if(settings_time_open) {
        settings_render_time_editor(parent, interactive);
        return;
    }

    int index = settings_selected;
    if(index < 0 || index >= SETTING_COUNT) index = 0;

    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    lv_obj_t * title = make_label(head, settings_names[index],
                                  FONT_DETAIL_TITLE, COL_TEXT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);

    switch(index) {
        case SETTING_BLUETOOTH: {
            ble_remote_state_t state = BLE_REMOTE_OFF;
            char status[64] = "Off";
            ble_remote_status(&state, status, sizeof(status));
            lv_obj_t * open_row = settings_make_value_row(
                parent, "Bluetooth Remote", "Open", COL_ACCENT, 64,
                FONT_H2, FONT_H2, NULL);
            if(interactive) {
                focus_add_clickable(open_row, settings_ble_open_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(open_row, settings_ble_open_touch_cb, NULL);
            }
            settings_make_value_row(parent, "Status", status,
                                    state == BLE_REMOTE_CONNECTED ? COL_GREEN
                                                                  : COL_MUTED,
                                    52, FONT_H2, FONT_CAPTION, NULL);
            char paired[32];
            snprintf(paired, sizeof(paired), "%d", ble_remote_paired_count());
            settings_make_value_row(parent, "Paired devices", paired, COL_TEXT,
                                    52, FONT_H2, FONT_NUM, NULL);
            break;
        }
        case SETTING_GENERAL: {
            lv_obj_t * time_row = settings_make_value_row(
                parent, "Time format", g_clock_12h ? "12H" : "24H",
                COL_TEXT, 64, FONT_H2, FONT_H2,
                &settings_detail_time_format_value);
            if(interactive) {
                focus_add_clickable(time_row, settings_time_format_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(time_row, settings_time_format_touch_cb, NULL);
            }

            char time_text[16];
            settings_format_time(time_text, sizeof(time_text));
            lv_obj_t * set_time_row = settings_make_value_row(
                parent, "Set time", time_text, COL_TEXT, 64,
                FONT_H2, FONT_H2, &settings_detail_set_time_value);
            if(interactive) {
                focus_add_clickable(set_time_row, settings_open_time_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(set_time_row, settings_open_time_touch_cb, NULL);
            }

            lv_obj_t * sort_row = settings_make_value_row(
                parent, "Sort order", settings_sort_field_text(), COL_TEXT, 64,
                FONT_H2, FONT_H2, &settings_detail_sort_value);
            if(interactive) {
                focus_add_clickable(sort_row, settings_sort_field_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(sort_row, settings_sort_field_touch_cb, NULL);
            }

            lv_obj_t * order_row = settings_make_value_row(
                parent, "Order", settings_sort_order_text(), COL_TEXT, 64,
                FONT_H2, FONT_H2, &settings_detail_order_value);
            if(interactive) {
                focus_add_clickable(order_row, settings_sort_order_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(order_row, settings_sort_order_touch_cb, NULL);
            }

            lv_obj_t * menubar_row = settings_make_value_row(
                parent, "Menubar position", ui_menubar_left() ? "Left" : "Right",
                COL_TEXT, 64, FONT_H2, FONT_H2,
                &settings_detail_menubar_value);
            if(interactive) {
                focus_add_clickable(menubar_row, settings_menubar_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(menubar_row, settings_menubar_touch_cb, NULL);
            }

            lv_obj_t * touch_row = settings_make_value_row(
                parent, "Touch input", ui_touch_enabled() ? "On" : "Off",
                COL_TEXT, 64, FONT_H2, FONT_H2,
                &settings_detail_touch_value);
            if(interactive) {
                focus_add_clickable(touch_row, settings_touch_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(touch_row, settings_touch_touch_cb, NULL);
            }

            char brightness_value[8];
            snprintf(brightness_value, sizeof(brightness_value), "%d%%",
                     settings_brightness);
            lv_obj_t * brightness_row = settings_make_value_row(
                parent, "Brightness", brightness_value, COL_TEXT, 64,
                FONT_H2, FONT_H2, &settings_detail_brightness_value);
            if(interactive) {
                focus_add_clickable(brightness_row, settings_brightness_action,
                                    settings_brightness_adjust, NULL, true);
            }
            else {
                touch_add_clickable(brightness_row, settings_brightness_touch_cb,
                                    NULL);
            }

            lv_obj_t * screen_row = settings_make_value_row(
                parent, "Screen flip",
                g_system_config.screen_flip == 180 ? "180 deg" : "0 deg",
                COL_TEXT, 64, FONT_H2, FONT_H2,
                &settings_detail_screen_value);
            if(interactive) {
                focus_add_clickable(screen_row, settings_screen_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(screen_row, settings_screen_touch_cb, NULL);
            }

            char volume_value[8];
            snprintf(volume_value, sizeof(volume_value), "%d%%",
                     g_system_config.volume);
            lv_obj_t * volume_row = settings_make_value_row(
                parent, "Prompt volume", volume_value, COL_TEXT, 64,
                FONT_H2, FONT_H2, &settings_detail_volume_value);
            if(interactive) {
                focus_add_clickable(volume_row, settings_volume_action,
                                    settings_volume_adjust, NULL, true);
            }
            else {
                touch_add_clickable(volume_row, settings_volume_touch_cb, NULL);
            }

            lv_obj_t * sound_row = settings_make_value_row(
                parent, "Prompt sound",
                g_system_config.sound_enabled ? "On" : "Off",
                COL_TEXT, 64, FONT_H2, FONT_H2,
                &settings_detail_sound_value);
            if(interactive) {
                focus_add_clickable(sound_row, settings_sound_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(sound_row, settings_sound_touch_cb, NULL);
            }

            lv_obj_t * wifi_row = settings_make_value_row(
                parent, "WiFi mode",
                g_system_config.wifi_mode == 1 ? "AP" : "Station",
                COL_TEXT, 64, FONT_H2, FONT_H2,
                &settings_detail_wifi_mode_value);
            if(interactive) {
                focus_add_clickable(wifi_row, settings_wifi_mode_action,
                                    NULL, NULL, true);
            }
            else {
                touch_add_clickable(wifi_row, settings_wifi_mode_touch_cb, NULL);
            }

            char wifi_text[128];
            snprintf(wifi_text, sizeof(wifi_text), "%s  %s",
                     g_system_config.wifi_status, g_system_config.wifi_ip);
            settings_make_value_row(parent, "WiFi status", wifi_text,
                                    COL_MUTED, 52, FONT_H2, FONT_CAPTION,
                                    &settings_detail_wifi_status_value);
            break;
        }
        case SETTING_THEME:
            break;
        case SETTING_NOW_PLAYING_BG: {
            lv_obj_t * theme_row = settings_make_choice_row(
                parent, "Theme", g_np_bg_mode == NP_BG_THEME,
                &settings_detail_np_bg_theme_check);
            if(interactive) {
                focus_add_clickable(theme_row, settings_np_bg_action, NULL,
                                    (void *)(intptr_t)NP_BG_THEME, true);
            }
            else {
                touch_add_clickable(theme_row, settings_np_bg_touch_cb,
                                    (void *)(intptr_t)NP_BG_THEME);
            }

            lv_obj_t * blur_row = settings_make_choice_row(
                parent, "Blurred cover",
                g_np_bg_mode == NP_BG_BLURRED_COVER,
                &settings_detail_np_bg_blur_check);
            if(interactive) {
                focus_add_clickable(blur_row, settings_np_bg_action, NULL,
                                    (void *)(intptr_t)NP_BG_BLURRED_COVER, true);
            }
            else {
                touch_add_clickable(blur_row, settings_np_bg_touch_cb,
                                    (void *)(intptr_t)NP_BG_BLURRED_COVER);
            }

            lv_obj_t * mesh_row = settings_make_choice_row(
                parent, "Cover gradient",
                g_np_bg_mode == NP_BG_COVER_GRADIENT,
                &settings_detail_np_bg_mesh_check);
            if(interactive) {
                focus_add_clickable(mesh_row, settings_np_bg_action, NULL,
                                    (void *)(intptr_t)NP_BG_COVER_GRADIENT, true);
            }
            else {
                touch_add_clickable(mesh_row, settings_np_bg_touch_cb,
                                    (void *)(intptr_t)NP_BG_COVER_GRADIENT);
            }
            break;
        }
        case SETTING_AFC: {
            ui_afc_status_t st;
            settings_afc_read_status(&st);
            if(!st.available) {
                settings_make_value_row(parent, "Apple File Conduit Sync",
                                        "Unavailable in simulator",
                                        COL_MUTED, 60, FONT_H2,
                                        FONT_CAPTION, NULL);
                break;
            }

            settings_afc_action_row(parent, "AFC mode",
                                    st.afc_mode ? "Reconnect"
                                                : "Switch to AFC",
                                    UI_AFC_ACTION_PAIR, interactive);
            settings_afc_action_row(parent, "Switch to iAP2",
                                    st.afc_mode ? "Reboot" : "Already iAP2",
                                    UI_AFC_ACTION_IPOD_MODE, interactive);

            char value[96];
            char size[24];
            int total_albums = player_album_count();
            if(total_albums > 0) {
                snprintf(value, sizeof(value), "%d / %d",
                         st.cached_albums, total_albums);
            }
            else {
                snprintf(value, sizeof(value), "%d", st.cached_albums);
            }
            settings_make_value_row(parent, "Cached albums", value, COL_TEXT,
                                    52, FONT_H2, FONT_NUM, NULL);

            uint64_t total_bytes = st.thumb_bytes + st.cover_bytes +
                                   st.original_bytes;
            settings_format_bytes(total_bytes, size, sizeof(size));
            settings_make_value_row(parent, "Cache size", size, COL_TEXT,
                                    52, FONT_H2, FONT_NUM, NULL);

            settings_format_bytes(st.thumb_bytes, size, sizeof(size));
            snprintf(value, sizeof(value), "%d files, %s",
                     st.thumb_count, size);
            settings_make_value_row(parent, "Thumbnails", value, COL_MUTED,
                                    52, FONT_H2, FONT_CAPTION, NULL);

            settings_make_value_row(parent, "USB mode",
                                    st.afc_mode ? "AFC (config 4)"
                                                : "iAP2 (config 2)",
                                    st.afc_mode ? COL_ACCENT : COL_GREEN,
                                    52, FONT_H2, FONT_CAPTION, NULL);

            settings_make_value_row(parent, "AFC status",
                                    st.status[0] ? st.status : "idle",
                                    st.connected ? COL_GREEN : COL_MUTED,
                                    52, FONT_H2, FONT_CAPTION, NULL);
            break;
        }
        case SETTING_USB: {
            settings_make_value_row(parent, "USB-A host", "Active (HS)",
                                    COL_GREEN, 52, FONT_H2, FONT_CAPTION, NULL);
            settings_make_value_row(parent, "USB-C data", "Device / console",
                                    COL_TEXT, 52, FONT_H2, FONT_CAPTION, NULL);
            settings_make_value_row(parent, "USB-C OTG host",
                                    "Requires board rework", COL_MUTED,
                                    52, FONT_H2, FONT_CAPTION, NULL);
            settings_make_value_row(parent, "Console", "USB Serial/JTAG",
                                    COL_TEXT, 52, FONT_H2, FONT_CAPTION, NULL);
            break;
        }
        case SETTING_DEVICE: {
            player_snapshot_t snapshot;
            player_get_snapshot(&snapshot);
            settings_make_value_row(parent, "Device", snapshot.device_name,
                                    COL_TEXT, 52, FONT_H2, FONT_H2, NULL);
            char battery[32];
            if(snapshot.battery < 0) {
                snprintf(battery, sizeof(battery), "--%%");
            }
            else {
                snprintf(battery, sizeof(battery), "%d%%", snapshot.battery);
            }
            settings_make_value_row(parent, "Battery", battery, COL_TEXT, 52,
                                    FONT_H2, FONT_H2, NULL);
            settings_make_value_row(parent, "Connection",
                                    snapshot.connected ? "Connected" : "Disconnected",
                                    snapshot.connected ? COL_GREEN : COL_AMBER,
                                    52, FONT_H2, FONT_H2, NULL);
            break;
        }
        case SETTING_BACKEND: {
            player_snapshot_t snapshot;
            player_get_snapshot(&snapshot);
#if defined(ESP_PLATFORM)
            const char * backend_name = snapshot.real_device
                                            ? "iAP2 (USB)" : "AFC (USB)";
#else
            const char * backend_name = snapshot.real_device
                                            ? "iAP2 (USB)" : "Mock library";
#endif
            settings_make_value_row(parent, "Backend", backend_name,
                                    COL_TEXT, 52, FONT_H2, FONT_H2, NULL);
            settings_make_value_row(parent, "iAP2",
                                    snapshot.real_device
                                        ? (snapshot.connected
                                               ? "Connected" : "Waiting")
                                        : "Not connected",
                                    snapshot.real_device && snapshot.connected
                                        ? COL_GREEN : COL_MUTED,
                                    52, FONT_H2, FONT_H2, NULL);
            break;
        }
        case SETTING_ABOUT:
            settings_make_value_row(parent, "Model", "IAP2 Tab5",
                                    COL_TEXT, 52, FONT_H2, FONT_H2, NULL);
            settings_make_value_row(parent, "Version", "v0.1",
                                    COL_TEXT, 52, FONT_H2, FONT_H2, NULL);
            settings_make_value_row(parent, "UI", "LVGL 9.6 / SDL2",
                                    COL_MUTED, 52, FONT_H2, FONT_H2, NULL);
            break;
        default:
            break;
    }
}

static void settings_create(lv_obj_t * parent)
{
    settings_right_content = NULL;
    settings_count_label = NULL;
    settings_detail_time_format_value = NULL;
    settings_detail_set_time_value = NULL;
    settings_time_hour_value = NULL;
    settings_time_minute_value = NULL;
    settings_detail_touch_value = NULL;
    settings_detail_menubar_value = NULL;
    settings_detail_brightness_value = NULL;
    settings_detail_sort_value = NULL;
    settings_detail_order_value = NULL;
    settings_detail_np_bg_theme_check = NULL;
    settings_detail_np_bg_blur_check = NULL;
    settings_detail_np_bg_mesh_check = NULL;

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(root, false);

    lv_obj_t * left = make_panel(root);
    lv_obj_set_size(left, SIDE_PANEL_W, LV_PCT(100));
    lv_obj_set_style_pad_all(left, 18, 0);
    lv_obj_set_style_pad_row(left, 0, 0);

    lv_obj_t * head = lv_obj_create(left);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, "Settings", FONT_PAGE_TITLE, COL_TEXT);
    settings_count_label = make_label(head, "0/0", FONT_NUM, COL_MUTED);
    settings_update_count();

    lv_obj_t * spacer = lv_obj_create(left);
    lv_obj_set_size(spacer, LV_PCT(100), 8);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_set_clickable(spacer, false);

    for(int i = 0; i < SETTING_COUNT; i++) {
        lv_obj_t * row = make_row(left, 56);
        lv_obj_set_style_pad_left(row, 16, 0);
        make_label(row, settings_names[i], FONT_H2, COL_TEXT);

        if(i == settings_selected) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT_DARK), 0);
        }

        if(!settings_detail_open) {
            int focus_index = focus_add_clickable(row, settings_open, NULL,
                                                  (void *)(intptr_t)i, true);
            focus_set_on_focus(focus_index, settings_row_focus);
        }
        else {
            touch_add_clickable(row, settings_row_touch_cb, (void *)(intptr_t)i);
        }
    }

    lv_obj_t * right = make_panel(root);
    lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_all(right, 0, 0);
    settings_right_content = make_panel_content(right, 18);
    lv_obj_set_style_pad_row(settings_right_content, 10, 0);
    lv_obj_set_scroll_dir(settings_right_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_right_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_elastic(settings_right_content, true);
    lv_obj_set_scrollable(settings_right_content, true);
    settings_render_detail(settings_right_content, settings_detail_open);

    if(settings_detail_open) {
        if(settings_time_open) {
            focus_select(0);
        }
        else {
            focus_select(settings_initial_focus(settings_selected));
        }
        ui_set_hint("UP/DOWN Select   ENTER Change   7 Play/Pause");
    }
    else {
        focus_select(settings_selected);
        ui_set_hint("UP/DOWN Select   ENTER Open   7 Play/Pause");
    }
}

/* -------------------------------------------------------------------------
 * Page table
 * ------------------------------------------------------------------------- */
static const page_t g_pages[SCR_COUNT] = {
    [SCR_NOW_PLAYING] = {
        .create = np_create,
        .tick = np_tick,
        .back = np_back,
        .same_key = np_same_key,
    },
    [SCR_ALBUMS] = {
        .create = albums_create,
        .tick = NULL,
        .back = albums_back,
        .same_key = albums_same_key,
        .key = albums_key,
    },
    [SCR_ARTISTS] = {
        .create = artists_create,
        .tick = NULL,
        .back = artists_back,
        .same_key = artists_same_key,
        .key = artists_key,
    },
    [SCR_SONGS] = {
        .create = songs_create,
        .tick = songs_tick,
        .back = songs_back,
        .same_key = songs_same_key,
        .key = songs_key,
    },
    [SCR_PLAYLISTS] = {
        .create = playlists_create,
        .tick = NULL,
        .back = playlists_back,
        .same_key = playlists_same_key,
    },
    [SCR_BLUETOOTH] = {
        .create = ble_create,
        .tick = ble_tick,
        .back = ble_back,
        .same_key = ble_same_key,
        .key = ble_key,
    },
    [SCR_SETTINGS] = {
        .create = settings_create,
        .tick = NULL,
        .back = settings_back,
        .same_key = settings_same_key,
        .key = settings_key,
    },
};

/* -------------------------------------------------------------------------
 * Dedicated AFC sync screen (boot mode 2: no media controls)
 * ------------------------------------------------------------------------- */
static lv_obj_t * afc_sd_vendor;
static lv_obj_t * afc_sd_type;
static lv_obj_t * afc_sd_fs;
static lv_obj_t * afc_sd_usage;
static lv_obj_t * afc_usb_conn;
static lv_obj_t * afc_usb_mode;
static lv_obj_t * afc_usb_op;
static lv_obj_t * afc_lib_id;
static lv_obj_t * afc_lib_db;
static lv_obj_t * afc_lib_tracks;
static lv_obj_t * afc_lib_updated;
static lv_obj_t * afc_cache_size;
static lv_obj_t * afc_cache_thumbs;
static lv_obj_t * afc_phase_status[3];
static lv_obj_t * afc_pct_label;
static lv_obj_t * afc_progress_info;
static lv_obj_t * afc_progress_sub;
static lv_obj_t * afc_progress_album;
static lv_obj_t * afc_progress_artist;
static lv_obj_t * afc_progress_bar;

static void afc_set_text(lv_obj_t * label, const char * text)
{
    if(label != NULL) lv_label_set_text(label, text != NULL ? text : "");
}

static void afc_sync_update(void)
{
    ui_afc_status_t st;
    memset(&st, 0, sizeof(st));
    if(g_afc_status_cb != NULL) g_afc_status_cb(&st);

    if(!st.available) {
        afc_set_text(afc_sd_vendor, "Unavailable");
        afc_set_text(afc_sd_type, "—");
        afc_set_text(afc_sd_fs, "—");
        afc_set_text(afc_sd_usage, "—");
        afc_set_text(afc_usb_conn, "Unavailable");
        afc_set_text(afc_usb_mode, "—");
        afc_set_text(afc_usb_op, "—");
        afc_set_text(afc_lib_id, "—");
        afc_set_text(afc_lib_db, "—");
        afc_set_text(afc_lib_updated, "—");
        afc_set_text(afc_cache_size, "—");
        afc_set_text(afc_cache_thumbs, "—");
        for(int i = 0; i < 3; i++) afc_set_text(afc_phase_status[i], "—");
        afc_set_text(afc_pct_label, "--%");
        afc_set_text(afc_progress_info, "AFC backend unavailable");
        afc_set_text(afc_progress_sub, "");
        afc_set_text(afc_progress_album, "");
        afc_set_text(afc_progress_artist, "");
        lv_bar_set_value(afc_progress_bar, 0, LV_ANIM_OFF);
        return;
    }

    char value[128];
    char used[24];
    char total[24];

    afc_set_text(afc_sd_vendor,
                 st.sd_info_ready ? st.sd_vendor : "not mounted");
    afc_set_text(afc_sd_type, st.sd_info_ready ? st.sd_type : "—");
    afc_set_text(afc_sd_fs, st.sd_info_ready ? st.sd_fs : "—");
    settings_format_bytes(st.sd_used_bytes, used, sizeof(used));
    settings_format_bytes(st.sd_total_bytes, total, sizeof(total));
    snprintf(value, sizeof(value), "%s / %s", used, total);
    afc_set_text(afc_sd_usage, value);

    afc_set_text(afc_usb_conn,
                 st.connected ? "AFC ready" : st.usb_status);
    lv_obj_set_style_text_color(afc_usb_conn,
                                lv_color_hex(st.connected ? COL_GREEN
                                                          : COL_AMBER), 0);
    afc_set_text(afc_usb_mode, st.usb_mode);
    afc_set_text(afc_usb_op, st.usb_operation);
    afc_set_text(afc_lib_id,
                 st.library_suffix[0] != '\0' ? st.library_suffix : "—");

    snprintf(value, sizeof(value), "%d", st.library_albums);
    afc_set_text(afc_lib_db, value);
    snprintf(value, sizeof(value), "%d tracks", st.library_tracks);
    afc_set_text(afc_lib_tracks, value);
    afc_set_text(afc_lib_updated, st.library_updated);

    settings_format_bytes(st.cache_bytes, value, sizeof(value));
    afc_set_text(afc_cache_size, value);
    snprintf(value, sizeof(value), "%d files", st.thumb_count);
    afc_set_text(afc_cache_thumbs, value);

    int active_phase = st.job_active ? st.sync_phase : 0;
    for(int i = 0; i < 3; i++) {
        int phase = i + 1;
        const char * text = "—";
        uint32_t color = COL_MUTED;
        if(st.job_active) {
            if(active_phase > phase) {
                text = "done";
                color = COL_GREEN;
            }
            else if(active_phase == phase) {
                text = "running";
                color = COL_AMBER;
            }
            else {
                text = "waiting";
                color = COL_MUTED;
            }
        }
        afc_set_text(afc_phase_status[i], text);
        lv_obj_set_style_text_color(afc_phase_status[i],
                                    lv_color_hex(color), 0);
    }

    int permille = 0;
    if(st.sync_total > 0) {
        permille = st.sync_attempted * 1000 / st.sync_total;
        if(permille > 1000) permille = 1000;
    }
    lv_bar_set_value(afc_progress_bar, permille, LV_ANIM_OFF);
    snprintf(value, sizeof(value), "%d%%", permille / 10);
    afc_set_text(afc_pct_label, value);

    if(st.job_active) {
        switch(st.sync_phase) {
            case 1:
                snprintf(value, sizeof(value), "Database sync");
                break;
            case 2:
                snprintf(value, sizeof(value), "Album art cache %d / %d",
                         st.sync_attempted, st.sync_total);
                break;
            case 3:
                snprintf(value, sizeof(value), "Convert thumbnail %d / %d",
                         st.sync_attempted, st.sync_total);
                break;
            default:
                snprintf(value, sizeof(value), "Preparing…");
                break;
        }
    }
    else {
        snprintf(value, sizeof(value), "No sync running");
    }
    afc_set_text(afc_progress_info, value);

    snprintf(value, sizeof(value), "done %d   failed %d",
             st.sync_copied, st.sync_failed);
    afc_set_text(afc_progress_sub, value);

    if(st.sync_album[0] != '\0') {
        afc_set_text(afc_progress_album, st.sync_album);
        afc_set_text(afc_progress_artist, st.sync_artist);
    }
    else if(st.job_active) {
        afc_set_text(afc_progress_album, "Waiting for iPod / AFC");
        afc_set_text(afc_progress_artist, "");
    }
    else {
        afc_set_text(afc_progress_album, "Idle");
        afc_set_text(afc_progress_artist, "");
    }
}

static void afc_sync_tick_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    afc_sync_update();
}

static void afc_sync_action(void * user)
{
    if(g_afc_action_cb != NULL) {
        g_afc_action_cb((int)(intptr_t)user);
    }
}

static lv_obj_t * afc_sync_button(lv_obj_t * parent, const char * text,
                                  int action)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_set_size(button, 240, 56);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_PANEL_ALT), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_label(button, text, FONT_H2, COL_TEXT);
    focus_add_clickable(button, afc_sync_action, NULL,
                        (void *)(intptr_t)action, true);
    return button;
}

static void afc_section(lv_obj_t * parent, const char * text)
{
    lv_obj_t * label = make_label(parent, text, FONT_H2, COL_MUTED);
    lv_obj_set_style_pad_top(label, 6, 0);
}

static void afc_value_row(lv_obj_t * parent, const char * label,
                          const char * value, uint32_t color, int height,
                          const lv_font_t * value_font, lv_obj_t ** out)
{
    settings_make_value_row(parent, label, value, color, height,
                            FONT_CAPTION, value_font, out);
}

static void afc_phase_row(lv_obj_t * parent, const char * text,
                          lv_obj_t ** status_out)
{
    lv_obj_t * row = make_row(parent, 30);
    make_label(row, text, FONT_H2, COL_TEXT);
    lv_obj_t * spacer = lv_obj_create(row);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_t * status = make_label(row, "—", FONT_CAPTION, COL_MUTED);
    lv_obj_set_width(status, 88);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
    *status_out = status;
}

static void ui_afc_sync_build(lv_obj_t * screen)
{
    lv_obj_t * root = lv_obj_create(screen);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 28, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);

    make_label(root, "AFC Sync", FONT_H1, COL_TEXT);

    lv_obj_t * content = lv_obj_create(root);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_column(content, 16, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(content, false);

    /* SD card and USB */
    lv_obj_t * left = make_panel(content);
    lv_obj_set_width(left, 370);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_style_pad_all(left, 16, 0);
    lv_obj_set_style_pad_row(left, 4, 0);

    afc_section(left, "SD Card");
    afc_value_row(left, "Vendor", "—", COL_TEXT, 34, FONT_H2,
                  &afc_sd_vendor);
    afc_value_row(left, "Type", "—", COL_TEXT, 34, FONT_CAPTION,
                  &afc_sd_type);
    afc_value_row(left, "Filesystem", "—", COL_TEXT, 34, FONT_H2,
                  &afc_sd_fs);
    afc_value_row(left, "Used / Size", "—", COL_TEXT, 34, FONT_NUM,
                  &afc_sd_usage);

    afc_section(left, "USB");
    afc_value_row(left, "Connection", "—", COL_AMBER, 38, FONT_H2,
                  &afc_usb_conn);
    afc_value_row(left, "USB Mode", "—", COL_ACCENT, 34, FONT_CAPTION,
                  &afc_usb_mode);
    afc_value_row(left, "Operation", "idle", COL_TEXT, 34, FONT_H2,
                  &afc_usb_op);
    afc_value_row(left, "Device ID", "—", COL_TEXT, 34, FONT_NUM,
                  &afc_lib_id);

    /* Library and cache */
    lv_obj_t * middle = make_panel(content);
    lv_obj_set_width(middle, 350);
    lv_obj_set_height(middle, LV_PCT(100));
    lv_obj_set_style_pad_all(middle, 16, 0);
    lv_obj_set_style_pad_row(middle, 4, 0);

    afc_section(middle, "Library");
    afc_value_row(middle, "Database Item", "—", COL_TEXT, 36, FONT_H2,
                  &afc_lib_db);
    afc_value_row(middle, "Tracks", "—", COL_TEXT, 34, FONT_NUM,
                  &afc_lib_tracks);
    afc_value_row(middle, "Sort Basis", "date_added", COL_TEXT, 34, FONT_H2,
                  NULL);
    afc_value_row(middle, "Updated", "—", COL_TEXT, 34, FONT_NUM,
                  &afc_lib_updated);

    afc_section(middle, "Album Art Cache");
    afc_value_row(middle, "Cache Size", "—", COL_TEXT, 40, FONT_H2,
                  &afc_cache_size);
    afc_value_row(middle, "Thumbnails", "—", COL_TEXT, 34, FONT_NUM,
                  &afc_cache_thumbs);

    /* Sync phases and progress */
    lv_obj_t * right = make_panel(content);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_all(right, 18, 0);
    lv_obj_set_style_pad_row(right, 4, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    afc_section(right, "Sync");
    afc_phase_row(right, "1  Database sync", &afc_phase_status[0]);
    afc_phase_row(right, "2  Album art cache", &afc_phase_status[1]);
    afc_phase_row(right, "3  Convert thumbnail", &afc_phase_status[2]);

    afc_pct_label = make_label(right, "--%", FONT_H1, COL_TEXT);
    lv_obj_set_style_pad_top(afc_pct_label, 6, 0);

    afc_progress_bar = lv_bar_create(right);
    lv_obj_set_size(afc_progress_bar, LV_PCT(100), 20);
    lv_bar_set_range(afc_progress_bar, 0, 1000);
    lv_bar_set_value(afc_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(afc_progress_bar, lv_color_hex(COL_PANEL_ALT),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(afc_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(afc_progress_bar, lv_color_hex(COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(afc_progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(afc_progress_bar, 0, LV_PART_INDICATOR);

    afc_progress_info = make_label(right, "No sync running", FONT_H2,
                                   COL_TEXT);
    afc_progress_sub = make_label(right, "done 0   failed 0", FONT_CAPTION,
                                  COL_MUTED);
    afc_progress_album = make_label(right, "Idle", FONT_H2, COL_TEXT);
    lv_obj_set_width(afc_progress_album, LV_PCT(100));
    lv_label_set_long_mode(afc_progress_album, LV_LABEL_LONG_DOT);
    afc_progress_artist = make_label(right, "", FONT_CAPTION, COL_MUTED);
    lv_obj_set_width(afc_progress_artist, LV_PCT(100));
    lv_label_set_long_mode(afc_progress_artist, LV_LABEL_LONG_DOT);

    lv_obj_t * buttons = lv_obj_create(root);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, 64);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(buttons, 0, 0);
    lv_obj_set_style_pad_all(buttons, 0, 0);
    lv_obj_set_style_pad_column(buttons, 18, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(buttons, false);

    afc_sync_button(buttons, "Quick sync", UI_AFC_ACTION_QUICK_SYNC);
    afc_sync_button(buttons, "Full sync", UI_AFC_ACTION_FULL_SYNC);
    afc_sync_button(buttons, "Exit to iAP2", UI_AFC_ACTION_IPOD_MODE);

    ui_set_hint("UP/DOWN Select   ENTER Run");
    afc_sync_update();
    lv_timer_create(afc_sync_tick_cb, 500, NULL);
    if(g_focus_count > 0) focus_select(0);
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */
void ui_init(void)
{
    player_init();
    g_device_available = player_device_available();
    if(s_thumb_pack_dir[0] != '\0') {
        char path[160];
        snprintf(path, sizeof(path), "%s/thumbs.bin", s_thumb_pack_dir);
        ui_set_thumb_pack_path(path, player_library_album_count());
    }
    thumbs_init();
    fonts_init();

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_TEXT), 0);

    if(g_afc_sync_mode) {
        ui_afc_sync_build(screen);
        return;
    }

    build_menubar(screen);

    g_content = lv_obj_create(screen);
    lv_obj_set_style_bg_opa(g_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_content, 0, 0);
    lv_obj_set_style_radius(g_content, 0, 0);
    lv_obj_set_style_pad_all(g_content, 0, 0);
    lv_obj_set_scrollable(g_content, false);

    g_page = lv_obj_create(g_content);
    lv_obj_set_style_bg_color(g_page, lv_color_hex(COL_MAIN_BG), 0);
    lv_obj_set_style_bg_opa(g_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_page, 0, 0);
    lv_obj_set_style_radius(g_page, 0, 0);
    lv_obj_set_style_pad_all(g_page, 0, 0);
    lv_obj_set_scrollable(g_page, false);

    build_mini_now(g_content);
    ui_menubar_apply_side();

    g_current_screen = SCR_NOW_PLAYING;
    ui_layout_page_area();
    g_pages[SCR_NOW_PLAYING].create(g_page);
    ui_menu_update();
    ui_menu_play_update();
    player_set_change_callback(ui_player_changed);
    update_clock();
    update_status();

    lv_timer_create(ui_tick_cb, 500, NULL);
    lv_timer_create(settings_time_blink_timer_cb, 250, NULL);
}
