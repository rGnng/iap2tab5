#include "ui.h"
#include "theme.h"

#include "icons.h"
#include "fonts/inter_fonts.h"
#include "plug_art.h"
#include "player.h"
#include "ble_remote.h"
#include "thumb_cache_format.h"

#include "lvgl.h"
#include "src/widgets/label/lv_label_private.h"

/* TinyTTF is enabled in lv_conf.h. Some ESP-IDF component builds do not
 * propagate the public declaration through lvgl.h, so declare it here. */
lv_font_t * lv_tiny_ttf_create_data_ex(const void * data, size_t data_size,
                                       int32_t font_size,
                                       lv_font_kerning_t kerning,
                                       size_t cache_size);

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "dirent.h"
#include "sys/stat.h"
#include "unistd.h"
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
static const ui_theme_t * g_theme = &ui_themes[UI_THEME_GRAY];
#define COL_MENU_TOP_BG     (g_theme->menu_top_bg)
#define COL_MENU_TOP_FG     (g_theme->menu_top_fg)
#define COL_MENU_BODY_BG    (g_theme->menu_body_bg)
#define COL_MENU_BODY_FG    (g_theme->menu_body_fg)
#define COL_MINI_BG         (g_theme->mini_bg)
#define COL_MINI_FG         (g_theme->mini_fg)
#define COL_MINI_MUTED      (g_theme->mini_muted)
#define COL_MINI_DIM        (g_theme->mini_dim)
#define COL_MAIN_BG         (g_theme->bg)
#define COL_MAIN_FG         (g_theme->text)
#define COL_BG              (g_theme->bg)
#define COL_STATUS          (g_theme->bg)
#define COL_SURFACE         (g_theme->surface)
#define COL_PANEL           (g_theme->panel)
#define COL_PANEL_ALT       (g_theme->panel_alt)
#define COL_BORDER          (g_theme->border)
#define COL_TEXT            (g_theme->text)
#define COL_MUTED           (g_theme->muted)
#define COL_DIM             (g_theme->dim)
#define COL_ACCENT          (g_theme->accent)
#define COL_ACCENT_DARK     (g_theme->selection)
#define COL_FOCUS           (g_theme->focus)
#define COL_GREEN           (g_theme->green)
#define COL_AMBER           (g_theme->amber)
#define COL_RED             (g_theme->red)
#define COL_PURPLE          (g_theme->purple)
#define COL_CYAN            (g_theme->cyan)

#define LIST_ROW_GAP 4

#define SCREEN_W   1280
#define SCREEN_H   720
/* The menubar is 108 px wide: a 72 px clock strip on top and six 108 px
 * square menu tiles below (72 + 6 × 108 = 720).  The mini player uses the
 * same 72 px height as the clock strip. */
#define MENUBAR_TIME_H     72
#define MENUBAR_TILE_H     108
#define MENUBAR_W          108
#define MINI_NOW_H         MENUBAR_TIME_H
/* Shared master-detail split: Albums / Artists / Playlists / Settings. */
#define SIDE_PANEL_W       480
#define CONTENT_Y          0
#define CONTENT_W          (SCREEN_W - MENUBAR_W)
#define CONTENT_H          SCREEN_H

#define MAX_FOCUS 1024

/* Master-detail pages build a short track list in the right-hand preview.
 * Enter opens the detail, which shows every track. */
#define DETAIL_PREVIEW_MAX_TRACKS 15

/* Type scale: four proportional levels plus tabular numeric/clock faces.
 * The queue is a compact 54 px list, so it keeps its own smaller pair. */
static const lv_font_t * FONT_H1 = &inter_36;
static const lv_font_t * FONT_NP_TITLE = &inter_40;
static const lv_font_t * FONT_NP_ARTIST = &inter_32;
static const lv_font_t * FONT_NP_ALBUM = &inter_32;
static const lv_font_t * FONT_NP_TIME = &inter_tabular_24;
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

enum {
    SORT_FIELD_ADD_DATE = 0,
    SORT_FIELD_NAME,
    SORT_FIELD_YEAR,
    SORT_FIELD_COUNT,
};
static int g_sort_field = SORT_FIELD_ADD_DATE;
static bool g_sort_desc = true;

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
    UI_PERF_LOG("Japanese font memory: bytes=%ld free=%u largest=%u", size,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    jp_font_data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
#else
    jp_font_data = malloc((size_t)size);
#endif
    size_t font_read = jp_font_data != NULL ? fread(jp_font_data, 1, (size_t)size, f) : 0;
    if(jp_font_data == NULL || font_read != (size_t)size) {
        UI_PERF_LOG("Japanese font read failed: allocated=%d read=%u/%ld error=%d errno=%d",
            jp_font_data != NULL, (unsigned)font_read, size, ferror(f), errno);
        fclose(f);
        free(jp_font_data);
        jp_font_data = NULL;
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
    /* Artist and album share one 32 px face in the default Now Playing
     * layout so the two metadata lines read as a pair. */
    FONT_NP_ALBUM = FONT_NP_ARTIST;
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
static bool g_afc_sync_mode;

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
static void ui_page_teardown(void);
static bool ui_apply_theme(void);

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
    .sound_mode = 2,
    .volume = 65,
    .wifi_status = "WiFi off",
    .wifi_ip = "--",
};
static void (*g_system_config_changed_cb)(const ui_system_config_t * cfg);
static void (*g_feedback_cb)(int type);
static bool g_key_feedback_scope;
static bool g_key_feedback_sent;
static void (*g_afc_status_cb)(ui_afc_status_t * out);
static void (*g_afc_action_cb)(int action);
static void (*g_time_changed_cb)(time_t now);

/* Key Feedback: 0 = none, 1 = touch only, 2 = touch + key. */
static void ui_feedback_emit(ui_feedback_t type, bool from_key)
{
    int mode = g_system_config.sound_mode;
    if(!g_system_config.sound_enabled || mode <= 0) return;
    if(from_key && mode < 2) return;
    if(g_key_feedback_scope && g_key_feedback_sent) return;
    if(g_feedback_cb != NULL) {
        if(g_key_feedback_scope) g_key_feedback_sent = true;
        g_feedback_cb((int)type);
    }
}

void ui_play_key_feedback(ui_key_t key)
{
    switch(key) {
        case UI_KEY_BACK:
        case UI_KEY_6:
            ui_feedback_emit(UI_FEEDBACK_BACK, true);
            break;
        case UI_KEY_ENTER:
        case UI_KEY_7:
        case UI_KEY_STAR:
            ui_feedback_emit(UI_FEEDBACK_ACTIVATE, true);
            break;
        case UI_KEY_1:
        case UI_KEY_2:
        case UI_KEY_3:
        case UI_KEY_4:
        case UI_KEY_5:
        case UI_KEY_CONTEXT:
        case UI_KEY_SCREEN_PREV:
        case UI_KEY_SCREEN_NEXT:
        case UI_KEY_PREV_TRACK:
        case UI_KEY_NEXT_TRACK:
        case UI_KEY_VOLUME_UP:
        case UI_KEY_VOLUME_DOWN:
        case UI_KEY_MUTE:
            ui_feedback_emit(UI_FEEDBACK_CLICK, true);
            break;
        default:
            /* Rotary movement and list paging stay silent. */
            break;
    }
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

/* Keep each label's normal color underneath a selection state. This also
 * restores metadata colors correctly when virtual rows are recycled. */
static void theme_mark_selected(lv_obj_t * obj, bool selected)
{
    if(g_theme->id == UI_THEME_GRAY) return;
    if(lv_obj_check_type(obj, &lv_label_class)) {
        if(selected) lv_obj_add_state(obj, LV_STATE_USER_1);
        else lv_obj_remove_state(obj, LV_STATE_USER_1);
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for(uint32_t i = 0; i < count; i++) {
        theme_mark_selected(lv_obj_get_child(obj, i), selected);
    }
}

static void focus_visual(focus_item_t * item, bool focused)
{
    if(item == NULL || item->obj == NULL || item->silent) return;

    if(g_afc_sync_mode) {
        lv_obj_set_style_outline_width(item->obj, 0, 0);
        lv_obj_set_style_border_width(item->obj, focused ? 2 : 1, 0);
        lv_obj_set_style_border_side(item->obj, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_color(item->obj,
            lv_color_hex(focused ? g_theme->outline : COL_BORDER), 0);
        return;
    }
    if(item->fill) {
        theme_mark_selected(item->obj, focused);
        lv_obj_set_style_bg_opa(item->obj, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(item->obj, lv_color_hex(COL_FOCUS), 0);
        lv_obj_set_style_border_width(item->obj, focused ? 3 : 0, 0);
        lv_obj_set_style_border_side(item->obj, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(item->obj, lv_color_hex(COL_ACCENT), 0);
    }
    else {
        lv_obj_set_style_outline_width(item->obj, focused ? 3 : 0, 0);
        lv_obj_set_style_outline_color(item->obj, lv_color_hex(g_theme->outline), 0);
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

static void focus_activate(bool from_key)
{
    if(g_focus_count <= 0 || g_focus_index < 0) return;
    if(g_edit_mode) {
        ui_exit_edit();
        return;
    }
    focus_item_t * item = &g_focus[g_focus_index];
    ui_feedback_emit(UI_FEEDBACK_ACTIVATE, from_key);
    if(item->activate) item->activate(item->user);
}

static void focus_click_async(void * arg)
{
    int index = (int)(intptr_t)arg;
    focus_select(index);
    focus_activate(false);
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
static void feedback_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_feedback_emit(UI_FEEDBACK_CLICK, false);
}

static void touch_add_clickable(lv_obj_t * obj, lv_event_cb_t cb, void * user)
{
    lv_obj_set_clickable(obj, true);
    enable_event_bubble(obj);
    lv_obj_add_event_cb(obj, feedback_touch_cb, LV_EVENT_CLICKED, NULL);
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
    if(g_theme->id != UI_THEME_GRAY) {
        lv_obj_set_style_text_color(label, lv_color_hex(g_theme->on_selection),
                                    LV_STATE_USER_1);
    }
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

/* Overflowing list text is clipped (no wrapping, no ellipsis).  The cut-off
 * end fades into the background of the row the label sits on. */
#define TEXT_FADE_W      16
#define MARQUEE_SPEED_PX 36
#define MARQUEE_IDLE_MS  1200
#define MARQUEE_HOLD_MS  1000
#define MARQUEE_BACK_MS  400

static lv_color_t text_fade_bg(lv_obj_t * obj)
{
    for(lv_obj_t * p = lv_obj_get_parent(obj); p != NULL; p = lv_obj_get_parent(p)) {
        if(lv_obj_get_style_bg_opa(p, 0) >= LV_OPA_COVER) {
            return lv_obj_get_style_bg_color(p, 0);
        }
    }
    return lv_color_hex(COL_MAIN_BG);
}

static void text_fade_draw_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(obj),
                     lv_obj_get_style_text_font(obj, 0), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int box_w = lv_area_get_width(&area);
    if(size.x <= box_w) return;

    lv_color_t bg = text_fade_bg(obj);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_grad.dir = LV_GRAD_DIR_HOR;
    dsc.bg_grad.stops_count = 2;
    dsc.bg_grad.stops[0].color = bg;
    dsc.bg_grad.stops[0].opa = LV_OPA_TRANSP;
    dsc.bg_grad.stops[0].frac = 0;
    dsc.bg_grad.stops[1].color = bg;
    dsc.bg_grad.stops[1].opa = LV_OPA_COVER;
    dsc.bg_grad.stops[1].frac = 255;

    lv_area_t right = area;
    right.x1 = area.x2 - TEXT_FADE_W + 1;
    lv_draw_rect(lv_event_get_layer(e), &dsc, &right);

}

static void text_fade_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_DRAW_POST) text_fade_draw_cb(e);
}

/* List text: no wrapping, cut-off end fades out on the right. */
static void ui_text_fade(lv_obj_t * label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_add_event_cb(label, text_fade_cb, LV_EVENT_DRAW_POST, NULL);
}

/* Secondary columns: no wrapping, hard cut at the column edge. */
static void ui_text_clip(lv_obj_t * label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
}

static lv_obj_t * g_marquee_label;

static void marquee_exec(void * obj, int32_t v)
{
    lv_label_t * label = (lv_label_t *)obj;
    label->offset.x = v;
    lv_obj_invalidate(obj);
}

/* Stops the running marquee and restores the label to its start position.
 * Call with NULL before the owning page is torn down. */
static void ui_marquee_stop(void)
{
    if(g_marquee_label == NULL) return;
    lv_anim_delete(g_marquee_label, marquee_exec);
    if(lv_obj_is_in_widget_tree(g_marquee_label)) {
        ((lv_label_t *)g_marquee_label)->offset.x = 0;
        lv_obj_invalidate(g_marquee_label);
    }
    g_marquee_label = NULL;
}

/* Idle-triggered marquee: after MARQUEE_IDLE_MS the overflowing text scrolls
 * right to left until its last character is visible, holds, then returns to
 * the start.  Only one label scrolls at a time. */
static void ui_marquee_start(lv_obj_t * label)
{
    ui_marquee_stop();
    if(label == NULL) return;

    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(label),
                     lv_obj_get_style_text_font(label, 0), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t area;
    lv_obj_get_coords(label, &area);
    int overflow = size.x - lv_area_get_width(&area);
    if(overflow <= 0) return;

    g_marquee_label = label;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, marquee_exec);
    lv_anim_set_values(&a, 0, -overflow);
    lv_anim_set_duration(&a, (uint32_t)overflow * 1000 / MARQUEE_SPEED_PX);
    lv_anim_set_delay(&a, MARQUEE_IDLE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_reverse_delay(&a, MARQUEE_HOLD_MS);
    lv_anim_set_reverse_duration(&a, MARQUEE_BACK_MS);
    lv_anim_set_repeat_count(&a, 1);
    lv_anim_start(&a);
}

/* List counter: "current | total" as two boxes split by a dashed rule.  Both
 * boxes share the width the total needs, so the counter never changes width
 * while the selection moves. */
#define COUNTER_H       36
#define COUNTER_PAD     10
#define COUNTER_DASHES   6
typedef struct {
    lv_obj_t * group;
    lv_obj_t * cur_box;
    lv_obj_t * cur_label;
    lv_obj_t * tot_box;
    lv_obj_t * tot_label;
    int digits;
} counter_t;

static lv_obj_t * counter_box(lv_obj_t * parent, lv_obj_t ** label_out)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, 0, COUNTER_H);
    lv_obj_set_style_bg_color(box, lv_color_hex(g_theme->counter), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_hor(box, COUNTER_PAD, 0);
    lv_obj_set_style_pad_ver(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(box, false);
    lv_obj_t * label = make_label(box, "0", FONT_NUM, COL_MUTED);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    if(label_out) *label_out = label;
    return box;
}

static void counter_create(lv_obj_t * parent, counter_t * c)
{
    c->digits = 0;
    c->group = lv_obj_create(parent);
    lv_obj_set_size(c->group, LV_SIZE_CONTENT, COUNTER_H);
    lv_obj_set_style_bg_opa(c->group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c->group, 0, 0);
    lv_obj_set_style_pad_all(c->group, 0, 0);
    lv_obj_set_style_pad_column(c->group, 0, 0);
    lv_obj_set_flex_flow(c->group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c->group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(c->group, false);

    c->cur_box = counter_box(c->group, &c->cur_label);

    lv_obj_t * dash = lv_obj_create(c->group);
    lv_obj_set_size(dash, 2, COUNTER_H);
    lv_obj_set_style_bg_opa(dash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dash, 0, 0);
    lv_obj_set_style_pad_all(dash, 0, 0);
    lv_obj_set_style_pad_row(dash, 3, 0);
    lv_obj_set_flex_flow(dash, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dash, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(dash, false);
    for(int i = 0; i < COUNTER_DASHES; i++) {
        lv_obj_t * d = lv_obj_create(dash);
        lv_obj_set_size(d, 2, 3);
        lv_obj_set_style_bg_color(d, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_radius(d, 0, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_set_scrollable(d, false);
    }

    c->tot_box = counter_box(c->group, &c->tot_label);
}

static void counter_set(counter_t * c, int current, int total)
{
    if(c == NULL || c->group == NULL) return;
    char text[16];
    snprintf(text, sizeof(text), "%d", total);
    int digits = (int)strlen(text);
    if(digits < 1) digits = 1;
    if(digits != c->digits) {
        c->digits = digits;
        lv_point_t glyph;
        lv_text_get_size(&glyph, "0", FONT_NUM, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        int w = digits * glyph.x + 2 * COUNTER_PAD;
        lv_obj_set_width(c->cur_box, w);
        lv_obj_set_width(c->tot_box, w);
    }
    snprintf(text, sizeof(text), "%d", current);
    lv_label_set_text(c->cur_label, text);
    snprintf(text, sizeof(text), "%d", total);
    lv_label_set_text(c->tot_label, text);
}

static lv_obj_t * make_icon(lv_obj_t * parent, const char * icon, uint32_t color)
{
    return make_label(parent, icon, &bootstrap_icons_24, color);
}

/* Draw classic raised/sunken edges inside the existing bounds: no extra
 * layout, shadow buffers, or changes to hit targets. Every corner is square. */
static void theme_bevel_draw(lv_event_t * event)
{
    lv_obj_t * obj = lv_event_get_target_obj(event);
    bool down = lv_obj_has_state(obj, LV_STATE_PRESSED) ||
                lv_obj_has_state(obj, LV_STATE_CHECKED);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.radius = 0;
    dsc.border_width = 2;
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_side = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT;
    dsc.border_color = lv_color_hex(down ? g_theme->bevel_shadow
                                        : g_theme->bevel_light);
    lv_draw_rect(lv_event_get_layer(event), &dsc, &area);
    dsc.border_side = LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT;
    dsc.border_color = lv_color_hex(down ? g_theme->bevel_light
                                        : g_theme->bevel_shadow);
    lv_draw_rect(lv_event_get_layer(event), &dsc, &area);
}

static void theme_chrome(lv_obj_t * obj, uint32_t bottom)
{
    if(g_theme->id == UI_THEME_AQUA) {
        lv_obj_set_style_bg_grad_color(obj, lv_color_hex(bottom), 0);
        lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    }
    else if(g_theme->id == UI_THEME_WINDOWS_95) {
        lv_obj_add_event_cb(obj, theme_bevel_draw, LV_EVENT_DRAW_POST, NULL);
    }
}

static void theme_button(lv_obj_t * button)
{
    if(g_theme->id == UI_THEME_GRAY) return;
    lv_obj_set_style_radius(button, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(g_theme->button_top), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(button, g_theme->id == UI_THEME_AQUA ? 1 : 0, 0);
    theme_chrome(button, g_theme->button_bottom);
    lv_obj_set_style_bg_color(button, lv_color_hex(g_theme->button_bottom),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_NONE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_FOCUS), LV_STATE_CHECKED);
    lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_NONE, LV_STATE_CHECKED);
}

static void theme_mode_button(lv_obj_t * button, bool active)
{
    if(g_theme->id == UI_THEME_GRAY) {
        lv_obj_set_style_bg_color(button, lv_color_hex(COL_FOCUS), 0);
        lv_obj_set_style_bg_opa(button, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    else {
        if(active) lv_obj_add_state(button, LV_STATE_CHECKED);
        else lv_obj_remove_state(button, LV_STATE_CHECKED);
        theme_mark_selected(button, active);
    }
}

static lv_obj_t * make_panel(lv_obj_t * parent)
{
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    if(g_theme->id == UI_THEME_WINDOWS_95) {
        lv_obj_add_event_cb(panel, theme_bevel_draw, LV_EVENT_DRAW_POST, NULL);
    }
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return panel;
}

/* Loading and populated pages share this exact header geometry. */
static lv_obj_t * make_page_header(lv_obj_t * parent, const char * title, int height)
{
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);
    make_label(head, title, FONT_PAGE_TITLE, COL_TEXT);
    return head;
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
    if(g_theme->id != UI_THEME_GRAY) {
        lv_obj_set_style_bg_color(list, lv_color_hex(COL_MUTED), LV_PART_SCROLLBAR);
    }
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
    lv_obj_set_clickable(button, true);
    lv_obj_add_event_cb(button, feedback_touch_cb, LV_EVENT_CLICKED, NULL);
    return button;
}

/* Transport controls have a theme frame. The secondary star stays flat so
 * its larger hit target can extend left of the aligned 30 px glyph. */
static lv_obj_t * make_icon_button(lv_obj_t * parent, const char * icon,
                                   int width, int height, uint32_t fg,
                                   const lv_font_t * font)
{
    lv_obj_t * button = make_flat_icon_button(parent, icon, width, height, fg, font);
    theme_button(button);
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
static FILE * s_thumb_pack_file;
static uint8_t * s_thumb_pack_ram;
static size_t s_thumb_pack_ram_size;
static bool s_thumb_pack_ram_ready;
static uint32_t s_thumb_pack_count;
static uint8_t s_thumb_valid[512];
static bool s_thumb_pack_ready;

#if defined(ESP_PLATFORM)
typedef struct {
    char path[128];
    int albums;
} thumb_load_arg_t;

static void thumb_pack_load_task(void * p)
{
    thumb_load_arg_t * a = (thumb_load_arg_t *)p;
    const size_t one = (size_t)REAL_THUMB_SIZE * REAL_THUMB_SIZE * 2;
    /* Include the header: real_thumb_read() uses file-relative slot offsets,
     * including for the final album in the pack. */
    size_t bytes = THUMB_CACHE_HEADER_SIZE + (size_t)a->albums * one;
    if(bytes > 0 && bytes <= 16u * 1024u * 1024u) {
        FILE * f = fopen(a->path, "rb");
        if(f != NULL) {
            uint32_t t0 = (uint32_t)esp_timer_get_time();
            /* Full-cache preload is optional. Keep room for fonts, display
             * layers and JPEG/USB buffers; SD remains the fallback. */
            const size_t reserve = 4u * 1024u * 1024u;
            size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            uint8_t * ram = available > bytes && available - bytes >= reserve ?
                ui_alloc_aligned(bytes) : NULL;
            if(ram == NULL) UI_PERF_LOG("thumb preload skipped: bytes=%u free=%u reserve=%u",
                (unsigned)bytes, (unsigned)available, (unsigned)reserve);
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
        if(xTaskCreateWithCaps(thumb_pack_load_task, "thumb_load", 6144, arg,
                               1, NULL, MALLOC_CAP_SPIRAM |
                                        MALLOC_CAP_8BIT) != pdPASS) {
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

    /* The packed thumbnail set is the only source used once it is ready.
     * Falling back to individual <id>.thm files caused a burst of SD reads
     * while the album list was being built; on IDF 5.5 the internal DMA pool
     * cannot service that burst and SDMMC allocation fails.  Albums without
     * a packed thumbnail simply show the placeholder. */
    if(s_thumb_pack_ready) {
        if(id > s_thumb_pack_count) return false;
        uint32_t n = (uint32_t)id - 1;
        if((s_thumb_valid[n >> 3] & (1u << (n & 7))) == 0) return false;
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
        return false;
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

static const lv_image_dsc_t * real_thumb_cached(uint16_t id)
{
    if(id == 0) return NULL;
    for(int i = 0; i < COVER_CACHE_SLOTS; i++) {
        if(cover_cache[i].id == id) return &cover_cache[i].dsc;
    }
    return NULL;
}

static const lv_image_dsc_t * real_thumb_for_album(uint16_t id)
{
    if(id == 0) return NULL;
    const lv_image_dsc_t * cached = real_thumb_cached(id);
    if(cached != NULL) return cached;

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

/* No file access or decoding on the scrolling path. */
static const void * thumb_for_album_cached(const player_album_t * album)
{
    if(album->cover_index >= PLAYER_REAL_COVER_BASE) {
        return real_thumb_cached(
            (uint16_t)(album->cover_index - PLAYER_REAL_COVER_BASE));
    }
    if(thumbs_ready && album->cover_index < THUMB_COUNT && album->cover[0]) {
        return &thumb54_dsc[album->cover_index];
    }
    return NULL;
}

static bool album_thumb_missing(const player_album_t * album)
{
    if(album->cover_index < PLAYER_REAL_COVER_BASE) return album->cover[0] == '\0';
    uint32_t id = album->cover_index - PLAYER_REAL_COVER_BASE;
    if(id == 0) return true;
    if(!s_thumb_pack_ready) return false; /* Legacy file: try at idle. */
    uint32_t n = id - 1;
    return id > s_thumb_pack_count || n / 8 >= sizeof(s_thumb_valid) ||
           (s_thumb_valid[n / 8] & (1u << (n & 7))) == 0;
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
 * buffer.  Keep one UI copy here so the LVGL image descriptor stays valid.
 *
 * This is a Now Playing cache keyed by the iAP2 song ID (track_key), stored
 * separately from the album-artwork cache used by the library pages:
 *   <library>/np/<8-hex-hash>.cov  (8-byte ID header + RGB565 image)
 * A hash is used for the file name because the SD card is mounted with FAT
 * 8.3 names; the full track ID is verified from the file header. */
#define NP_ART_SIZE 360
#define NP_ART_BYTES ((size_t)NP_ART_SIZE * NP_ART_SIZE * 2)
#define NP_CACHE_MAX_FILES 64

static uint8_t * g_iap2_cover_buf;
static lv_image_dsc_t g_iap2_cover_dsc;
static uint32_t g_iap2_cover_rev;
static uint64_t g_iap2_cover_track_id;
static bool g_iap2_cover_ready;

static char s_np_cache_dir[160] = "/sdcard/np";

static uint32_t np_cache_hash(uint64_t id)
{
    uint32_t h = (uint32_t)(id ^ (id >> 32));
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static void np_cache_path(uint64_t id, char * path, size_t len, bool tmp)
{
    snprintf(path, len, "%s/%08x.%s", s_np_cache_dir,
             (unsigned)np_cache_hash(id), tmp ? "tmp" : "cov");
}

static void np_cover_dsc_init(void)
{
    lv_memzero(&g_iap2_cover_dsc, sizeof(g_iap2_cover_dsc));
    g_iap2_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_iap2_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    g_iap2_cover_dsc.header.w = NP_ART_SIZE;
    g_iap2_cover_dsc.header.h = NP_ART_SIZE;
    g_iap2_cover_dsc.header.stride = NP_ART_SIZE * 2;
    g_iap2_cover_dsc.data_size = NP_ART_BYTES;
    g_iap2_cover_dsc.data = g_iap2_cover_buf;
}

static bool np_cache_load(uint64_t track_id, uint8_t * dst)
{
#if defined(ESP_PLATFORM)
    if(track_id == 0 || dst == NULL) return false;
    char path[192];
    np_cache_path(track_id, path, sizeof(path), false);
    FILE * f = fopen(path, "rb");
    if(f == NULL) return false;
    uint64_t stored = 0;
    bool ok = fread(&stored, 1, sizeof(stored), f) == sizeof(stored) &&
              stored == track_id &&
              fread(dst, 1, NP_ART_BYTES, f) == NP_ART_BYTES;
    fclose(f);
    return ok;
#else
    (void)track_id;
    (void)dst;
    return false;
#endif
}

#if defined(ESP_PLATFORM)
typedef struct {
    uint64_t id;
    uint8_t * data;
} np_cache_job_t;

static QueueHandle_t s_np_cache_queue;

static void np_cache_ensure_dir(void)
{
    mkdir(s_np_cache_dir, 0777);
}

typedef struct {
    char path[192];
    time_t mtime;
} np_cache_entry_t;

static int np_cache_entry_cmp(const void * a, const void * b)
{
    const np_cache_entry_t * ea = a;
    const np_cache_entry_t * eb = b;
    return (ea->mtime > eb->mtime) - (ea->mtime < eb->mtime);
}

static void np_cache_prune(void)
{
    DIR * dir = opendir(s_np_cache_dir);
    if(dir == NULL) return;

    /* 128 entries x ~200 bytes would overflow the small cache task stack.
     * Keep the directory listing in PSRAM instead. */
    const int max_entries = NP_CACHE_MAX_FILES * 2;
    np_cache_entry_t * entries = heap_caps_malloc(
        (size_t)max_entries * sizeof(*entries),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(entries == NULL) {
        closedir(dir);
        return;
    }
    int count = 0;

    struct dirent * ent;
    while((ent = readdir(dir)) != NULL && count < max_entries) {
        size_t n = strlen(ent->d_name);
        if(n < 4 || strcmp(ent->d_name + n - 4, ".cov") != 0) continue;
        np_cache_entry_t * e = &entries[count];
        snprintf(e->path, sizeof(e->path), "%s/%s", s_np_cache_dir,
                 ent->d_name);
        struct stat st;
        e->mtime = stat(e->path, &st) == 0 ? st.st_mtime : 0;
        count++;
    }
    closedir(dir);

    if(count > NP_CACHE_MAX_FILES) {
        qsort(entries, count, sizeof(entries[0]), np_cache_entry_cmp);
        for(int i = 0; i < count - NP_CACHE_MAX_FILES; i++) {
            unlink(entries[i].path);
        }
    }
    free(entries);
}

static void np_cache_task(void * arg)
{
    (void)arg;
    np_cache_job_t job;
    for(;;) {
        if(xQueueReceive(s_np_cache_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        np_cache_ensure_dir();
        char path[192];
        char tmp[192];
        np_cache_path(job.id, path, sizeof(path), false);
        np_cache_path(job.id, tmp, sizeof(tmp), true);
        FILE * f = fopen(tmp, "wb");
        if(f != NULL) {
            bool ok = fwrite(&job.id, 1, sizeof(job.id), f) == sizeof(job.id) &&
                      fwrite(job.data, 1, NP_ART_BYTES, f) == NP_ART_BYTES;
            fclose(f);
            if(ok) {
                rename(tmp, path);
                np_cache_prune();
            }
            else {
                unlink(tmp);
            }
        }
        free(job.data);
    }
}
#endif

void ui_set_np_cache_dir(const char * dir)
{
    if(dir == NULL || dir[0] == '\0') return;
    snprintf(s_np_cache_dir, sizeof(s_np_cache_dir), "%s", dir);
#if defined(ESP_PLATFORM)
    np_cache_ensure_dir();
    if(s_np_cache_queue == NULL) {
        s_np_cache_queue = xQueueCreate(2, sizeof(np_cache_job_t));
        if(s_np_cache_queue != NULL) {
            xTaskCreateWithCaps(np_cache_task, "np_cache", 12288, NULL, 2,
                                NULL, MALLOC_CAP_SPIRAM |
                                      MALLOC_CAP_8BIT);
        }
    }
#endif
}

static void np_cache_save_async(uint64_t track_id, const uint8_t * src)
{
#if defined(ESP_PLATFORM)
    if(track_id == 0 || src == NULL || s_np_cache_queue == NULL) return;
    uint8_t * copy = ui_alloc_aligned(NP_ART_BYTES);
    if(copy == NULL) return;
    memcpy(copy, src, NP_ART_BYTES);
    np_cache_job_t job = { .id = track_id, .data = copy };
    if(xQueueSend(s_np_cache_queue, &job, 0) != pdTRUE) {
        free(copy);
    }
#else
    (void)track_id;
    (void)src;
#endif
}

static const lv_image_dsc_t * iap2_cover_src(uint64_t track_id)
{
    if(g_iap2_cover_buf == NULL) {
        g_iap2_cover_buf = ui_alloc_aligned(NP_ART_BYTES);
    }

    if(track_id != g_iap2_cover_track_id) {
        g_iap2_cover_track_id = track_id;
        g_iap2_cover_rev = 0xffffffffu;
        g_iap2_cover_ready = false;
        if(track_id != 0 && g_iap2_cover_buf != NULL &&
           np_cache_load(track_id, g_iap2_cover_buf)) {
            g_iap2_cover_ready = true;
            np_cover_dsc_init();
            UI_PERF_LOG("np_art track=%016llx source=cache",
                        (unsigned long long)track_id);
        }
    }

    uint32_t rev = player_artwork_revision();
    if(rev != g_iap2_cover_rev) {
        g_iap2_cover_rev = rev;
        if(g_iap2_cover_buf != NULL &&
           player_artwork_copy(g_iap2_cover_buf, NP_ART_BYTES)) {
            g_iap2_cover_ready = true;
            np_cover_dsc_init();
            UI_PERF_LOG("np_art track=%016llx source=iap2",
                        (unsigned long long)track_id);
            if(track_id != 0) {
                np_cache_save_async(track_id, g_iap2_cover_buf);
            }
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
static lv_obj_t * g_mini_time;
static uint64_t g_mini_track_id;
static uint32_t g_mini_art_rev;
static uint32_t g_mini_rev = 0xffffffffu;
static uint32_t g_mini_elapsed = 0xffffffffu;
static lv_obj_t * g_status_time;
static bool g_clock_12h;
static int g_clock_offset_min;
static lv_obj_t * g_menubar;
static lv_obj_t * g_menu_items[SCR_COUNT];
static lv_obj_t * g_menu_icons[SCR_COUNT];
static bool g_menubar_left = true;
static lv_indev_t * g_touch_indev;
static bool g_touch_enabled = true;
static screen_id_t g_current_screen = SCR_NOW_PLAYING;
static screen_id_t g_ble_return_screen = SCR_COUNT;
static bool g_ctx_menu_open;
static bool g_device_available;
static bool g_ui_trace;
static uint32_t g_tab_start_us;
static bool g_tab_frame_pending;
static bool g_tab_content_frame_pending;
static bool g_page_loading, g_page_wait_frame;
static screen_id_t g_page_loading_screen = SCR_COUNT;
static lv_timer_t * g_page_load_timer, * g_page_detail_timer;
#define PAGE_LOADING_KEYS 64
static ui_key_t g_page_loading_keys[PAGE_LOADING_KEYS];
static int g_page_loading_key_count;

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
    /* Called when the tab is entered: the cursor returns to the left list
     * and the list keeps its remembered row. */
    void (*enter)(void);
} page_t;

static const page_t g_pages[SCR_COUNT];

static void ui_show_screen(screen_id_t id);
static void ui_rebuild_page(void);
static bool ui_list_page(int direction);
static void np_toggle_queue(void);
static void context_menu_open(void);
static void context_menu_close(void);
static bool context_menu_key(ui_key_t key);
static void ui_dispatch_key(ui_key_t key);

static void menu_click_async(void * arg)
{
    ui_show_screen((screen_id_t)(intptr_t)arg);
}

static void menu_click_cb(lv_event_t * event)
{
    ui_feedback_emit(UI_FEEDBACK_CLICK, false);
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    ui_call_async(menu_click_async, (void *)(intptr_t)index);
}

static player_status_t g_menu_play_status = (player_status_t)-1;

/* Device availability.  In iAP2 mode a missing iPod and a missing SD card
 * both put the UI into a blocked state: the menubar dims everything except
 * Now Playing and Settings, and Now Playing shows a placeholder. */
static bool g_dev_iap2_mode;
static bool g_dev_present;
static bool g_dev_sd_ready = true;
static bool g_sd_missing_seen;
static volatile bool g_dev_state_dirty;
static bool g_dev_blocked_last;

static bool ui_sd_needs_reboot(void)
{
    /* The card appeared after boot; the library and config were loaded
     * without it, so a power cycle is required. */
    return g_sd_missing_seen && g_dev_sd_ready;
}

/* Tabs that need the iPod library and therefore become unusable while the
 * device is unavailable. */
static bool ui_screen_needs_devices(screen_id_t id)
{
    return id == SCR_ALBUMS || id == SCR_ARTISTS ||
           id == SCR_SONGS || id == SCR_PLAYLISTS;
}

static bool ui_device_blocked(void)
{
    if(!g_dev_sd_ready || ui_sd_needs_reboot()) return true;
    if(g_dev_iap2_mode && !g_dev_present) return true;
    return false;
}

void ui_set_device_state(bool iap2_mode, bool present, bool sd_ready)
{
    if(iap2_mode != g_dev_iap2_mode || present != g_dev_present ||
       sd_ready != g_dev_sd_ready) {
        g_dev_iap2_mode = iap2_mode;
        g_dev_present = present;
        g_dev_sd_ready = sd_ready;
        g_dev_state_dirty = true;
    }
    if(!sd_ready) g_sd_missing_seen = true;
}

static void ui_menu_update(void);

static void ui_menu_play_update(void)
{
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    if(snapshot.status == g_menu_play_status) return;
    g_menu_play_status = snapshot.status;
    ui_menu_update();
}

static void ui_player_changed(void)
{
    ui_menu_play_update();
}

static void ui_menu_update(void)
{
    bool blocked = ui_device_blocked();
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    for(int i = 0; i < SCR_COUNT; i++) {
        if(g_menu_items[i] == NULL || g_menu_icons[i] == NULL) continue;
        bool active = (i == (int)g_current_screen) ||
                      (i == SCR_SETTINGS && g_current_screen == SCR_BLUETOOTH);
        bool dimmed = blocked && i != SCR_NOW_PLAYING && i != SCR_SETTINGS;
        /* The selected tile is a solid square: the full 96 x 96 touch area
         * uses the focus grey fill, with a white icon and no edge strip. */
        lv_obj_set_style_bg_color(g_menu_items[i],
                                  lv_color_hex(active ? COL_FOCUS : COL_BG), 0);
        lv_obj_set_style_bg_opa(g_menu_items[i],
                                active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g_menu_items[i], 0, 0);
        lv_obj_set_style_text_color(g_menu_icons[i],
                                    lv_color_hex(dimmed ? COL_DIM
                                                : active ? COL_MENU_TOP_FG
                                                         : COL_MENU_BODY_FG), 0);
    }

    /* With no SD card or no iPod the Now Playing tile shows why. */
    const char * np_icon = NULL;
    if(!g_dev_sd_ready || ui_sd_needs_reboot()) {
        np_icon = ICON_SD_CARD;
    }
    else if(g_dev_iap2_mode && !g_dev_present) {
        np_icon = ICON_PHONE;
    }
    else {
        np_icon = snapshot.status == PLAYER_PLAYING ? ICON_PLAY : ICON_PAUSE;
    }
    if(g_menu_icons[SCR_NOW_PLAYING] != NULL) {
        lv_label_set_text(g_menu_icons[SCR_NOW_PLAYING], np_icon);
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

    bool has_track = snapshot.track.title[0] != '\0';
    if(g_mini_star != NULL) {
        lv_obj_set_hidden(g_mini_star, !has_track);
    }
    if(!has_track && g_mini_title != NULL) {
        lv_label_set_text(g_mini_title, "");
        if(g_mini_artist != NULL) lv_obj_set_hidden(g_mini_artist, true);
    }

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

    if(snapshot.track.id != g_mini_track_id || art_changed) {
        g_mini_track_id = snapshot.track.id;
        lv_label_set_text(g_mini_title, has_track ? snapshot.track.title : "");
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

        const lv_image_dsc_t * art =
            snapshot.real_device && snapshot.track.title[0] != '\0'
                ? iap2_cover_src(snapshot.track.id) : NULL;
        player_album_t now_album;
        bool has_now_album = !snapshot.real_device &&
                             find_album(snapshot.track.album_id, &now_album);
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
    lv_obj_set_style_pad_left(g_mini, 16, 0);
    lv_obj_set_style_pad_right(g_mini, 20, 0);
    lv_obj_set_style_pad_ver(g_mini, 0, 0);
    lv_obj_set_style_pad_column(g_mini, 20, 0);
    lv_obj_set_flex_flow(g_mini, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_mini, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(g_mini, false);
    lv_obj_set_clickable(g_mini, true);
    lv_obj_add_event_cb(g_mini, mini_now_click_cb, LV_EVENT_CLICKED, NULL);

    theme_chrome(g_mini, g_theme->button_bottom);

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

    g_mini_title = make_label(text, "", FONT_H2, COL_MINI_FG);
    lv_obj_set_width(g_mini_title, LV_PCT(100));
    lv_obj_set_height(g_mini_title, 34);
    ui_text_fade(g_mini_title);
    g_mini_artist = make_label(text, "", FONT_CAPTION, COL_MINI_MUTED);
    lv_obj_set_width(g_mini_artist, LV_PCT(100));
    lv_obj_set_height(g_mini_artist, 24);
    ui_text_fade(g_mini_artist);

    /* Elapsed/duration only, centred vertically beside the status icons. */
    g_mini_time = make_label(g_mini, "0:00 / 0:00", FONT_NUM,
                             COL_MINI_MUTED);
    lv_obj_set_width(g_mini_time, 160);
    lv_obj_set_height(g_mini_time, 24);
    lv_obj_set_style_text_align(g_mini_time, LV_TEXT_ALIGN_RIGHT, 0);
    ui_text_fade(g_mini_time);

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
    if(show_mini && !g_page_loading) ui_mini_update();
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
    theme_chrome(time_area, 0x163F70);
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
        lv_obj_set_size(item, MENUBAR_W, MENUBAR_TILE_H);
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

void ui_set_time_changed_cb(void (*cb)(time_t now))
{
    g_time_changed_cb = cb;
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
    if(g_page_loading) return;
    if(g_current_screen == SCR_ALBUMS && albums_left_list != NULL) {
        lv_obj_scroll_by(albums_left_list, 0, dy, LV_ANIM_OFF);
    }
    else if(g_current_screen == SCR_SONGS && songs_list != NULL) {
        lv_obj_scroll_by(songs_list, 0, dy, LV_ANIM_OFF);
    }
}

static void update_clock(void)
{
    static bool last_12h;
    time_t now = time(NULL) + g_clock_offset_min * 60;
    struct tm local;
    localtime_r(&now, &local);
    char text[16];
    if(g_clock_12h) {
        strftime(text, sizeof(text), "%I:%M", &local);
        if(text[0] == '0' && text[1] != '\0') {
            memmove(text, text + 1, strlen(text));
        }
    }
    else {
        strftime(text, sizeof(text), "%H:%M", &local);
    }
    if(strcmp(text, lv_label_get_text(g_status_time)) != 0 || g_clock_12h != last_12h) {
        last_12h = g_clock_12h;
        lv_obj_set_style_text_font(g_status_time,
                                   g_clock_12h ? FONT_CLOCK_12
                                               : FONT_CLOCK, 0);
        lv_label_set_text(g_status_time, text);
    }
}

static void settings_time_blink_tick(void);

static void ui_tick_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    if(ui_apply_theme()) return;
    static uint32_t last_second;
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    uint32_t perf_t1 = 0, perf_t2 = 0, perf_t3 = 0, perf_t4 = 0;
    uint32_t perf_t5 = 0, perf_t6 = 0;

    update_clock();
    if(g_ui_trace) perf_t1 = perf_now();
    if(g_dev_state_dirty) {
        g_dev_state_dirty = false;
        bool blocked = ui_device_blocked();
        ui_menu_update();

        /* The device disappeared while a library tab was open: that tab can
         * no longer render, so fall back to Now Playing (which shows the
         * plug / SD placeholder).  Only Settings and Now Playing stay. */
        if(blocked && !g_dev_blocked_last && !g_afc_sync_mode &&
           ui_screen_needs_devices(g_current_screen)) {
            g_dev_blocked_last = blocked;
            ui_show_screen(SCR_NOW_PLAYING);
            return;
        }
        g_dev_blocked_last = blocked;

        if(g_current_screen == SCR_NOW_PLAYING && !g_afc_sync_mode) {
            ui_rebuild_page();
            return;
        }
    }
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

    if(!g_page_loading && g_current_screen < SCR_COUNT && g_pages[g_current_screen].tick) {
        g_pages[g_current_screen].tick();
    }
    if(g_ui_trace) perf_t6 = perf_now();

    if(!g_page_loading && g_current_screen != SCR_NOW_PLAYING) ui_mini_update();
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
    if(!g_page_loading) settings_time_blink_tick();
}

static void ui_page_cancel_load(void)
{
    if(g_page_load_timer) lv_timer_pause(g_page_load_timer);
    if(g_page_detail_timer) lv_timer_pause(g_page_detail_timer);
    g_page_loading = g_page_wait_frame = false;
    g_page_loading_screen = SCR_COUNT;
    g_page_detail_timer = NULL;
    g_page_loading_key_count = 0;
    g_tab_frame_pending = g_tab_content_frame_pending = false;
}

static void ui_page_loading_shell(void)
{
    static const char * names[SCR_COUNT] = {
        "Now Playing", "Albums", "Artists", "Songs", "Playlists", "Bluetooth Remote", "Settings"
    };
    lv_obj_t * panel = make_panel(g_page);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    bool bluetooth = g_current_screen == SCR_BLUETOOTH;
    lv_obj_set_style_pad_all(panel, bluetooth ? 22 : 16, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    if(g_current_screen != SCR_NOW_PLAYING) {
        make_page_header(panel, names[g_current_screen], bluetooth ? 48 : 56);
    }
    make_label(panel, "Loading...", FONT_CAPTION, COL_MUTED);
}

/* Called only after the destination shell has completed a display refresh.
 * A newer tab request cancels this single timer and replaces its target. */
static void ui_page_load_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(!g_page_loading || g_page_loading_screen != g_current_screen) return;
    screen_id_t screen = g_current_screen;
    uint32_t start = g_ui_trace ? perf_now() : 0;
    lv_obj_clean(g_page);
    g_pages[screen].create(g_page);
    if(g_page_detail_timer) lv_timer_pause(g_page_detail_timer);
    g_page_loading = false;
    ui_menu_update();
    if(screen != SCR_NOW_PLAYING) ui_mini_update();
    g_tab_content_frame_pending = true;
    if(g_ui_trace) UI_PERF_LOG("tab_content_build: screen=%d build=%u us",
        screen, (unsigned)(perf_now() - start));

    /* Preserve rotary/Enter input received while the shell was visible.
     * Tab navigation itself is immediate and clears this queue. */
    ui_key_t keys[PAGE_LOADING_KEYS];
    int count = g_page_loading_key_count;
    memcpy(keys, g_page_loading_keys, (size_t)count * sizeof(keys[0]));
    g_page_loading_key_count = 0;
    for(int i = 0; i < count; i++) {
        /* Feedback was already emitted when the physical key arrived. */
        bool old_scope = g_key_feedback_scope;
        bool old_sent = g_key_feedback_sent;
        g_key_feedback_scope = g_key_feedback_sent = true;
        ui_dispatch_key(keys[i]);
        g_key_feedback_scope = old_scope;
        g_key_feedback_sent = old_sent;
        if(g_current_screen != screen || g_page_loading) break;
    }
}

static bool ui_page_defer_key(ui_key_t key)
{
    if(!g_page_loading) return false;
    switch(key) {
        case UI_KEY_UP:
        case UI_KEY_DOWN:
        case UI_KEY_ENTER:
        case UI_KEY_CONTEXT:
        case UI_KEY_PAGE_UP:
        case UI_KEY_PAGE_DOWN:
            break;
        case UI_KEY_SCREEN_PREV:
            if(g_current_screen != SCR_NOW_PLAYING) return false;
            break;
        default: return false;
    }
    if(g_page_loading_key_count < PAGE_LOADING_KEYS) {
        g_page_loading_keys[g_page_loading_key_count++] = key;
    }
    return true;
}

static void ui_tab_refresh_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if(g_page_loading && g_page_wait_frame &&
       g_page_loading_screen == g_current_screen) {
        g_page_wait_frame = false;
        lv_timer_reset(g_page_load_timer);
        lv_timer_resume(g_page_load_timer);
    }
    if(!g_page_loading && g_page_detail_timer) {
        lv_timer_reset(g_page_detail_timer);
        lv_timer_resume(g_page_detail_timer);
        g_page_detail_timer = NULL;
    }
    if(g_tab_frame_pending) {
        g_tab_frame_pending = false;
        if(g_ui_trace) UI_PERF_LOG("tab_frame: screen=%d ready=%u us",
            g_current_screen, (unsigned)(perf_now() - g_tab_start_us));
    }
    if(g_tab_content_frame_pending) {
        g_tab_content_frame_pending = false;
        if(g_ui_trace) UI_PERF_LOG("tab_content_frame: screen=%d ready=%u us",
            g_current_screen, (unsigned)(perf_now() - g_tab_start_us));
    }
}

static void ui_show_screen(screen_id_t id)
{
    if(g_afc_sync_mode || id < 0 || id >= SCR_COUNT) return;
    if(g_ctx_menu_open) context_menu_close();
    if(g_current_screen == SCR_SETTINGS && id != SCR_SETTINGS) ble_remote_scan_stop();
    if(id == g_current_screen) {
        if(!g_page_loading && g_pages[id].same_key) g_pages[id].same_key();
        return;
    }

    g_tab_start_us = perf_now();
    if(id != SCR_BLUETOOTH) g_ble_return_screen = SCR_COUNT;
    g_current_screen = id;
    ui_exit_edit();
    focus_clear();
    ui_page_teardown();
    lv_obj_clean(g_page);
    ui_set_hint("");
    if(g_page_load_timer == NULL && id != SCR_NOW_PLAYING) {
        g_page_load_timer = lv_timer_create(ui_page_load_cb, 16, NULL);
    }
    if(g_page_load_timer) lv_timer_pause(g_page_load_timer);
    /* Now Playing has no page heading and opens directly in its normal layout. */
    g_page_loading = id != SCR_NOW_PLAYING && g_page_load_timer != NULL;
    g_page_loading_screen = id;
    ui_layout_page_area();
    if(g_pages[id].enter) g_pages[id].enter();
    if(g_page_loading) {
        ui_page_loading_shell();
        g_page_wait_frame = true;
    }
    else g_pages[id].create(g_page);
    ui_menu_update();
    g_tab_frame_pending = true;
    if(g_ui_trace) UI_PERF_LOG("tab_build: screen=%d build=%u us",
        g_current_screen, (unsigned)(perf_now() - g_tab_start_us));
}

static void ui_rebuild_page(void)
{
    if(g_page_loading) {
        lv_timer_pause(g_page_load_timer);
        lv_obj_clean(g_page);
        ui_page_loading_shell();
        g_page_wait_frame = true;
        return;
    }
    ui_exit_edit();
    focus_clear();
    ui_page_teardown();
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
    ui_feedback_emit(UI_FEEDBACK_BACK, true);
    if(g_page_loading) {
        screen_id_t target = g_current_screen == SCR_BLUETOOTH && g_ble_return_screen != SCR_COUNT ?
            g_ble_return_screen : SCR_NOW_PLAYING;
        ui_show_screen(target);
        return;
    }
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
    if(!g_page_loading && g_current_screen < SCR_COUNT && g_pages[g_current_screen].tick) {
        g_pages[g_current_screen].tick();
    }
}

static void ui_dispatch_key(ui_key_t key)
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
                focus_activate(true);
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

    if(ui_page_defer_key(key)) return;

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
            focus_activate(true);
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
            /* The controller's "previous page" button opens the queue on
             * Now Playing, matching the Page Up shortcut. */
            if(g_current_screen == SCR_NOW_PLAYING) {
                np_toggle_queue();
                break;
            }
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

void ui_handle_key(ui_key_t key)
{
    /* Page and modal handlers can consume Enter/Back before the generic
     * focus code. Emit here, then suppress duplicate action/preview sounds
     * during this dispatch. Keep previews when enabling a previously silent
     * mode, and leave touch events outside this scope unchanged. */
    bool previous_scope = g_key_feedback_scope;
    bool previous_sent = g_key_feedback_sent;
    g_key_feedback_scope = true;
    g_key_feedback_sent = false;
    ui_play_key_feedback(key);
    ui_dispatch_key(key);
    g_key_feedback_scope = previous_scope;
    g_key_feedback_sent = previous_sent;
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
static lv_obj_t * np_artist;
static lv_obj_t * np_album;
static lv_obj_t * np_time_cur;
static lv_obj_t * np_time_dur;
static lv_obj_t * np_progress;
static lv_obj_t * np_play_icon;
static lv_obj_t * np_shuffle_btn;
static lv_obj_t * np_repeat_btn;
static lv_obj_t * np_shuffle_icon;
static lv_obj_t * np_repeat_icon;
static lv_obj_t * np_star_icon;
static lv_obj_t * np_star_btn;
static lv_obj_t * np_cover_widget;
static uint32_t g_np_cover_rev;
static uint64_t g_np_cover_track_id;
#define NP_QUEUE_ROW_H        56
#define NP_QUEUE_VISIBLE_ROWS  11
#define NP_QUEUE_LIST_H       (NP_QUEUE_VISIBLE_ROWS * NP_QUEUE_ROW_H)
/* Rows built for the queue after the current item.  The full queue is
 * always played; only this many upcoming entries are listed. */
#define NP_QUEUE_MAX_ROWS     100

/* Default Now Playing layout: cover on the left; title, artist and album
 * occupy three separate lines, with a secondary star action below. The
 * progress bar and the five transport/mode buttons span the full column
 * width below the cover. */
#define NP_COVER_SIZE         360
#define NP_QUEUE_LEFT_W       620
#define NP_QUEUE_PADDING       36
#define NP_INFO_W             (NP_QUEUE_LEFT_W - 2 * NP_QUEUE_PADDING)
#define NP_COVER_GAP           24
/* Default view uses symmetric margins inside the content area. NP_INFO_W
 * above remains the compact queue-open layout's metadata/control width. */
#define NP_MARGIN_X           72
#define NP_TOP_W             (CONTENT_W - 2 * NP_MARGIN_X)
#define NP_MAIN_INFO_W        (NP_TOP_W - NP_COVER_SIZE - NP_COVER_GAP)
#define NP_TOP_SPACER         72
#define NP_INFO_BOTTOM        24
#define NP_GAP_PROGRESS       64
#define NP_CONTROLS_BOTTOM    48
#define NP_BUTTON_W            96
#define NP_BUTTON_H            84
#define NP_BUTTON_GAP          28
/* Golden-ratio rhythm: 26:16:10 gaps; star = round(48 px / φ) = 30 px.
 * Keep its 56 px touch target independent of the smaller visual. */
#define NP_TEXT_GAP_TITLE      26
#define NP_TEXT_GAP_ARTIST     16
#define NP_TEXT_GAP_ALBUM      10
#define NP_STAR_ICON_SIZE      30
#define NP_STAR_TARGET_SIZE    56

static lv_obj_t * np_queue_list;
static bool np_queue_visible = false;
static int np_queue_focus_start;
#define NP_QUEUE_FILL_MS 16
#define NP_QUEUE_FILL_BATCH 4
#define NP_QUEUE_FILL_BUDGET_US 8000
static lv_timer_t * np_queue_fill_timer;
static lv_obj_t * np_queue_loading, * np_queue_spacer;
static bool np_queue_wait_frame;
static int np_queue_fill_next, np_queue_fill_end, np_queue_fill_current;
static uint32_t np_queue_fill_rev;
static uint32_t np_queue_fill_start_us;
static void np_queue_fill_cb(lv_timer_t * timer);
static uint32_t np_queue_lookup_us, np_queue_widget_us;
static uint32_t np_toggle_start_us;
static bool np_toggle_frame_pending;

static void np_display_refresh_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    /* A zero-delay/async callback may run before LVGL renders. Resume only
     * after the empty panel has actually completed its first refresh. */
    if(np_queue_wait_frame && np_queue_list != NULL && np_queue_visible &&
       g_current_screen == SCR_NOW_PLAYING) {
        np_queue_wait_frame = false;
        lv_timer_reset(np_queue_fill_timer);
        lv_timer_resume(np_queue_fill_timer);
    }
    if(np_toggle_frame_pending) {
        np_toggle_frame_pending = false;
        if(g_ui_trace) UI_PERF_LOG("np_queue_frame: open=%d ready=%u us",
            np_queue_visible, (unsigned)(perf_now() - np_toggle_start_us));
    }
}
static uint64_t np_last_track;
static int np_last_queue_count = -1;
static uint32_t np_last_queue_rev;

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
    if(ui_device_blocked()) return;
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    bool has_track = snapshot.track.title[0] != '\0';
    const char * title = has_track ? snapshot.track.title : "";
    if(np_title != NULL) {
        if(strcmp(lv_label_get_text(np_title), title) != 0) {
            lv_label_set_text(np_title, title);
            if(!np_queue_visible) {
                /* Fit the title to its text, capped at the metadata column
                 * width so long titles fade within the column. */
                lv_point_t title_size;
                lv_text_get_size(&title_size, title, FONT_NP_TITLE, 0, 0,
                                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                int32_t title_w = title_size.x;
                int32_t max_w = NP_MAIN_INFO_W;
                if(title_w > max_w) title_w = max_w;
                lv_obj_set_width(np_title, title_w);
            }
        }
        lv_obj_set_hidden(np_title, !has_track);
    }
    if(np_star_btn != NULL) {
        lv_obj_set_hidden(np_star_btn, !has_track);
    }

    if(np_queue_visible) {
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
    }
    else {
        const char * artist = has_track ? snapshot.track.artist : "";
        const char * album = has_track ? snapshot.track.album : "";
        lv_label_set_text(np_artist, artist);
        lv_obj_set_hidden(np_artist, artist[0] == '\0');
        lv_label_set_text(np_album, album);
        lv_obj_set_hidden(np_album, album[0] == '\0');
    }

    /* The NP cache is keyed by the iAP2 song ID (track_key).  A track
     * change or a new iAP2 artwork revision refreshes the cover; the album
     * artwork cache used by the library pages is deliberately not involved. */
    if(snapshot.real_device && np_cover_widget != NULL) {
        uint32_t art_rev = player_artwork_revision();
        bool track_changed = (snapshot.track.id != g_np_cover_track_id);
        if(art_rev != g_np_cover_rev || track_changed) {
            g_np_cover_rev = art_rev;
            g_np_cover_track_id = snapshot.track.id;
            const void * src = iap2_cover_src(snapshot.track.id);
            lv_image_set_src(np_cover_widget, src);
            lv_obj_set_style_bg_color(np_cover_widget,
                lv_color_hex(src ? COL_PANEL : COL_PANEL_ALT), 0);
            lv_obj_set_style_bg_opa(np_cover_widget,
                                    src ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
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
    theme_mode_button(np_shuffle_btn, snapshot.shuffle);
    theme_mode_button(np_repeat_btn, snapshot.repeat != PLAYER_REPEAT_OFF);
    lv_label_set_text(np_repeat_icon,
                      snapshot.repeat == PLAYER_REPEAT_ONE ? ICON_REPEAT_ONE : ICON_REPEAT);
    lv_label_set_text(np_star_icon,
                      snapshot.starred ? ICON_STAR_FILL : ICON_STAR);
    lv_obj_set_style_text_color(np_star_icon,
                                lv_color_hex(snapshot.starred ? COL_ACCENT : COL_MUTED), 0);
}

static void np_tick(void)
{
    if(ui_device_blocked()) return;
    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    bool track_changed = (snapshot.track.id != np_last_track);
    bool queue_changed = (player_queue_count() != np_last_queue_count) ||
                         (player_queue_revision() != np_last_queue_rev);
    np_last_track = snapshot.track.id;
    np_last_queue_count = player_queue_count();
    np_last_queue_rev = player_queue_revision();

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
    /* Back closes the Up Next panel when it is open. Now Playing is the
     * root screen, so with the panel hidden there is nowhere to go. */
    if(np_queue_visible) {
        np_toggle_queue();
    }
    return true;
}

static void np_queue_row(lv_obj_t * parent, int queue_index)
{
    uint32_t lookup_start = g_ui_trace ? perf_now() : 0;
    player_track_t track;
    if(!player_queue_at(queue_index, &track)) return;
    uint32_t widget_start = g_ui_trace ? perf_now() : 0;
    if(g_ui_trace) np_queue_lookup_us += widget_start - lookup_start;

    lv_obj_t * row = make_row(parent, NP_QUEUE_ROW_H);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_style_pad_column(row, 10, 0);

    lv_obj_t * title = make_label(row, track.title, FONT_H2, COL_TEXT);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_height(title, 34);
    ui_text_fade(title);

    lv_obj_t * artist = make_label(row, track.artist, FONT_H2, COL_MUTED);
    lv_obj_set_width(artist, 200);
    lv_obj_set_height(artist, 34);
    ui_text_clip(artist);

    focus_add_clickable(row, np_queue_action, NULL, (void *)(intptr_t)queue_index, true);
    if(g_ui_trace) np_queue_widget_us += perf_now() - widget_start;
}

static void np_queue_fill_cb(lv_timer_t * timer)
{
    if(g_current_screen != SCR_NOW_PLAYING || !np_queue_visible || np_queue_list == NULL) {
        lv_timer_pause(timer);
        return;
    }
    if(player_queue_revision() != np_queue_fill_rev ||
       player_queue_index() != np_queue_fill_current) {
        lv_timer_pause(timer);
        ui_rebuild_page();
        return;
    }
    int old_count = g_focus_count;
    uint32_t batch_start = perf_now();
    int attempted = 0;
    while(np_queue_fill_next < np_queue_fill_end) {
        int before = g_focus_count;
        np_queue_row(np_queue_list, np_queue_fill_next++);
        if(g_focus_count > before) {
            /* Rows precede the tail spacer, including the first row. */
            lv_obj_move_to_index(g_focus[g_focus_count - 1].obj,
                                 lv_obj_get_index(np_queue_spacer));
        }
        if(++attempted >= NP_QUEUE_FILL_BATCH ||
           (uint32_t)(perf_now() - batch_start) >= NP_QUEUE_FILL_BUDGET_US) break;
    }
    lv_obj_set_height(np_queue_spacer,
        (np_queue_fill_end - np_queue_fill_next) * NP_QUEUE_ROW_H);
    if(g_focus_count > np_queue_focus_start) lv_obj_set_hidden(np_queue_loading, true);
    if(old_count == np_queue_focus_start && g_focus_count > old_count) {
        focus_select(np_queue_focus_start);
    }
    perf_log_us("np_queue_batch", batch_start);
    if(np_queue_fill_next >= np_queue_fill_end) {
        lv_timer_pause(timer);
        lv_obj_set_hidden(np_queue_spacer, true);
        if(g_focus_count == np_queue_focus_start) {
            lv_label_set_text(np_queue_loading, "Queue details unavailable");
        }
        if(g_ui_trace) UI_PERF_LOG("np_queue_filled: rows=%d lookup=%u widgets=%u elapsed=%u us",
            g_focus_count - np_queue_focus_start, (unsigned)np_queue_lookup_us,
            (unsigned)np_queue_widget_us, (unsigned)(perf_now() - np_queue_fill_start_us));
    }
}

static bool find_album(uint64_t album_id, player_album_t * out)
{
    for(int i = 0; i < player_album_count(); i++) {
        if(player_album_get(i, out) && out->id == album_id) return true;
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
    if(ui_device_blocked()) return;
    np_toggle_start_us = perf_now();
    np_queue_visible = !np_queue_visible;
    ui_rebuild_page();
    np_toggle_frame_pending = true;
    if(g_ui_trace) UI_PERF_LOG("np_queue_toggle: open=%d build=%u us",
        np_queue_visible, (unsigned)(perf_now() - np_toggle_start_us));
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

    np_title = make_label(title_row, "", FONT_H2, COL_TEXT);
    lv_obj_set_flex_grow(np_title, 1);
    lv_obj_set_height(np_title, 44);
    ui_text_fade(np_title);

    lv_obj_t * star = make_flat_icon_button(title_row, ICON_STAR, 56, 56,
                                            COL_MUTED, &bootstrap_icons_32);
    np_star_btn = star;
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
    ui_text_fade(np_meta);

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
    lv_obj_set_style_bg_color(np_progress, lv_color_hex(g_theme->progress), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(np_progress, lv_color_hex(COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(np_progress, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(np_progress, 0, LV_PART_INDICATOR);
    np_time_dur = make_label(progress_row, "0:00", FONT_NUM, COL_MUTED);
    lv_obj_set_width(np_time_dur, 72);
    lv_obj_set_style_text_align(np_time_dur, LV_TEXT_ALIGN_RIGHT, 0);

    /* Keep transport against the bottom inset; give spare height to the
     * gap after progress rather than changing the artwork or text gaps. */
    lv_obj_t * controls_gap = np_vertical_gap(left, 16);
    lv_obj_set_flex_grow(controls_gap, 1);

    lv_obj_t * controls = lv_obj_create(left);
    lv_obj_set_size(controls, NP_INFO_W, 84);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(controls, false);

    lv_obj_t * shuffle = make_icon_button(controls, ICON_SHUFFLE, 56, 56,
                                                COL_MUTED, &bootstrap_icons_32);
    np_shuffle_btn = shuffle;
    np_shuffle_icon = lv_obj_get_child(shuffle, 0);
    lv_obj_add_event_cb(shuffle, np_shuffle_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * prev = make_icon_button(controls, ICON_PREV, 84, 70,
                                            COL_TEXT, &bootstrap_icons_48);
    lv_obj_t * play = make_icon_button(controls, ICON_PLAY, 108, 80,
                                            COL_ACCENT, &bootstrap_icons_48);
    lv_obj_t * next = make_icon_button(controls, ICON_NEXT, 84, 70,
                                            COL_TEXT, &bootstrap_icons_48);
    np_play_icon = lv_obj_get_child(play, 0);
    lv_obj_add_event_cb(prev, np_prev_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(play, np_play_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next, np_next_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * repeat = make_icon_button(controls, ICON_REPEAT, 56, 56,
                                               COL_MUTED, &bootstrap_icons_32);
    np_repeat_btn = repeat;
    np_repeat_icon = lv_obj_get_child(repeat, 0);
    lv_obj_add_event_cb(repeat, np_repeat_click_cb, LV_EVENT_CLICKED, NULL);
}

static void np_placeholder(lv_obj_t * parent)
{
    bool sd = !g_dev_sd_ready || ui_sd_needs_reboot();

    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, 18, 0);
    lv_obj_set_scrollable(box, false);

    if(sd) {
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t * icons = lv_obj_create(box);
        lv_obj_set_size(icons, LV_SIZE_CONTENT, 64);
        lv_obj_set_style_bg_opa(icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icons, 0, 0);
        lv_obj_set_style_pad_all(icons, 0, 0);
        lv_obj_set_style_pad_column(icons, 6, 0);
        lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(icons, false);
        make_label(icons, ICON_SD_CARD, &bootstrap_icons_48, COL_MUTED);
        make_label(icons, ICON_QUESTION_LG, &bootstrap_icons_48, COL_MUTED);

        if(g_dev_sd_ready) {
            make_label(box, "Press Power to Reboot", FONT_GROUP, COL_ACCENT);
        }
    }
    else {
        lv_obj_t * img = lv_image_create(box);
        lv_image_set_src(img, &plug_in_dsc);
        lv_obj_align(img, LV_ALIGN_BOTTOM_MID, 0, -24);
    }
}

static void np_create(lv_obj_t * parent)
{
    uint32_t create_start = g_ui_trace ? perf_now() : 0;
    np_queue_lookup_us = np_queue_widget_us = 0;
    np_queue_list = NULL;
    np_meta = NULL;
    np_artist = NULL;
    np_album = NULL;
    np_star_btn = NULL;
    np_cover_widget = NULL;

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);
    np_last_track = snapshot.track.id;
    np_last_queue_count = player_queue_count();
    np_last_queue_rev = player_queue_revision();

    if(ui_device_blocked()) {
        np_title = np_meta = np_artist = np_album = NULL;
        np_time_cur = np_time_dur = np_progress = NULL;
        np_play_icon = np_shuffle_btn = np_repeat_btn = NULL;
        np_shuffle_icon = np_repeat_icon = np_star_icon = np_star_btn = NULL;
        np_cover_widget = NULL;
        np_placeholder(parent);
        ui_set_hint("");
        return;
    }

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
    bool has_now_album = !snapshot.real_device &&
                         find_album(snapshot.track.album_id, &now_album);
    const lv_image_dsc_t * iap2_cover =
        (snapshot.real_device && snapshot.track.title[0] != '\0')
            ? iap2_cover_src(snapshot.track.id) : NULL;
    const void * album_cover = (!snapshot.real_device && has_now_album)
                                   ? album_cover_src(&now_album) : NULL;
    const void * now_cover =
        iap2_cover != NULL ? (const void *)iap2_cover : album_cover;
    bool real_cover = iap2_cover != NULL ||
                      (!snapshot.real_device && has_now_album &&
                       now_album.cover_index >= PLAYER_REAL_COVER_BASE);
    bool has_cover = now_cover != NULL;
    uint32_t cover_color = iap2_cover != NULL
                               ? COL_PANEL
                               : (album_cover != NULL ? now_album.color
                                                      : COL_PANEL_ALT);

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
        lv_obj_set_style_bg_color(scrim, lv_color_hex(g_theme->scrim), 0);
        lv_obj_set_style_bg_opa(scrim, g_theme->scrim_opa, 0);
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
        lv_obj_set_size(left, NP_QUEUE_LEFT_W, LV_PCT(100));
    }
    else {
        lv_obj_set_size(left, LV_PCT(100), LV_PCT(100));
    }
    lv_obj_set_style_pad_all(left, np_queue_visible ? NP_QUEUE_PADDING : 0, 0);
    lv_obj_set_style_pad_row(left, 0, 0);
    if(np_queue_visible) {
        lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }
    else {
        lv_obj_set_layout(left, LV_LAYOUT_NONE);
    }

    if(np_queue_visible) {
        np_build_vertical_left(left, now_cover, cover_color);
    }
    else {
        /* Cover and metadata share a top block; progress and transport
         * controls have independent anchors below it. */
        lv_obj_t * top = lv_obj_create(left);
        lv_obj_set_size(top, NP_TOP_W, NP_COVER_SIZE);
        lv_obj_set_pos(top, NP_MARGIN_X, NP_TOP_SPACER);
        lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top, 0, 0);
        lv_obj_set_style_pad_all(top, 0, 0);
        lv_obj_set_style_pad_column(top, NP_COVER_GAP, 0);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(top, false);

        np_cover_widget = make_cover(top, now_cover, cover_color, NP_COVER_SIZE);

        lv_obj_t * info = lv_obj_create(top);
        lv_obj_set_size(info, 0, NP_COVER_SIZE);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_set_style_pad_all(info, 0, 0);
        lv_obj_set_style_pad_bottom(info, NP_INFO_BOTTOM, 0);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(info, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(info, 0, 0);
        lv_obj_set_scrollable(info, false);

        lv_obj_t * title_row = lv_obj_create(info);
        lv_obj_set_size(title_row, LV_PCT(100), 60);
        lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(title_row, 0, 0);
        lv_obj_set_style_pad_all(title_row, 0, 0);
        lv_obj_set_style_pad_column(title_row, 12, 0);
        lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(title_row, false);

        np_title = make_label(title_row, "", FONT_NP_TITLE, COL_TEXT);
        lv_obj_set_width(np_title, 300);
        lv_obj_set_height(np_title, 50);
        ui_text_fade(np_title);

        np_vertical_gap(info, NP_TEXT_GAP_TITLE);

        np_artist = make_label(info, "", FONT_NP_ARTIST, COL_MUTED);
        lv_obj_set_width(np_artist, LV_PCT(100));
        lv_obj_set_height(np_artist, 44);
        ui_text_fade(np_artist);

        np_vertical_gap(info, NP_TEXT_GAP_ARTIST);

        np_album = make_label(info, "", FONT_NP_ALBUM, COL_MUTED);
        lv_obj_set_width(np_album, LV_PCT(100));
        lv_obj_set_height(np_album, 44);
        ui_text_fade(np_album);

        np_vertical_gap(info, NP_TEXT_GAP_ALBUM);

        lv_obj_t * star = make_flat_icon_button(info, ICON_STAR,
                                                NP_STAR_TARGET_SIZE,
                                                NP_STAR_TARGET_SIZE,
                                                COL_MUTED, &bootstrap_icons_40);
        lv_obj_set_style_margin_left(star,
            -(NP_STAR_TARGET_SIZE - NP_STAR_ICON_SIZE) / 2, 0);
        /* Align the visible glyph to the bottom inset while retaining its
         * larger touch target, as on the text-aligned left edge. */
        lv_obj_set_style_margin_bottom(star,
            -(NP_STAR_TARGET_SIZE - NP_STAR_ICON_SIZE) / 2, 0);
        np_star_btn = star;
        np_star_icon = lv_obj_get_child(star, 0);
        /* Scale the glyph around its centre, preserving the button's hit
         * area and aligning the visible star with the metadata text. */
        lv_obj_set_style_transform_pivot_x(np_star_icon, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(np_star_icon, LV_PCT(50), 0);
        lv_obj_set_style_transform_scale(np_star_icon,
                                         LV_SCALE_NONE * NP_STAR_ICON_SIZE / 40, 0);
        lv_obj_add_event_cb(star, np_star_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * progress_row = lv_obj_create(left);
        lv_obj_set_size(progress_row, NP_TOP_W, 32);
        lv_obj_set_pos(progress_row, NP_MARGIN_X,
                       NP_TOP_SPACER + NP_COVER_SIZE + NP_GAP_PROGRESS);
        lv_obj_set_style_bg_opa(progress_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(progress_row, 0, 0);
        lv_obj_set_style_pad_all(progress_row, 0, 0);
        lv_obj_set_style_pad_column(progress_row, 12, 0);
        lv_obj_set_flex_flow(progress_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(progress_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(progress_row, false);

        np_time_cur = make_label(progress_row, "0:00", FONT_NP_TIME, COL_MUTED);
        lv_obj_set_width(np_time_cur, 88);
        lv_obj_set_style_text_align(np_time_cur, LV_TEXT_ALIGN_LEFT, 0);
        np_progress = lv_bar_create(progress_row);
        lv_obj_set_height(np_progress, 10);
        lv_obj_set_flex_grow(np_progress, 1);
        lv_obj_set_style_bg_color(np_progress, lv_color_hex(g_theme->progress),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(np_progress, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(np_progress, lv_color_hex(COL_ACCENT),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_radius(np_progress, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(np_progress, 0, LV_PART_INDICATOR);
        np_time_dur = make_label(progress_row, "0:00", FONT_NP_TIME, COL_MUTED);
        lv_obj_set_width(np_time_dur, 88);
        lv_obj_set_style_text_align(np_time_dur, LV_TEXT_ALIGN_RIGHT, 0);

        /* One transport row: shuffle, prev, play, next, repeat. Every button
         * uses the same 96x84 target and 48 px icon so the row reads evenly
         * and the touch targets are comfortable on the 5" panel. */
        lv_obj_t * controls = lv_obj_create(left);
        lv_obj_set_size(controls, NP_TOP_W, NP_BUTTON_H);
        lv_obj_set_pos(controls, NP_MARGIN_X,
                       SCREEN_H - NP_CONTROLS_BOTTOM - NP_BUTTON_H);
        lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(controls, 0, 0);
        lv_obj_set_style_pad_all(controls, 0, 0);
        lv_obj_set_style_pad_column(controls, NP_BUTTON_GAP, 0);
        lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(controls, false);

        lv_obj_t * shuffle = make_icon_button(controls, ICON_SHUFFLE,
                                                    NP_BUTTON_W, NP_BUTTON_H,
                                                    COL_MUTED,
                                                    &bootstrap_icons_48);
        lv_obj_t * prev = make_icon_button(controls, ICON_PREV,
                                                NP_BUTTON_W, NP_BUTTON_H,
                                                COL_TEXT, &bootstrap_icons_48);
        lv_obj_t * play = make_icon_button(controls, ICON_PLAY,
                                                NP_BUTTON_W, NP_BUTTON_H,
                                                COL_ACCENT, &bootstrap_icons_48);
        lv_obj_t * next = make_icon_button(controls, ICON_NEXT,
                                                NP_BUTTON_W, NP_BUTTON_H,
                                                COL_TEXT, &bootstrap_icons_48);
        lv_obj_t * repeat = make_icon_button(controls, ICON_REPEAT,
                                                  NP_BUTTON_W, NP_BUTTON_H,
                                                  COL_MUTED,
                                                  &bootstrap_icons_48);
        np_shuffle_btn = shuffle;
        np_repeat_btn = repeat;
        np_shuffle_icon = lv_obj_get_child(shuffle, 0);
        np_repeat_icon = lv_obj_get_child(repeat, 0);
        np_play_icon = lv_obj_get_child(play, 0);
        lv_obj_add_event_cb(shuffle, np_shuffle_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(prev, np_prev_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(play, np_play_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(next, np_next_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(repeat, np_repeat_click_cb, LV_EVENT_CLICKED, NULL);
    }

    uint32_t left_done = g_ui_trace ? perf_now() : 0;
    np_queue_focus_start = g_focus_count;
    if(np_queue_visible) {
        /* Right: queue */
        lv_obj_t * right = make_panel(root);
        lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
        lv_obj_set_flex_grow(right, 1);
        lv_obj_set_height(right, LV_PCT(100));
        lv_obj_set_style_pad_all(right, 18, 0);
        lv_obj_set_style_pad_right(right, 16, 0);

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

        np_queue_list = make_scroll(right);
        lv_obj_set_flex_grow(np_queue_list, 0);
        lv_obj_set_height(np_queue_list, NP_QUEUE_LIST_H);
        lv_obj_set_style_pad_right(np_queue_list, 0, 0);
        lv_obj_set_style_pad_row(np_queue_list, 0, 0);
        int total = player_queue_count();
        np_queue_fill_current = player_queue_index();
        int start = np_queue_fill_current >= 0 ? np_queue_fill_current + 1 : total;
        np_queue_fill_next = start;
        np_queue_fill_end = total < start + NP_QUEUE_MAX_ROWS ? total : start + NP_QUEUE_MAX_ROWS;
        np_queue_fill_rev = player_queue_revision();
        np_queue_fill_start_us = perf_now();
        np_queue_loading = make_label(np_queue_list,
            start < total ? "Loading queue..." : "No upcoming songs", FONT_CAPTION, COL_MUTED);
        if(start < total) {
            /* Floating status doesn't add height to the reserved row extent. */
            lv_obj_set_ignore_layout(np_queue_loading, true);
            np_queue_spacer = lv_obj_create(np_queue_list);
            lv_obj_set_size(np_queue_spacer, LV_PCT(100), (np_queue_fill_end - start) * NP_QUEUE_ROW_H);
            lv_obj_set_style_bg_opa(np_queue_spacer, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(np_queue_spacer, 0, 0);
            lv_obj_set_style_pad_all(np_queue_spacer, 0, 0);
            lv_obj_set_scrollable(np_queue_spacer, false);
            lv_obj_set_clickable(np_queue_spacer, false);
            if(np_queue_fill_timer == NULL) {
                np_queue_fill_timer = lv_timer_create(np_queue_fill_cb, NP_QUEUE_FILL_MS, NULL);
            }
            if(np_queue_fill_timer != NULL) {
                lv_timer_pause(np_queue_fill_timer);
                np_queue_wait_frame = true;
            }
            else lv_label_set_text(np_queue_loading, "Queue details unavailable");
        }
    }

    if(np_queue_visible && g_focus_count > 0) focus_select(np_queue_focus_start);
    g_np_cover_rev = player_artwork_revision();
    g_np_cover_track_id = snapshot.track.id;
    np_refresh_ui();
    ui_set_hint("UP/DOWN Select   ENTER Play/Jump   7 Play/Pause");
    if(g_ui_trace) UI_PERF_LOG("np_build: open=%d left=%u lookup=%u widgets=%u total=%u us rows=%d",
        np_queue_visible, (unsigned)(left_done - create_start),
        (unsigned)np_queue_lookup_us, (unsigned)np_queue_widget_us,
        (unsigned)(perf_now() - create_start), g_focus_count - np_queue_focus_start);
}

/* -------------------------------------------------------------------------
 * Albums
 * ------------------------------------------------------------------------- */
/* Albums layout (content area 1172 x 648, below the 72 px mini bar):
 *   left : 16 pad + 56 header + 8 x 70 rows + 16 pad = 648
 *   right: 16 pad + 112 header + 9 x 56 rows + 16 pad = 648 */
#define ALBUMS_HEADER_H    56
#define ALBUMS_ROW_H       70
#define ALBUMS_VISIBLE_ROWS 8
#define ALBUMS_LIST_H      (ALBUMS_VISIBLE_ROWS * ALBUMS_ROW_H)
#define ALBUMS_STRIDE      ALBUMS_ROW_H
/* Visual row 0 is the "Shuffle All" item (index -1); album N sits at row
 * N + ALBUMS_LEAD.  The list opens scrolled past it. */
#define ALBUMS_LEAD        1
static int albums_row_top(int index)
{
    return (index + ALBUMS_LEAD) * ALBUMS_STRIDE;
}
static int albums_first_for_top(int top)
{
    int first = top / ALBUMS_STRIDE - ALBUMS_LEAD;
    if(first > 0) first--;
    return first < 0 ? 0 : first;
}
#define ALBUMS_POOL        10
#define ALBUMS_REFRESH_MS  16
#define ALBUMS_COVER_IDLE_MS 140
#define ALBUMS_COVER_BATCH  2
#define ALBUMS_COVER_BUDGET_US 4000
#define ALBUMS_COVER_RETRY_MS 250
#define ALBUMS_COVER_MAX_ATTEMPTS 3
#define ALBUM_TRACK_ROW_H  56
#define ALBUM_HEADER_H     112
#define ALBUM_TRACK_ROWS   9
#define ALBUM_TRACK_LIST_H (ALBUM_TRACK_ROWS * ALBUM_TRACK_ROW_H)

static int albums_selected;
static bool albums_detail_open;
static lv_obj_t * albums_right_content;
static lv_obj_t * albums_left_list;
static lv_obj_t * albums_shuffle_row;
static counter_t albums_counter;
static lv_obj_t * albums_shuffle_icon;
static int albums_play_focus = -1;
static int albums_left_scroll_y;
static lv_obj_t * albums_rows[ALBUMS_POOL];
static lv_obj_t * albums_row_covers[ALBUMS_POOL];
static lv_obj_t * albums_row_titles[ALBUMS_POOL];
static lv_obj_t * albums_row_artists[ALBUMS_POOL];
static uint32_t albums_swipe_guard;
static uint64_t albums_row_album_ids[ALBUMS_POOL];
static int albums_row_indices[ALBUMS_POOL] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static bool albums_row_visible[ALBUMS_POOL];
static bool albums_row_cover_pending[ALBUMS_POOL];
static uint8_t albums_row_cover_attempts[ALBUMS_POOL];
static uint32_t albums_row_cover_attempt_tick[ALBUMS_POOL];
static bool albums_row_selected[ALBUMS_POOL];
static int albums_view_top;
static int albums_view_h;
static int albums_first_index = -1;
static bool albums_detail_dirty;
static lv_timer_t * albums_detail_timer;
static lv_timer_t * albums_refresh_timer;
static lv_timer_t * albums_cover_timer;
static bool albums_refresh_pending;
static uint32_t albums_last_scroll_tick;
static lv_obj_t * albums_detail_title;
static lv_obj_t * artists_detail_name;
static lv_obj_t * playlists_detail_title;
static lv_obj_t * playlists_name_labels[64];

static void albums_update_count(void)
{
    if(albums_counter.group == NULL) return;
    int count = player_album_count();
    if(count <= 0) {
        counter_set(&albums_counter, 0, 0);
        return;
    }
    counter_set(&albums_counter, albums_selected + 1, count);
}

static void albums_save_position(void)
{
    if(albums_left_list) {
        albums_left_scroll_y = lv_obj_get_scroll_y(albums_left_list);
    }
}

static void albums_rebuild(void)
{
    albums_save_position();
    ui_rebuild_page();
}

/* Snap touch scrolling to whole track rows without forcing the focused row
 * to the top of the viewport. */
static void albums_track_scroll_end_cb(lv_event_t * event)
{
    lv_obj_t * list = lv_event_get_target(event);
    int top = lv_obj_get_scroll_top(list);
    int snapped = ((top + ALBUM_TRACK_ROW_H / 2) / ALBUM_TRACK_ROW_H) *
                  ALBUM_TRACK_ROW_H;
    if(snapped != top) {
        lv_obj_scroll_to_y(list, snapped, LV_ANIM_ON);
    }
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
    theme_mark_selected(row, selected);
}

static void albums_apply_shuffle_row_style(void)
{
    if(albums_shuffle_row == NULL) return;
    bool selected = albums_selected < 0;
    bool focused = selected && !albums_detail_open;
    lv_obj_set_style_bg_opa(albums_shuffle_row,
                            selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(albums_shuffle_row,
                              lv_color_hex(focused ? COL_FOCUS : COL_ACCENT_DARK), 0);
    lv_obj_set_style_border_width(albums_shuffle_row, focused ? 3 : 0, 0);
    lv_obj_set_style_border_side(albums_shuffle_row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(albums_shuffle_row, lv_color_hex(COL_ACCENT), 0);
    theme_mark_selected(albums_shuffle_row, selected);
}

static void albums_update_selection(void)
{
    albums_apply_shuffle_row_style();
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
        albums_row_cover_pending[slot] = false;
        return;
    }

    /* Recheck visibility even for retained rows. Overscan rows may have been
     * bound with placeholders before they entered the viewport. */
    int top = albums_view_top;
    int view_h = albums_view_h;
    int row_top = albums_row_top(index);
    bool visible = row_top < top + view_h && row_top + ALBUMS_ROW_H > top;

    bool same = albums_row_album_ids[slot] == album.id &&
                albums_row_indices[slot] == index;
    if(!same) {
        lv_obj_set_hidden(row, false);
        lv_obj_set_pos(row, 0, albums_row_top(index));
        lv_obj_set_user_data(row, (void *)(intptr_t)index);
        lv_label_set_text(albums_row_titles[slot], album.title);
        lv_label_set_text(albums_row_artists[slot], album.artist);
        lv_obj_set_style_bg_color(albums_row_covers[slot],
                                  lv_color_hex(album.color ? album.color : COL_PANEL), 0);
        albums_row_album_ids[slot] = album.id;
        albums_row_indices[slot] = index;
    }
    if(!same || visible != albums_row_visible[slot]) {
        const void * src = visible ? thumb_for_album_cached(&album) : NULL;
        lv_image_set_src(albums_row_covers[slot], src);
        albums_row_visible[slot] = visible;
        albums_row_cover_pending[slot] = visible && src == NULL &&
                                         !album_thumb_missing(&album);
        albums_row_cover_attempts[slot] = 0;
        albums_row_cover_attempt_tick[slot] = 0;
    }
    bool selected = index == albums_selected;
    if(!same || albums_row_selected[slot] != selected) {
        albums_apply_row_style(row, index);
        albums_row_selected[slot] = selected;
    }
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
            albums_row_cover_pending[i] = false;
        }
        perf_log_us("albums_refresh", perf_t0);
        return;
    }

    int top = lv_obj_get_scroll_top(albums_left_list);
    if(top < 0) top = 0;
    albums_view_top = top;
    albums_view_h = lv_obj_get_height(albums_left_list);
    int first = albums_first_for_top(top);
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
                albums_fill_row(slot, index);
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
        if(albums_row_indices[slot] < 0) {
            lv_obj_set_hidden(albums_rows[slot], true);
            albums_row_cover_pending[slot] = false;
        }
    }
    perf_log_us("albums_refresh", perf_t0);
}

static void albums_refresh_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    albums_refresh_pending = false;
    if(g_current_screen != SCR_ALBUMS || albums_left_list == NULL) return;
    albums_refresh_rows();
}

static void albums_schedule_refresh(void)
{
    if(albums_refresh_timer == NULL) {
        albums_refresh_timer = lv_timer_create(albums_refresh_timer_cb, ALBUMS_REFRESH_MS,
                                               NULL);
        if(albums_refresh_timer == NULL) return;
        lv_timer_pause(albums_refresh_timer);
    }
    /* Coalesce to a frame, without postponing rows indefinitely during a
     * continuous drag. This path only binds text and existing cached images. */
    if(!albums_refresh_pending) {
        albums_refresh_pending = true;
        lv_timer_reset(albums_refresh_timer);
        lv_timer_resume(albums_refresh_timer);
    }
}

static void albums_cover_timer_cb(lv_timer_t * timer)
{
    if(g_current_screen != SCR_ALBUMS || albums_left_list == NULL) {
        lv_timer_pause(timer);
        return;
    }
    if(lv_tick_elaps(albums_last_scroll_tick) < ALBUMS_COVER_IDLE_MS ||
       lv_obj_is_scrolling(albums_left_list)) return;

    /* Reconcile the final viewport before touching artwork, including when
     * movement stayed within the same ten-row pool. */
    albums_refresh_rows();
    lv_timer_set_period(timer, ALBUMS_REFRESH_MS);
    uint32_t start = perf_now();
    int attempted = 0;
    for(int index = albums_first_index;
        index < albums_first_index + ALBUMS_POOL; index++) {
        for(int slot = 0; slot < ALBUMS_POOL; slot++) {
            if(albums_row_indices[slot] != index ||
               !albums_row_cover_pending[slot] || !albums_row_visible[slot]) continue;
            if(albums_row_cover_attempts[slot] > 0 &&
               lv_tick_elaps(albums_row_cover_attempt_tick[slot]) <
                   ALBUMS_COVER_RETRY_MS) continue;
            player_album_t album;
            if(!player_album_get(index, &album) ||
               album.id != albums_row_album_ids[slot]) {
                albums_row_cover_pending[slot] = false;
                continue;
            }
            const void * src = thumb_for_album(&album, 54);
            albums_row_cover_attempts[slot]++;
            albums_row_cover_attempt_tick[slot] = lv_tick_get();
            if(src != NULL) {
                lv_image_set_src(albums_row_covers[slot], src);
                albums_row_cover_pending[slot] = false;
            }
            else if(album_thumb_missing(&album) ||
                    albums_row_cover_attempts[slot] >= ALBUMS_COVER_MAX_ATTEMPTS) {
                albums_row_cover_pending[slot] = false;
            }
            attempted++;
            break;
        }
        /* One slow SD read may exceed the budget; yield before starting the
         * next one. PSRAM/cache hits can fill two covers per frame. */
        if(attempted >= ALBUMS_COVER_BATCH ||
           (uint32_t)(perf_now() - start) >= ALBUMS_COVER_BUDGET_US) break;
    }
    if(attempted) perf_log_us("albums_cover_batch", start);
    int visible = 0, loaded = 0;
    bool pending = false;
    uint32_t next_delay = ALBUMS_COVER_RETRY_MS;
    for(int slot = 0; slot < ALBUMS_POOL; slot++) {
        if(albums_row_cover_pending[slot]) {
            pending = true;
            uint32_t elapsed = lv_tick_elaps(albums_row_cover_attempt_tick[slot]);
            uint32_t delay = albums_row_cover_attempts[slot] == 0 ||
                             elapsed >= ALBUMS_COVER_RETRY_MS
                ? ALBUMS_REFRESH_MS : ALBUMS_COVER_RETRY_MS - elapsed;
            if(delay < next_delay) next_delay = delay;
        }
        if(albums_row_visible[slot] && albums_row_indices[slot] >= 0) {
            visible++;
            if(lv_image_get_src(albums_row_covers[slot]) != NULL) loaded++;
        }
    }
    if(pending) {
        lv_timer_set_period(timer, next_delay < ALBUMS_REFRESH_MS
                                     ? ALBUMS_REFRESH_MS : next_delay);
        return;
    }
    lv_timer_pause(timer);
    if(g_ui_trace) {
        UI_PERF_LOG("albums_covers settled: visible=%d loaded=%d missing=%d",
                    visible, loaded, visible - loaded);
    }
}

static void albums_schedule_covers(void)
{
    if(albums_cover_timer == NULL) {
        albums_cover_timer = lv_timer_create(albums_cover_timer_cb,
                                             ALBUMS_COVER_IDLE_MS, NULL);
        if(albums_cover_timer == NULL) return;
    }
    lv_timer_set_period(albums_cover_timer, ALBUMS_COVER_IDLE_MS);
    lv_timer_reset(albums_cover_timer);
    lv_timer_resume(albums_cover_timer);
}

static void albums_cancel_work(void)
{
    if(albums_refresh_timer) lv_timer_pause(albums_refresh_timer);
    if(albums_cover_timer) lv_timer_pause(albums_cover_timer);
    albums_refresh_pending = false;
}

static void albums_scroll_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    if(albums_left_list == NULL) return;
    albums_last_scroll_tick = lv_tick_get();
    albums_schedule_refresh();
    albums_schedule_covers();
}

static void albums_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(g_current_screen != SCR_ALBUMS) return;
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
    if(g_page_detail_timer != albums_detail_timer) lv_timer_resume(albums_detail_timer);
}

static void albums_select_visual(int index)
{
    int count = player_album_count();
    if(count <= 0) return;
    if(index < -1) index = -1;
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
    albums_select_visual(actual / ALBUMS_STRIDE - ALBUMS_LEAD);
    albums_schedule_detail();
    albums_swipe_guard = lv_tick_get();
}

static void albums_scroll_to_selected(void)
{
    if(albums_left_list == NULL) return;
    int count = player_album_count();
    if(count <= 0) return;
    if(albums_selected < -1) albums_selected = -1;
    if(albums_selected >= count) albums_selected = count - 1;

    int top = lv_obj_get_scroll_top(albums_left_list);
    if(top < 0) top = 0;
    int view = lv_obj_get_height(albums_left_list);
    int row_top = albums_row_top(albums_selected);
    int new_top = top;
    if(row_top < top) new_top = row_top;
    else if(row_top + ALBUMS_ROW_H > top + view) {
        new_top = row_top + ALBUMS_ROW_H - view;
    }
    int max_top = (count + ALBUMS_LEAD) * ALBUMS_STRIDE - view;
    if(max_top < 0) max_top = 0;
    if(new_top < 0) new_top = 0;
    if(new_top > max_top) new_top = max_top;
    if(new_top != top) {
        lv_obj_scroll_to_y(albums_left_list, new_top, LV_ANIM_OFF);
    }
    int actual = lv_obj_get_scroll_top(albums_left_list);
    if(actual < 0) actual = 0;
    int first = albums_first_for_top(actual);
    if(first != albums_first_index || actual != albums_view_top) {
        albums_refresh_rows();
    }
    /* Shuffle All is outside the recycled album pool. Refresh its highlight
     * on this same key press even when scrolling required a row refresh. */
    albums_update_selection();
}

static void albums_select(int index)
{
    uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
    int count = player_album_count();
    if(count <= 0) return;
    if(index < -1) index = -1;
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
    if(albums_play_focus >= 0) {
        focus_select(albums_play_focus);
    }
    else if(g_focus_count > 0) {
        focus_select(0);
    }
}

static void albums_shuffle_all(void)
{
    player_play_all_shuffled();
    ui_set_hint("Shuffle All");
}

static void albums_shuffle_all_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    albums_shuffle_all();
}

static void albums_track_action(void * user)
{
    int index = (int)(intptr_t)user;
    player_album_t album;
    if(!player_album_get(albums_selected, &album)) return;
    player_play_album_from(album.id, index);
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
    ui_feedback_emit(UI_FEEDBACK_CLICK, false);
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
            if(albums_selected < 0) {
                albums_shuffle_all();
                return true;
            }
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

static void albums_shuffle_action(void * user)
{
    LV_UNUSED(user);
    player_album_t album;
    if(!player_album_get(albums_selected, &album)) return;
    player_play_album_shuffled(album.id);
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

static void albums_shuffle_async(void * arg)
{
    albums_shuffle_action(arg);
}

static void albums_shuffle_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(albums_shuffle_async, NULL);
}

static void albums_enter(void)
{
    albums_detail_open = false;
    /* Minus items are temporary actions, not remembered album selections.
     * A new visit starts at visible item 1 if an action was selected. */
    if(albums_selected < 0) {
        albums_selected = 0;
        albums_left_scroll_y = albums_row_top(0);
    }
}

static bool albums_back(void)
{
    if(albums_detail_open) {
        albums_detail_open = false;
        albums_rebuild();
        return true;
    }
    /* Back never lands on the Shuffle All row: from any album it returns to
     * the first album with that row scrolled out of view, and only from the
     * first album does it leave the page. */
    if(albums_selected == 0 || albums_selected < 0) return false;

    albums_left_scroll_y = albums_row_top(0);
    if(albums_left_list) {
        lv_obj_scroll_to_y(albums_left_list, albums_left_scroll_y, LV_ANIM_OFF);
    }
    albums_select(0);
    return true;
}

static void albums_same_key(void)
{
    if(albums_detail_open) {
        albums_detail_open = false;
        albums_rebuild();
        return;
    }
    albums_left_scroll_y = albums_row_top(0);
    if(albums_left_list) {
        lv_obj_scroll_to_y(albums_left_list, albums_left_scroll_y, LV_ANIM_OFF);
    }
    albums_select(0);
}

static void albums_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);
    albums_shuffle_icon = NULL;
    albums_play_focus = -1;

    player_album_t album;
    if(!player_album_get(albums_selected, &album)) return;

    lv_obj_t * detail_head = lv_obj_create(parent);
    /* 108 px top: 80 px cover + title/subtitle on the left, icon controls
     * on the right, all vertically centred against the cover. */
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
    lv_obj_set_width(detail_text, 0);
    lv_obj_set_height(detail_text, LV_PCT(100));
    lv_obj_set_flex_grow(detail_text, 1);
    lv_obj_set_style_bg_opa(detail_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_text, 0, 0);
    lv_obj_set_style_pad_all(detail_text, 0, 0);
    lv_obj_set_style_pad_row(detail_text, 3, 0);
    lv_obj_set_flex_flow(detail_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_text, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
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
    lv_obj_set_width(title, 0);
    lv_obj_set_height(title, 34);
    lv_obj_set_flex_grow(title, 1);
    ui_text_fade(title);
    albums_detail_title = title;

    char subtitle[96];
    if(album.year > 0) {
        snprintf(subtitle, sizeof(subtitle), "%s  |  %u  |  %u tracks",
                 album.artist, album.year, album.track_count);
    }
    else {
        snprintf(subtitle, sizeof(subtitle), "%s  |  %u tracks",
                 album.artist, album.track_count);
    }
    lv_obj_t * subtitle_label = make_label(detail_text, subtitle,
                                           FONT_GROUP, COL_MUTED);
    lv_obj_set_width(subtitle_label, LV_PCT(100));
    lv_obj_set_height(subtitle_label, 24);
    ui_text_fade(subtitle_label);

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    lv_obj_t * controls = lv_obj_create(detail_head);
    lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_style_pad_column(controls, 6, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(controls, false);

    lv_obj_t * shuffle = make_icon_button(
        controls, ICON_SHUFFLE, 56, 56,
        snapshot.shuffle ? COL_ACCENT : COL_MUTED, &bootstrap_icons_32);
    albums_shuffle_icon = lv_obj_get_child(shuffle, 0);
    if(interactive) {
        focus_add_clickable(shuffle, albums_shuffle_action, NULL, NULL, true);
    }
    else {
        touch_add_clickable(shuffle, albums_shuffle_touch_cb, NULL);
    }

    lv_obj_t * play = make_icon_button(
        controls, ICON_PLAY_OUTLINE, 56, 56, COL_TEXT, &bootstrap_icons_32);
    if(interactive) {
        albums_play_focus = focus_add_clickable(play, albums_play_all_action,
                                                NULL, NULL, true);
    }
    else {
        touch_add_clickable(play, albums_play_touch_cb, NULL);
        albums_play_focus = -1;
    }

    lv_obj_t * track_list = make_scroll(parent);
    lv_obj_set_flex_grow(track_list, 0);
    lv_obj_set_height(track_list, ALBUM_TRACK_LIST_H);
    lv_obj_set_style_pad_row(track_list, 0, 0);
    lv_obj_set_style_pad_right(track_list, 14, 0);
    lv_obj_add_event_cb(track_list, albums_track_scroll_end_cb,
                        LV_EVENT_SCROLL_END, NULL);
    int track_count = player_album_track_count(album.id);

    /* The preview stays short; the opened detail shows the whole album. */
    int max_tracks = interactive ? track_count : DETAIL_PREVIEW_MAX_TRACKS;
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
        lv_obj_set_width(title, 0);
        lv_obj_set_height(title, 34);
        lv_obj_set_flex_grow(title, 1);
        ui_text_fade(title);
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
    if(max_tracks < track_count) {
        lv_obj_t * more = make_label(track_list, "More tracks not shown",
                                     FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(more, 32);
    }
}

static void albums_create(lv_obj_t * parent)
{
    albums_shuffle_icon = NULL;
    albums_play_focus = -1;
    albums_detail_title = NULL;
    albums_cancel_work();
    albums_right_content = NULL;
    albums_left_list = NULL;
    albums_shuffle_row = NULL;
    albums_counter.group = NULL;
    albums_first_index = -1;
    for(int i = 0; i < ALBUMS_POOL; i++) {
        albums_row_album_ids[i] = 0;
        albums_row_indices[i] = -1;
        albums_row_visible[i] = false;
        albums_row_cover_pending[i] = false;
        albums_row_cover_attempts[i] = 0;
        albums_row_selected[i] = false;
    }
    albums_detail_dirty = false;
    if(albums_detail_timer) lv_timer_pause(albums_detail_timer);

    player_sort_albums(g_sort_field, g_sort_desc);

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
    lv_obj_t * left_content = make_panel_content(left, 16);
    lv_obj_set_style_pad_row(left_content, 0, 0);

    lv_obj_t * head = make_page_header(left_content, "Albums", ALBUMS_HEADER_H);
    counter_create(head, &albums_counter);
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
    lv_obj_add_event_cb(list, albums_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    albums_left_list = list;

    int count = player_album_count();
    if(albums_selected >= count) albums_selected = 0;

    int content_h = count > 0 ? (count + ALBUMS_LEAD) * ALBUMS_STRIDE
                              : ALBUMS_ROW_H;
    lv_obj_t * spacer = lv_obj_create(list);
    lv_obj_set_size(spacer, LV_PCT(100), content_h);
    lv_obj_set_pos(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);
    lv_obj_set_clickable(spacer, false);

    albums_shuffle_row = make_row(list, ALBUMS_ROW_H);
    lv_obj_set_pos(albums_shuffle_row, 0, 0);
    lv_obj_set_width(albums_shuffle_row, LV_PCT(100));
    lv_obj_set_clickable(albums_shuffle_row, true);
    lv_obj_set_gesture_bubble(albums_shuffle_row, false);
    enable_event_bubble(albums_shuffle_row);
    lv_obj_add_event_cb(albums_shuffle_row, albums_shuffle_all_touch_cb,
                        LV_EVENT_CLICKED, NULL);
    make_label(albums_shuffle_row, "Shuffle All", FONT_H2, COL_TEXT);
    albums_apply_shuffle_row_style();

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
        lv_obj_set_width(albums_row_titles[i], LV_PCT(100));
        lv_obj_set_height(albums_row_titles[i], 34);
        ui_text_fade(albums_row_titles[i]);
        albums_row_artists[i] = make_label(text, "", FONT_GROUP, COL_MUTED);
        lv_obj_set_width(albums_row_artists[i], LV_PCT(100));
        lv_obj_set_height(albums_row_artists[i], 24);
        ui_text_fade(albums_row_artists[i]);
        lv_obj_set_hidden(row, true);
        albums_rows[i] = row;
    }

    lv_obj_t * right = make_panel(root);
    lv_obj_set_style_bg_color(right, lv_color_hex(COL_PANEL_ALT), 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_style_pad_all(right, 0, 0);
    albums_right_content = make_panel_content(right, 16);
    lv_obj_set_style_pad_row(albums_right_content, 0, 0);
    if(g_page_loading && !albums_detail_open) {
        make_label(albums_right_content, "Loading...", FONT_CAPTION, COL_MUTED);
        albums_schedule_detail();
        g_page_detail_timer = albums_detail_timer;
    }
    else albums_render_detail(albums_right_content, albums_detail_open);

    lv_obj_update_layout(list);
    if(albums_selected < 0) {
        /* A rebuild within this visit may keep the active action visible;
         * albums_enter() resets it only when returning from another page. */
        albums_left_scroll_y = 0;
    }
    else if(albums_left_scroll_y < albums_row_top(0)) {
        albums_left_scroll_y = albums_row_top(0);
    }
    lv_obj_scroll_to_y(list, albums_left_scroll_y, LV_ANIM_OFF);
    albums_refresh_rows();
    albums_schedule_covers();

    if(albums_detail_open) {
        focus_select(albums_play_focus >= 0 ? albums_play_focus : 0);
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
#define ARTIST_HEADER_H       56
#define ARTIST_DETAIL_ROWS    10
#define ARTIST_DETAIL_LIST_H  (ARTIST_DETAIL_ROWS * ARTIST_TRACK_ROW_H)
#define ARTISTS_POOL          11

static int artists_selected;
static bool artists_detail_open;
static bool artists_detail_dirty;
static lv_timer_t * artists_detail_timer;
static lv_obj_t * artists_right_content;
static lv_obj_t * artists_left_list;
static counter_t artists_counter;
static lv_obj_t * artists_shuffle_icon;
static int artists_play_focus = -1;
static int artists_left_scroll_y;
static lv_obj_t * artists_rows[ARTISTS_POOL];
static lv_obj_t * artists_names[ARTISTS_POOL];
static lv_obj_t * artists_metas[ARTISTS_POOL];
static uint64_t artists_row_ids[ARTISTS_POOL];
static int artists_row_indices[ARTISTS_POOL] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int artists_first_index = -1;

static void artists_update_count(void)
{
    if(artists_counter.group == NULL) return;
    int count = player_artist_count();
    if(count <= 0) {
        counter_set(&artists_counter, 0, 0);
        return;
    }
    counter_set(&artists_counter, artists_selected + 1, count);
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
    theme_mark_selected(row, selected);
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
        char albums_text[16];
        snprintf(albums_text, sizeof(albums_text), "%u", artist.album_count);
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
    if(g_current_screen != SCR_ARTISTS) return;
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
    if(g_page_detail_timer != artists_detail_timer) lv_timer_resume(artists_detail_timer);
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

static uint32_t artists_swipe_guard;

static void artists_swipe_page(int direction)
{
    if(artists_left_list == NULL) return;
    int rows = lv_obj_get_height(artists_left_list) / ARTISTS_ROW_H;
    if(rows < 2) rows = 2;
    int page = (rows - 1) * ARTISTS_ROW_H;
    int top = lv_obj_get_scroll_top(artists_left_list);
    if(top < 0) top = 0;
    lv_obj_scroll_to_y(artists_left_list, top + direction * page,
                       LV_ANIM_OFF);

    int actual = lv_obj_get_scroll_top(artists_left_list);
    if(actual < 0) actual = 0;
    artists_select(actual / ARTISTS_ROW_H);
    artists_swipe_guard = lv_tick_get();
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
    if(artists_play_focus >= 0) {
        focus_select(artists_play_focus);
    }
    else if(g_focus_count > 0) {
        focus_select(0);
    }
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
    player_artist_t artist;
    if(track_index >= 0 && track_index < player_song_count() &&
       player_song_get(track_index, &track) &&
       player_artist_get(artists_selected, &artist)) {
        player_play_artist_from(artist.id, track.id);
    }
}

static void artists_play_all_action(void * user)
{
    LV_UNUSED(user);
    player_artist_t artist;
    if(player_artist_get(artists_selected, &artist)) {
        player_play_artist(artist.id);
    }
}

static void artists_shuffle_action(void * user)
{
    LV_UNUSED(user);
    player_artist_t artist;
    if(!player_artist_get(artists_selected, &artist)) return;
    player_play_artist_shuffled(artist.id);
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
    if(artists_swipe_guard != 0 &&
       (uint32_t)(lv_tick_get() - artists_swipe_guard) < 1000) {
        return;
    }
    ui_feedback_emit(UI_FEEDBACK_CLICK, false);
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

static void artists_play_async(void * arg)
{
    artists_play_all_action(arg);
}

static void artists_play_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(artists_play_async, NULL);
}

static void artists_shuffle_async(void * arg)
{
    artists_shuffle_action(arg);
}

static void artists_shuffle_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(artists_shuffle_async, NULL);
}

static void artists_enter(void)
{
    artists_detail_open = false;
}

static bool artists_back(void)
{
    if(artists_detail_open) {
        artists_detail_open = false;
        artists_rebuild();
        return true;
    }
    if(artists_selected > 0) {
        artists_select(0);
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
    artists_shuffle_icon = NULL;
    artists_play_focus = -1;

    player_artist_t artist;
    if(!player_artist_get(artists_selected, &artist)) return;

    /* 108 px header: artist name/stats on the left, Shuffle/Play icons on
     * the right, vertically centred in the header. */
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), ARTIST_HEADER_H);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 16, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);

    lv_obj_t * head_text = lv_obj_create(head);
    lv_obj_set_width(head_text, 0);
    lv_obj_set_height(head_text, LV_PCT(100));
    lv_obj_set_flex_grow(head_text, 1);
    lv_obj_set_style_bg_opa(head_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head_text, 0, 0);
    lv_obj_set_style_pad_all(head_text, 0, 0);
    lv_obj_set_style_pad_column(head_text, 14, 0);
    lv_obj_set_flex_flow(head_text, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head_text, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head_text, false);

    lv_obj_t * artist_name = make_label(head_text, artist.name,
                                        FONT_DETAIL_TITLE, COL_TEXT);
    lv_obj_set_width(artist_name, 0);
    lv_obj_set_flex_grow(artist_name, 1);
    lv_obj_set_height(artist_name, 34);
    ui_text_fade(artist_name);
    artists_detail_name = artist_name;

    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "%u songs  |  %u albums",
             artist.song_count, artist.album_count);
    lv_obj_t * stats = make_label(head_text, subtitle, FONT_GROUP, COL_MUTED);
    lv_obj_set_width(stats, 220);
    lv_obj_set_height(stats, 24);
    lv_obj_set_style_text_align(stats, LV_TEXT_ALIGN_RIGHT, 0);
    ui_text_fade(stats);

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    lv_obj_t * controls = lv_obj_create(head);
    lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_style_pad_column(controls, 6, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(controls, false);

    lv_obj_t * shuffle = make_icon_button(
        controls, ICON_SHUFFLE, 56, 56,
        snapshot.shuffle ? COL_ACCENT : COL_MUTED, &bootstrap_icons_32);
    artists_shuffle_icon = lv_obj_get_child(shuffle, 0);
    if(interactive) {
        focus_add_clickable(shuffle, artists_shuffle_action, NULL, NULL, true);
    }
    else {
        touch_add_clickable(shuffle, artists_shuffle_touch_cb, NULL);
    }

    lv_obj_t * play = make_icon_button(
        controls, ICON_PLAY_OUTLINE, 56, 56, COL_TEXT, &bootstrap_icons_32);
    if(interactive) {
        artists_play_focus = focus_add_clickable(play, artists_play_all_action,
                                                 NULL, NULL, true);
    }
    else {
        touch_add_clickable(play, artists_play_touch_cb, NULL);
        artists_play_focus = -1;
    }

    lv_obj_t * album_list = make_scroll(parent);
    lv_obj_set_flex_grow(album_list, 0);
    lv_obj_set_height(album_list, ARTIST_DETAIL_LIST_H);
    lv_obj_set_style_pad_row(album_list, 0, 0);
    lv_obj_set_style_pad_right(album_list, 14, 0);
    lv_obj_add_event_cb(album_list, albums_track_scroll_end_cb,
                        LV_EVENT_SCROLL_END, NULL);

    /* Collect the artist's tracks in one pass.  The preview keeps only a
     * short list; the opened detail keeps every track. */
    int songs = player_song_count();
    int artist_songs = 0;
    for(int i = 0; i < songs; i++) {
        player_track_t track;
        if(player_song_get(i, &track) && track.artist_id == artist.id) {
            artist_songs++;
        }
    }

    int max_tracks = interactive ? artist_songs : DETAIL_PREVIEW_MAX_TRACKS;
    if(max_tracks > artist_songs) max_tracks = artist_songs;

    int * track_indices = NULL;
    int * track_albums = NULL;
    uint64_t * album_ids = NULL;
    int album_total = 0;
    int track_rows = 0;

    if(max_tracks > 0) {
        track_indices = malloc((size_t)max_tracks * sizeof(*track_indices));
        track_albums = malloc((size_t)max_tracks * sizeof(*track_albums));
        album_ids = malloc((size_t)max_tracks * sizeof(*album_ids));
        if(track_indices == NULL || track_albums == NULL || album_ids == NULL) {
            free(track_indices);
            free(track_albums);
            free(album_ids);
            return;
        }

        for(int i = 0; i < songs && track_rows < max_tracks; i++) {
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
                slot = album_total++;
                album_ids[slot] = track.album_id;
            }
            track_indices[track_rows] = i;
            track_albums[track_rows] = slot;
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
        lv_obj_set_width(album_title, 0);
        lv_obj_set_flex_grow(album_title, 1);
        lv_obj_set_height(album_title, 34);
        ui_text_fade(album_title);

        char meta[64];
        if(album.year > 0) {
            snprintf(meta, sizeof(meta), "%u  |  %u tracks",
                     album.year, album.track_count);
        }
        else {
            snprintf(meta, sizeof(meta), "%u tracks", album.track_count);
        }
        lv_obj_t * album_meta = make_label(header, meta, FONT_GROUP, COL_MUTED);
        lv_obj_set_height(album_meta, 24);
        make_icon(header, ICON_PLAY, COL_MUTED);

        if(interactive) {
            focus_add_clickable(header, artists_album_action, NULL,
                                (void *)(intptr_t)album_index, true);
        }
        else {
            touch_add_clickable(header, artists_album_touch_cb,
                                (void *)(intptr_t)album_index);
        }

        for(int j = 0; j < track_rows; j++) {
            if(track_albums[j] != a) continue;
            int track_index = track_indices[j];
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
            lv_obj_set_width(title, 0);
            lv_obj_set_flex_grow(title, 1);
            lv_obj_set_height(title, 34);
            ui_text_fade(title);

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

    if(max_tracks < artist_songs) {
        lv_obj_t * more = make_label(album_list, "More tracks not shown",
                                     FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(more, 32);
    }

    free(track_indices);
    free(track_albums);
    free(album_ids);
}

static void artists_create(lv_obj_t * parent)
{
    artists_shuffle_icon = NULL;
    artists_play_focus = -1;
    artists_detail_name = NULL;
    if(artists_detail_timer) lv_timer_pause(artists_detail_timer);
    artists_detail_dirty = false;
    artists_right_content = NULL;
    artists_left_list = NULL;
    artists_counter.group = NULL;
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
    lv_obj_set_style_pad_all(left, 16, 0);
    lv_obj_set_style_pad_row(left, 0, 0);

    lv_obj_t * head = make_page_header(left, "Artists", 56);
    counter_create(head, &artists_counter);
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
        lv_obj_set_style_pad_right(text, 14, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(text, false);
        artists_names[i] = make_label(text, "", FONT_H2, COL_TEXT);
        lv_obj_set_width(artists_names[i], 0);
        lv_obj_set_flex_grow(artists_names[i], 1);
        ui_text_fade(artists_names[i]);
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
    artists_right_content = make_panel_content(right, 16);
    lv_obj_set_style_pad_row(artists_right_content, 0, 0);
    if(g_page_loading && !artists_detail_open) {
        make_label(artists_right_content, "Loading...", FONT_CAPTION, COL_MUTED);
        artists_schedule_detail();
        g_page_detail_timer = artists_detail_timer;
    }
    else artists_render_detail(artists_right_content, artists_detail_open);

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, artists_left_scroll_y, LV_ANIM_OFF);
    artists_refresh_rows();

    if(artists_detail_open) {
        focus_select(artists_play_focus >= 0 ? artists_play_focus : 0);
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

#define COL_CTX_PANEL       (g_theme->context_panel)
#define COL_CTX_GROUP       (g_theme->context_group)
#define COL_CTX_BORDER      (g_theme->context_border)

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
        theme_mark_selected(item->row, focused);
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
static counter_t songs_counter;
static lv_obj_t * songs_rows[SONGS_POOL];
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
    theme_mark_selected(row, selected);
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

    bool current = (track.id == snapshot->track.id);
    if(songs_row_indices[slot] == index && songs_row_ids[slot] == track.id) {
        if(songs_row_current[slot] != current) {
            songs_row_current[slot] = current;
            lv_obj_set_style_text_color(songs_titles[slot],
                                        lv_color_hex(current ? COL_ACCENT
                                                             : COL_TEXT), 0);
        }
        songs_apply_selection(row, index == songs_selected);
        songs_row_selected[slot] = (index == songs_selected);
        return;
    }

    if(true) {
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
static void playlists_swipe_page(int direction);

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
    if(g_page_loading) return true;
    if(g_current_screen == SCR_ALBUMS) {
        albums_swipe_page(direction);
        return true;
    }
    if(g_current_screen == SCR_ARTISTS) {
        artists_swipe_page(direction);
        return true;
    }
    if(g_current_screen == SCR_SONGS) {
        songs_swipe_page(direction);
        return true;
    }
    if(g_current_screen == SCR_PLAYLISTS) {
        playlists_swipe_page(direction);
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
    if(songs_counter.group == NULL) return;
    int count = player_song_count();
    if(count <= 0) {
        counter_set(&songs_counter, 0, 0);
        return;
    }
    int position = songs_selected + 1;
    if(position < 1) position = 1;
    if(position > count) position = count;
    counter_set(&songs_counter, position, count);
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
        player_play_song(track.id);
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
    ui_feedback_emit(UI_FEEDBACK_CLICK, false);
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
    if(songs_selected > 0) {
        songs_select(0);
        return true;
    }
    return false;
}

static void songs_same_key(void)
{
    songs_select(0);
}

static void songs_create(lv_obj_t * parent)
{
    songs_counter.group = NULL;
    lv_obj_t * panel = make_panel(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);

    lv_obj_t * head = make_page_header(panel, "Songs", 56);
    lv_obj_set_style_pad_right(head, 14, 0);
    counter_create(head, &songs_counter);

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
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_set_clickable(row, true);
        lv_obj_set_gesture_bubble(row, false);
        enable_event_bubble(row);
        lv_obj_add_event_cb(row, songs_row_click_cb, LV_EVENT_CLICKED, NULL);

        songs_titles[i] = make_label(row, "", FONT_H2, COL_TEXT);
        lv_obj_set_flex_grow(songs_titles[i], 1);
        lv_obj_set_height(songs_titles[i], 34);
        ui_text_fade(songs_titles[i]);
        songs_artists[i] = make_label(row, "", FONT_H2, COL_MUTED);
        lv_obj_set_width(songs_artists[i], 230);
        lv_obj_set_height(songs_artists[i], 34);
        ui_text_clip(songs_artists[i]);
        songs_albums[i] = make_label(row, "", FONT_H2, COL_MUTED);
        lv_obj_set_width(songs_albums[i], 250);
        lv_obj_set_height(songs_albums[i], 34);
        ui_text_clip(songs_albums[i]);
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
#define PLAYLIST_TRACK_ROWS    10
#define PLAYLIST_HEADER_H      56
#define PLAYLIST_TRACK_LIST_H  (PLAYLIST_TRACK_ROWS * PLAYLIST_TRACK_ROW_H)

static int playlists_selected;
static bool playlists_detail_open;
static int playlists_play_focus = -1;
static bool playlists_detail_dirty;
static lv_timer_t * playlists_detail_timer;
static lv_obj_t * playlists_left_list;
static lv_obj_t * playlists_right_content;
static counter_t playlists_counter;
static int playlists_left_scroll_y;
static uint32_t playlists_swipe_guard;

static void playlists_update_count(void)
{
    if(playlists_counter.group == NULL) return;
    int count = player_playlist_count();
    if(count <= 0) {
        counter_set(&playlists_counter, 0, 0);
        return;
    }
    counter_set(&playlists_counter, playlists_selected + 1, count);
}

static void playlists_render_detail(lv_obj_t * parent, bool interactive);

static void playlists_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(g_current_screen != SCR_PLAYLISTS) return;
    if(playlists_detail_dirty && playlists_right_content != NULL) {
        playlists_detail_dirty = false;
        uint32_t perf_t0 = g_ui_trace ? perf_now() : 0;
        playlists_render_detail(playlists_right_content, playlists_detail_open);
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
    if(g_page_detail_timer != playlists_detail_timer) lv_timer_resume(playlists_detail_timer);
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
    if(playlists_play_focus >= 0) {
        focus_select(playlists_play_focus);
    }
    else if(g_focus_count > 0) {
        focus_select(0);
    }
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
        player_play_playlist_from(playlists_selected, index);
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
    if(playlists_swipe_guard != 0 &&
       (uint32_t)(lv_tick_get() - playlists_swipe_guard) < 1000) {
        return;
    }
    ui_feedback_emit(UI_FEEDBACK_CLICK, false);
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

static void playlists_enter(void)
{
    playlists_detail_open = false;
}

static bool playlists_back(void)
{
    if(playlists_detail_open) {
        playlists_detail_open = false;
        playlists_rebuild();
        return true;
    }
    if(playlists_selected > 0) {
        playlists_selected = 0;
        playlists_left_scroll_y = 0;
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

static void playlists_shuffle_action(void * user)
{
    LV_UNUSED(user);
    player_play_playlist_shuffled(playlists_selected);
}

static void playlists_shuffle_async(void * arg)
{
    playlists_shuffle_action(arg);
}

static void playlists_shuffle_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    ui_call_async(playlists_shuffle_async, NULL);
}

static void playlists_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);
    playlists_play_focus = -1;

    player_playlist_t playlist;
    if(!player_playlist_get(playlists_selected, &playlist)) return;

    /* 112 px header: name and track count on the left, Shuffle / Play
     * icons on the right, matching the Albums and Artists detail pages. */
    lv_obj_t * head = lv_obj_create(parent);
    lv_obj_set_size(head, LV_PCT(100), PLAYLIST_HEADER_H);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 16, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head, false);

    lv_obj_t * head_text = lv_obj_create(head);
    lv_obj_set_width(head_text, 0);
    lv_obj_set_height(head_text, LV_PCT(100));
    lv_obj_set_flex_grow(head_text, 1);
    lv_obj_set_style_bg_opa(head_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head_text, 0, 0);
    lv_obj_set_style_pad_all(head_text, 0, 0);
    lv_obj_set_style_pad_column(head_text, 14, 0);
    lv_obj_set_flex_flow(head_text, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head_text, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(head_text, false);

    lv_obj_t * title = make_label(head_text, playlist.name, FONT_DETAIL_TITLE,
                                  COL_TEXT);
    lv_obj_set_width(title, 0);
    lv_obj_set_height(title, 34);
    lv_obj_set_flex_grow(title, 1);
    ui_text_fade(title);
    playlists_detail_title = title;

    char meta[64];
    const char * noun = playlist.track_count == 1 ? "track" : "tracks";
    if(playlist.local) {
        snprintf(meta, sizeof(meta), "%u %s  |  Local",
                 playlist.track_count, noun);
    }
    else {
        snprintf(meta, sizeof(meta), "%u %s", playlist.track_count, noun);
    }
    lv_obj_t * meta_label = make_label(head_text, meta, FONT_GROUP, COL_MUTED);
    lv_obj_set_height(meta_label, 24);
    ui_text_clip(meta_label);

    player_snapshot_t snapshot;
    player_get_snapshot(&snapshot);

    lv_obj_t * controls = lv_obj_create(head);
    lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_style_pad_column(controls, 6, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(controls, false);

    lv_obj_t * shuffle = make_icon_button(
        controls, ICON_SHUFFLE, 56, 56,
        snapshot.shuffle ? COL_ACCENT : COL_MUTED, &bootstrap_icons_32);
    if(interactive) {
        focus_add_clickable(shuffle, playlists_shuffle_action, NULL, NULL, true);
    }
    else {
        touch_add_clickable(shuffle, playlists_shuffle_touch_cb, NULL);
    }

    lv_obj_t * play = make_icon_button(
        controls, ICON_PLAY_OUTLINE, 56, 56, COL_TEXT, &bootstrap_icons_32);
    if(interactive) {
        playlists_play_focus = focus_add_clickable(play, playlists_play_action,
                                                   NULL, NULL, true);
    }
    else {
        touch_add_clickable(play, playlists_play_touch_cb, NULL);
        playlists_play_focus = -1;
    }

    lv_obj_t * track_list = make_scroll(parent);
    lv_obj_set_flex_grow(track_list, 0);
    lv_obj_set_height(track_list, PLAYLIST_TRACK_LIST_H);
    lv_obj_set_style_pad_row(track_list, 0, 0);
    lv_obj_set_style_pad_right(track_list, 14, 0);
    int count = player_playlist_track_count(playlists_selected);
    /* The preview stays short; the opened detail shows the whole playlist. */
    int max_tracks = interactive ? count : DETAIL_PREVIEW_MAX_TRACKS;
    if(max_tracks > count) max_tracks = count;
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
        ui_text_fade(track_title);

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
    if(max_tracks < count) {
        lv_obj_t * more = make_label(track_list, "More tracks not shown",
                                     FONT_CAPTION, COL_MUTED);
        lv_obj_set_height(more, 32);
    }
}

static void playlists_swipe_page(int direction)
{
    if(playlists_left_list == NULL) return;
    focus_move(direction * (PLAYLISTS_VISIBLE_ROWS - 1));
    if(!playlists_detail_open) {
        playlists_selected = g_focus_index;
        playlists_update_count();
        playlists_schedule_detail();
    }
    playlists_swipe_guard = lv_tick_get();
}

static void playlists_create(lv_obj_t * parent)
{
    playlists_play_focus = -1;
    if(playlists_detail_timer) lv_timer_pause(playlists_detail_timer);
    playlists_detail_dirty = false;
    playlists_right_content = NULL;
    playlists_left_list = NULL;
    playlists_counter.group = NULL;

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
    lv_obj_set_style_pad_all(left, 16, 0);
    lv_obj_set_style_pad_row(left, 0, 0);

    lv_obj_t * head = make_page_header(left, "Playlists", 56);
    counter_create(head, &playlists_counter);

    lv_obj_t * list = make_scroll(left);
    lv_obj_set_flex_grow(list, 0);
    lv_obj_set_height(list, PLAYLISTS_LIST_H);
    lv_obj_set_style_pad_row(list, 0, 0);
    playlists_left_list = list;
    for(int i = 0; i < 64; i++) playlists_name_labels[i] = NULL;

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
        lv_obj_set_style_pad_right(text, 14, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(text, false);

        lv_obj_t * name = make_label(text, playlist.name, FONT_H2, COL_TEXT);
        if(i < 64) playlists_name_labels[i] = name;
        lv_obj_set_width(name, 0);
        lv_obj_set_flex_grow(name, 1);
        ui_text_fade(name);

        char meta[16];
        snprintf(meta, sizeof(meta), "%u", playlist.track_count);
        make_label(text, meta, FONT_CAPTION, COL_MUTED);

        if(i == playlists_selected) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT_DARK), 0);
            theme_mark_selected(row, true);
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
    playlists_right_content = make_panel_content(right, 16);
    lv_obj_set_style_pad_row(playlists_right_content, 0, 0);
    if(g_page_loading && !playlists_detail_open) {
        make_label(playlists_right_content, "Loading...", FONT_CAPTION, COL_MUTED);
        playlists_schedule_detail();
        g_page_detail_timer = playlists_detail_timer;
    }
    else playlists_render_detail(playlists_right_content, playlists_detail_open);

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, playlists_left_scroll_y, LV_ANIM_OFF);

    playlists_update_count();
    if(playlists_detail_open) {
        focus_select(playlists_play_focus >= 0 ? playlists_play_focus : 0);
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

    lv_obj_t * head = make_page_header(root, "Bluetooth Remote", 48);
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
    SETTING_AFC_IAP2 = 0,
    SETTING_BLUETOOTH,
    SETTING_KEY_FEEDBACK,
    SETTING_THEME,
    SETTING_NOW_PLAYING_BG,
    SETTING_USB,
    SETTING_BOOT,
    SETTING_ABOUT,
    SETTING_DATETIME,
    SETTING_COUNT,
};

static int settings_selected;
static bool settings_detail_open;
static lv_obj_t * settings_right_content;
static counter_t settings_counter;
static lv_obj_t * settings_detail_time_format_value;
static lv_obj_t * settings_detail_set_time_value;
static lv_obj_t * settings_detail_ble_value;
static lv_obj_t * settings_detail_usb_a_check;
static lv_obj_t * settings_detail_usb_both_check;
static lv_obj_t * settings_detail_boot_off_check;
static lv_obj_t * settings_detail_boot_anim1_check;
static lv_obj_t * settings_detail_theme_gray_check;
static lv_obj_t * settings_detail_theme_aqua_check;
static lv_obj_t * settings_detail_theme_win95_check;
static lv_obj_t * settings_time_ampm_value;
static lv_obj_t * settings_time_ampm_column;
static lv_obj_t * settings_time_ampm_spacer;
static lv_obj_t * settings_time_year_value;
static lv_obj_t * settings_time_month_value;
static lv_obj_t * settings_time_day_value;
static lv_obj_t * settings_time_hour_value;
static lv_obj_t * settings_time_minute_value;
static lv_obj_t * settings_time_up_button;
static lv_obj_t * settings_time_down_button;
static lv_obj_t * settings_time_up_icon;
static lv_obj_t * settings_time_down_icon;
static bool settings_time_open;
static bool settings_time_editing;
static bool settings_time_blink_on;
static bool settings_detail_dirty;
static lv_timer_t * settings_detail_timer;
static uint32_t settings_ble_rev;
static int settings_restore_focus = -1;
enum {
    SETTINGS_TIME_NONE = 0,
    SETTINGS_TIME_YEAR,
    SETTINGS_TIME_MONTH,
    SETTINGS_TIME_DAY,
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
static lv_obj_t * settings_detail_np_bg_mesh_check;
static lv_obj_t * settings_detail_screen_value;
static lv_obj_t * settings_detail_wifi_mode_value;
static lv_obj_t * settings_detail_wifi_status_value;
static lv_obj_t * settings_detail_volume_value;
static lv_obj_t * settings_detail_sound_touch_check;
static lv_obj_t * settings_detail_sound_both_check;
static lv_obj_t * settings_detail_sound_none_check;
static int settings_brightness = 70;

typedef enum {
    USB_MODE_HOST = 0,
    USB_MODE_BOTH = 2,
} usb_mode_t;

static usb_mode_t g_usb_mode = USB_MODE_HOST;

void ui_apply_saved_settings(const ui_system_config_t * cfg)
{
    if(cfg == NULL) return;

    g_system_config = *cfg;
    if(g_system_config.sound_mode < 0) g_system_config.sound_mode = 0;
    if(g_system_config.sound_mode > 2) g_system_config.sound_mode = 2;
    g_system_config.sound_enabled = g_system_config.sound_mode != 0;

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
    g_usb_mode = cfg->usb_mode == 2 ? USB_MODE_BOTH : USB_MODE_HOST;
    g_theme = ui_theme_get(cfg->theme);
    g_system_config.theme = g_theme->id;
    g_system_config.boot_screen = cfg->boot_screen;
    g_system_config.ble_enabled = cfg->ble_enabled;

    ui_set_menubar_left(cfg->menubar_left != 0);
    ui_set_touch_enabled(cfg->touch_enabled != 0);
}

static void settings_rebuild(void);

static const char * const settings_names[SETTING_COUNT] = {
    "iOS Device",
    "Bluetooth",
    "Key Feedback",
    "Theme",
    "Now Playing Background",
    "USB Options",
    "Boot Screen",
    "About",
    "Clock",
};

static int settings_initial_focus(int index)
{
    if(index == SETTING_NOW_PLAYING_BG) {
        return g_np_bg_mode == NP_BG_BLURRED_COVER
                   ? NP_BG_COVER_GRADIENT : (int)g_np_bg_mode;
    }
    if(index == SETTING_USB) {
        return g_usb_mode == USB_MODE_BOTH ? 1 : 0;
    }
    if(index == SETTING_THEME) {
        return g_system_config.theme >= 0 && g_system_config.theme <= 2
                   ? g_system_config.theme : 0;
    }
    return 0;
}

static void settings_update_count(void)
{
    if(settings_counter.group == NULL) return;
    counter_set(&settings_counter, settings_selected + 1, SETTING_COUNT);
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

static void settings_current_datetime(struct tm * out)
{
    if(out == NULL) return;
    time_t now = time(NULL) + (time_t)g_clock_offset_min * 60;
    localtime_r(&now, out);
}

static int settings_hour12(int hour)
{
    int h = hour % 12;
    return h == 0 ? 12 : h;
}

static void settings_update_time_labels(void)
{
    struct tm tm_now;
    settings_current_datetime(&tm_now);
    if(settings_detail_set_time_value) {
        char text[64];
        snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min);
        lv_label_set_text(settings_detail_set_time_value, text);
    }
    if(settings_time_year_value) {
        lv_label_set_text_fmt(settings_time_year_value, "%04d",
                              tm_now.tm_year + 1900);
    }
    if(settings_time_month_value) {
        lv_label_set_text_fmt(settings_time_month_value, "%02d",
                              tm_now.tm_mon + 1);
    }
    if(settings_time_day_value) {
        lv_label_set_text_fmt(settings_time_day_value, "%02d",
                              tm_now.tm_mday);
    }
    if(settings_time_hour_value) {
        lv_label_set_text_fmt(settings_time_hour_value, "%02d",
                              g_clock_12h ? settings_hour12(tm_now.tm_hour)
                                          : tm_now.tm_hour);
    }
    if(settings_time_minute_value) {
        lv_label_set_text_fmt(settings_time_minute_value, "%02d",
                              tm_now.tm_min);
    }
    if(settings_time_ampm_value) {
        lv_label_set_text(settings_time_ampm_value,
                          tm_now.tm_hour < 12 ? "AM" : "PM");
    }
    if(settings_time_ampm_column) {
        lv_obj_set_hidden(settings_time_ampm_column, !g_clock_12h);
    }
    if(settings_time_ampm_spacer) {
        lv_obj_set_hidden(settings_time_ampm_spacer, g_clock_12h);
    }
}

void ui_clock_synced(void)
{
    g_clock_offset_min = 0;
    update_clock();
    settings_update_time_labels();
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
    g_system_config.usb_mode = (int)g_usb_mode;

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

static void settings_key_sound_update_checks(void)
{
    int mode = g_system_config.sound_mode;
    if(settings_detail_sound_touch_check) {
        lv_label_set_text(settings_detail_sound_touch_check,
                          mode == 1 ? ICON_CHECK : "");
    }
    if(settings_detail_sound_both_check) {
        lv_label_set_text(settings_detail_sound_both_check,
                          mode == 2 ? ICON_CHECK : "");
    }
    if(settings_detail_sound_none_check) {
        lv_label_set_text(settings_detail_sound_none_check,
                          mode == 0 ? ICON_CHECK : "");
    }
}

static void settings_key_sound_action(void * user)
{
    int mode = (int)(intptr_t)user;
    g_system_config.sound_mode = mode;
    g_system_config.sound_enabled = mode != 0;
    settings_key_sound_update_checks();
    settings_system_changed();
    /* Keep the cursor on the chosen row: the volume row is 0, then Touch,
     * Touch + Key and None. */
    if(g_focus_count > 0) {
        focus_select(mode == 1 ? 1 : (mode == 2 ? 2 : 3));
    }
    /* Audible confirmation of the selected mode. */
    if(mode != 0) {
        ui_feedback_emit(UI_FEEDBACK_ACTIVATE, mode == 2);
    }
}

static void settings_key_sound_touch_cb(lv_event_t * event)
{
    settings_key_sound_action(lv_event_get_user_data(event));
}

static void settings_volume_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_volume_action(NULL);
}

static void settings_update_np_bg_checks(void)
{
    if(settings_detail_np_bg_theme_check) {
        lv_label_set_text(settings_detail_np_bg_theme_check,
                          g_np_bg_mode == NP_BG_THEME ? ICON_CHECK : "");
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
    settings_system_changed();
}

static void settings_time_format_touch_cb(lv_event_t * event)
{
    settings_time_format_action(lv_event_get_user_data(event));
}

static void settings_time_apply_blink(void);

static void settings_time_format_focus_cb(void * user)
{
    LV_UNUSED(user);
    settings_time_segment = SETTINGS_TIME_NONE;
    settings_time_editing = false;
    settings_time_apply_blink();
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
    struct tm tm_now;
    settings_current_datetime(&tm_now);
    time_t old_time = time(NULL) + (time_t)g_clock_offset_min * 60;

    switch(settings_time_segment) {
        case SETTINGS_TIME_YEAR:
            tm_now.tm_year += direction;
            break;
        case SETTINGS_TIME_MONTH:
            tm_now.tm_mon += direction;
            break;
        case SETTINGS_TIME_DAY:
            tm_now.tm_mday += direction;
            break;
        case SETTINGS_TIME_HOUR:
            if(g_clock_12h) {
                int pm = tm_now.tm_hour >= 12 ? 12 : 0;
                int hour12 = settings_hour12(tm_now.tm_hour);
                hour12 = ((hour12 - 1 + direction) % 12 + 12) % 12 + 1;
                tm_now.tm_hour = (hour12 % 12) + pm;
            }
            else {
                tm_now.tm_hour += direction;
            }
            break;
        case SETTINGS_TIME_AMPM:
            tm_now.tm_hour = (tm_now.tm_hour + 12) % 24;
            break;
        case SETTINGS_TIME_MINUTE:
            tm_now.tm_min += direction;
            break;
        default:
            return;
    }
    tm_now.tm_isdst = -1;
    time_t new_time = mktime(&tm_now);
    if(new_time == (time_t)-1) return;
    if(g_time_changed_cb != NULL) {
        /* Real hardware: commit immediately so the RTC keeps the new time. */
        g_time_changed_cb(new_time);
        g_clock_offset_min = 0;
    }
    else {
        /* Simulator: keep the old in-memory offset behaviour. */
        int64_t delta = (int64_t)new_time - (int64_t)old_time;
        g_clock_offset_min += (int)(delta / 60);
    }
    settings_update_time_labels();
    update_clock();
}

static bool settings_key(ui_key_t key)
{
    if(!settings_time_open) return false;
    if(!settings_time_editing) {
        if(key == UI_KEY_ENTER && settings_time_segment != SETTINGS_TIME_NONE) {
            settings_time_start_edit((void *)(intptr_t)settings_time_segment);
            return true;
        }
        return false; /* Up/Down move focus; Enter acts on the focused row */
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
    if(!settings_time_editing) {
        if(settings_time_segment == SETTINGS_TIME_NONE) {
            settings_time_segment = SETTINGS_TIME_YEAR;
        }
        settings_time_start_edit((void *)(intptr_t)settings_time_segment);
    }
    settings_time_adjust(1);
    lv_event_stop_bubbling(event);
}

static void settings_time_down_cb(lv_event_t * event)
{
    if(!settings_time_editing) {
        if(settings_time_segment == SETTINGS_TIME_NONE) {
            settings_time_segment = SETTINGS_TIME_YEAR;
        }
        settings_time_start_edit((void *)(intptr_t)settings_time_segment);
    }
    settings_time_adjust(-1);
    lv_event_stop_bubbling(event);
}

static void settings_time_apply_blink(void)
{
    bool active = settings_time_open && settings_detail_open;
    bool edit = settings_time_editing;
    bool blink = !edit || settings_time_blink_on;
    lv_obj_t * fields[] = {
        settings_time_year_value, settings_time_month_value,
        settings_time_day_value, settings_time_hour_value,
        settings_time_minute_value, settings_time_ampm_value,
    };
    int segments[] = {
        SETTINGS_TIME_YEAR, SETTINGS_TIME_MONTH, SETTINGS_TIME_DAY,
        SETTINGS_TIME_HOUR, SETTINGS_TIME_MINUTE, SETTINGS_TIME_AMPM,
    };
    for(size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if(fields[i] == NULL) continue;
        bool on = active && settings_time_segment == segments[i] && blink;
        lv_obj_set_style_bg_opa(fields[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        theme_mark_selected(fields[i], on);
    }

    uint32_t arrow_fg = edit ? COL_TEXT : COL_DIM;
    uint32_t arrow_bg = edit ? COL_FOCUS : COL_ACCENT_DARK;
    if(g_theme->id != UI_THEME_GRAY) {
        arrow_bg = g_theme->button_bottom;
    }
    lv_obj_t * buttons[] = {
        settings_time_down_button,
        settings_time_up_button,
    };
    lv_obj_t * icons[] = {
        settings_time_down_icon,
        settings_time_up_icon,
    };
    for(size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        if(buttons[i] != NULL) {
            lv_obj_set_style_bg_color(buttons[i], lv_color_hex(arrow_bg), 0);
        }
        if(icons[i] != NULL) {
            lv_obj_set_style_text_color(icons[i],
                                        lv_color_hex(arrow_fg), 0);
        }
    }
}

static void settings_time_blink_tick(void)
{
    if(g_current_screen != SCR_SETTINGS || !settings_time_open ||
       !settings_detail_open || !settings_time_editing ||
       settings_time_segment == SETTINGS_TIME_NONE) {
        return;
    }
    settings_time_blink_on = !settings_time_blink_on;
    settings_time_apply_blink();
}

static lv_obj_t * settings_time_touch_button(lv_obj_t * parent,
                                             const char * icon,
                                             lv_event_cb_t callback,
                                             lv_obj_t ** icon_out)
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
    if(callback != NULL) {
        lv_obj_set_clickable(button, true);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    }
    else {
        lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t * label = make_label(button, icon, &bootstrap_icons_32, COL_DIM);
    theme_button(button);
    if(icon_out != NULL) *icon_out = label;
    return button;
}

#define SETTINGS_ROW_H 64

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
    lv_obj_t * row = make_row(parent, SETTINGS_ROW_H);
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
                                             SETTINGS_ROW_H, FONT_H2,
                                             FONT_H2, NULL);
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

static lv_obj_t * settings_make_info_row(lv_obj_t * parent,
                                         const char * label,
                                         const char * value,
                                         const lv_font_t * label_font,
                                         const lv_font_t * value_font,
                                         int height)
{
    lv_obj_t * row = make_row(parent, height);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_t * name = make_label(row, label, label_font, COL_MUTED);
    lv_obj_set_flex_grow(name, 1);
    ui_text_fade(name);
    lv_obj_t * val = make_label(row, value, value_font, COL_MUTED);
    lv_obj_set_width(val, 360);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    ui_text_fade(val);
    return row;
}

static void settings_make_separator(lv_obj_t * parent, const char * left,
                                    const char * right)
{
    lv_obj_t * row = make_row(parent, 34);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_t * l = make_label(row, left, FONT_CAPTION, COL_MUTED);
    lv_obj_set_flex_grow(l, 1);
    ui_text_fade(l);
    lv_obj_t * r = make_label(row, right, FONT_CAPTION, COL_DIM);
    lv_obj_set_width(r, 430);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    ui_text_fade(r);
}

static void settings_update_theme_checks(void)
{
    int theme = g_system_config.theme;
    if(settings_detail_theme_gray_check) {
        lv_label_set_text(settings_detail_theme_gray_check,
                          theme == 0 ? ICON_CHECK : "");
    }
    if(settings_detail_theme_aqua_check) {
        lv_label_set_text(settings_detail_theme_aqua_check,
                          theme == 1 ? ICON_CHECK : "");
    }
    if(settings_detail_theme_win95_check) {
        lv_label_set_text(settings_detail_theme_win95_check,
                          theme == 2 ? ICON_CHECK : "");
    }
}

static void settings_theme_action(void * user)
{
    int theme = (int)(intptr_t)user;
    if(theme < 0 || theme > 2) return;
    g_system_config.theme = theme;
    settings_update_theme_checks();
    settings_system_changed();
}

static void settings_theme_touch_cb(lv_event_t * event)
{
    settings_theme_action(lv_event_get_user_data(event));
}

static void settings_ble_toggle_action(void * user)
{
    LV_UNUSED(user);
    bool enabled = !g_system_config.ble_enabled;
    g_system_config.ble_enabled = enabled ? 1 : 0;
    ble_remote_set_enabled(enabled);
    if(enabled) {
        ble_remote_scan_start();
    }
    settings_system_changed();
    settings_ble_rev = ble_remote_revision();
    settings_restore_focus = g_focus_index;
    ui_rebuild_page();
}

static void settings_ble_toggle_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_ble_toggle_action(NULL);
}

static void settings_ble_device_action(void * user)
{
    int index = (int)(intptr_t)user;
    ble_remote_device_t dev;
    if(ble_remote_paired_get(index, &dev)) {
        ble_remote_connect(dev.addr);
    }
    settings_rebuild();
}

static void settings_ble_device_async(void * arg)
{
    settings_ble_device_action(arg);
}

static void settings_ble_device_click_cb(lv_event_t * event)
{
    settings_ble_device_action(lv_event_get_user_data(event));
}

static void settings_ble_forget_action(void * user)
{
    int index = (int)(intptr_t)user;
    ble_remote_device_t dev;
    if(ble_remote_paired_get(index, &dev)) {
        ble_remote_unpair(dev.addr);
    }
    settings_rebuild();
}

static void settings_ble_forget_async(void * arg)
{
    settings_ble_forget_action(arg);
}

static void settings_ble_forget_cb(lv_event_t * event)
{
    ui_call_async(settings_ble_forget_async,
                  lv_event_get_user_data(event));
}

static void settings_ble_pair_action(void * user)
{
    LV_UNUSED(user);
    g_ble_return_screen = SCR_SETTINGS;
    ui_show_screen(SCR_BLUETOOTH);
}

static void settings_ble_pair_touch_cb(lv_event_t * event)
{
    LV_UNUSED(event);
    settings_ble_pair_action(NULL);
}

static void settings_ble_scan_connect_action(void * user)
{
    int index = (int)(intptr_t)user;
    ble_remote_device_t dev;
    if(ble_remote_scan_get(index, &dev)) {
        ble_remote_connect(dev.addr);
    }
    settings_ble_rev = ble_remote_revision();
    settings_restore_focus = g_focus_index;
    ui_rebuild_page();
}

static void settings_ble_scan_connect_async(void * arg)
{
    settings_ble_scan_connect_action(arg);
}

static void settings_ble_scan_connect_cb(lv_event_t * event)
{
    ui_call_async(settings_ble_scan_connect_async,
                  lv_event_get_user_data(event));
}

static void settings_tick(void)
{
    if(g_current_screen != SCR_SETTINGS ||
       settings_selected != SETTING_BLUETOOTH || !settings_detail_open) {
        return;
    }
    uint32_t rev = ble_remote_revision();
    if(rev == settings_ble_rev) return;
    settings_ble_rev = rev;
    settings_restore_focus = g_focus_index;
    ui_rebuild_page();
}

static void settings_update_usb_checks(void)
{
    if(settings_detail_usb_a_check) {
        lv_label_set_text(settings_detail_usb_a_check,
                          g_usb_mode == USB_MODE_HOST ? ICON_CHECK : "");
    }
    if(settings_detail_usb_both_check) {
        lv_label_set_text(settings_detail_usb_both_check,
                          g_usb_mode == USB_MODE_BOTH ? ICON_CHECK : "");
    }
}

static void settings_usb_mode_action(void * user)
{
    int mode = (int)(intptr_t)user;
    g_usb_mode = mode == 2 ? USB_MODE_BOTH : USB_MODE_HOST;
    settings_update_usb_checks();
    settings_system_changed();
    ui_set_hint("Reboot to apply USB mode");
}

static void settings_usb_mode_touch_cb(lv_event_t * event)
{
    settings_usb_mode_action(lv_event_get_user_data(event));
}

static void settings_boot_screen_action(void * user)
{
    g_system_config.boot_screen = (int)(intptr_t)user;
    if(settings_detail_boot_off_check) {
        lv_label_set_text(settings_detail_boot_off_check,
                          g_system_config.boot_screen == 0 ? ICON_CHECK : "");
    }
    if(settings_detail_boot_anim1_check) {
        lv_label_set_text(settings_detail_boot_anim1_check,
                          g_system_config.boot_screen == 1 ? ICON_CHECK : "");
    }
    settings_system_changed();
}

static void settings_boot_screen_touch_cb(lv_event_t * event)
{
    settings_boot_screen_action(lv_event_get_user_data(event));
}

static const char * settings_compile_time(void)
{
    static char text[48];
    if(text[0] != '\0') return text;
    char month[4] = "Jan";
    int day = 1, year = 2026, hour = 0, minute = 0, second = 0;
    sscanf(__DATE__, "%3s %d %d", month, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
    int hour12 = hour % 12;
    if(hour12 == 0) hour12 = 12;
    snprintf(text, sizeof(text), "%s %d %d  %d:%02d %s", month, day, year,
             hour12, minute, hour < 12 ? "AM" : "PM");
    return text;
}

static const char * settings_commit(void)
{
    static char text[32];
    if(text[0] != '\0') return text;
#if defined(ESP_PLATFORM)
    const esp_app_desc_t * app = esp_app_get_description();
    snprintf(text, sizeof(text), "%s", app != NULL ? app->version : "unknown");
    char * dash = strchr(text, '-');
    if(dash != NULL) *dash = '\0';
#else
    snprintf(text, sizeof(text), "local");
#endif
    return text;
}

static void settings_time_field(lv_obj_t * parent, const char * caption,
                                int width, int segment, bool interactive,
                                lv_obj_t ** value_out)
{
    lv_obj_t * column = lv_obj_create(parent);
    lv_obj_set_size(column, width, 104);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 0, 0);
    lv_obj_set_style_pad_row(column, 2, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(column, false);

    lv_obj_t * value = make_label(column, "00", FONT_CLOCK_LG, COL_TEXT);
    lv_obj_set_size(value, width, 72);
    lv_obj_set_style_pad_top(value, 12, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(value, lv_color_hex(COL_FOCUS), 0);
    lv_obj_set_style_bg_opa(value, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(value, 0, 0);
    if(interactive) {
        int index = focus_add_clickable(value, settings_time_start_edit, NULL,
                                        (void *)(intptr_t)segment, false);
        focus_set_silent(index, true);
        focus_set_on_focus(index, settings_time_segment_focus_cb);
    }
    make_label(column, caption, FONT_CAPTION, COL_MUTED);
    if(value_out != NULL) *value_out = value;
}

static void settings_render_time_editor(lv_obj_t * parent, bool interactive)
{
    settings_time_year_value = NULL;
    settings_time_month_value = NULL;
    settings_time_day_value = NULL;
    settings_time_hour_value = NULL;
    settings_time_minute_value = NULL;
    settings_time_ampm_value = NULL;
    settings_time_ampm_column = NULL;
    settings_time_ampm_spacer = NULL;
    settings_time_up_button = NULL;
    settings_time_down_button = NULL;
    settings_time_up_icon = NULL;
    settings_time_down_icon = NULL;

    lv_obj_t * format_row = settings_make_value_row(
        parent, "FORMAT", g_clock_12h ? "12H" : "24H", COL_ACCENT,
        SETTINGS_ROW_H, FONT_H2, FONT_H2, &settings_detail_time_format_value);
    if(interactive) {
        int index = focus_add_clickable(format_row, settings_time_format_action,
                                        NULL, NULL, true);
        focus_set_on_focus(index, settings_time_format_focus_cb);
    }
    else {
        touch_add_clickable(format_row, settings_time_format_touch_cb, NULL);
    }

    lv_obj_t * hint = make_label(parent, "UP/DOWN select   ENTER edit/confirm   BACK close",
                                 FONT_CAPTION, COL_MUTED);
    lv_obj_set_style_pad_left(hint, 16, 0);

    lv_obj_t * editor = lv_obj_create(parent);
    lv_obj_set_size(editor, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(editor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(editor, 0, 0);
    lv_obj_set_style_pad_all(editor, 0, 0);
    lv_obj_set_style_pad_row(editor, 12, 0);
    lv_obj_set_flex_flow(editor, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(editor, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(editor, false);

    lv_obj_t * box = lv_obj_create(editor);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(box, false);

    lv_obj_t * date_row = lv_obj_create(box);
    lv_obj_set_size(date_row, LV_PCT(100), 108);
    lv_obj_set_style_bg_opa(date_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(date_row, 0, 0);
    lv_obj_set_style_pad_all(date_row, 0, 0);
    lv_obj_set_style_pad_column(date_row, 18, 0);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(date_row, false);
    settings_time_field(date_row, "YEAR", 180, SETTINGS_TIME_YEAR,
                        interactive, &settings_time_year_value);
    settings_time_field(date_row, "MONTH", 180, SETTINGS_TIME_MONTH,
                        interactive, &settings_time_month_value);
    settings_time_field(date_row, "DAY", 180, SETTINGS_TIME_DAY,
                        interactive, &settings_time_day_value);

    lv_obj_t * time_row = lv_obj_create(box);
    lv_obj_set_size(time_row, LV_PCT(100), 108);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_style_pad_column(time_row, 18, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(time_row, false);
    settings_time_field(time_row, "HOUR", 180, SETTINGS_TIME_HOUR,
                        interactive, &settings_time_hour_value);
    settings_time_field(time_row, "MINUTE", 180, SETTINGS_TIME_MINUTE,
                        interactive, &settings_time_minute_value);
    settings_time_field(time_row, "AM/PM", 180, SETTINGS_TIME_AMPM,
                        interactive, &settings_time_ampm_value);
    settings_time_ampm_column = lv_obj_get_parent(settings_time_ampm_value);
    settings_time_ampm_spacer = lv_obj_create(time_row);
    lv_obj_set_size(settings_time_ampm_spacer, 180, 104);
    lv_obj_set_style_bg_opa(settings_time_ampm_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_time_ampm_spacer, 0, 0);
    lv_obj_set_style_pad_all(settings_time_ampm_spacer, 0, 0);
    lv_obj_set_scrollable(settings_time_ampm_spacer, false);
    lv_obj_clear_flag(settings_time_ampm_spacer, LV_OBJ_FLAG_CLICKABLE);

    /* Down on the left, up on the right.  Both stay visible and turn grey
     * while the editor is not in adjustment mode. */
    lv_obj_t * arrows = lv_obj_create(editor);
    lv_obj_set_size(arrows, LV_PCT(100), 64);
    lv_obj_set_style_bg_opa(arrows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arrows, 0, 0);
    lv_obj_set_style_pad_all(arrows, 0, 0);
    lv_obj_set_style_pad_top(arrows, 16, 0);
    lv_obj_set_style_pad_column(arrows, 28, 0);
    lv_obj_set_flex_flow(arrows, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arrows, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(arrows, false);

    settings_time_down_button = settings_time_touch_button(
        arrows, ICON_CHEVRON_DOWN, settings_time_down_cb,
        &settings_time_down_icon);
    settings_time_up_button = settings_time_touch_button(
        arrows, ICON_CHEVRON_UP, settings_time_up_cb,
        &settings_time_up_icon);
    lv_obj_set_size(settings_time_down_button, 220, 64);
    lv_obj_set_size(settings_time_up_button, 220, 64);

    settings_update_time_labels();
    settings_time_apply_blink();
}

static void settings_np_bg_touch_cb(lv_event_t * event)
{
    settings_np_bg_action(lv_event_get_user_data(event));
}

static void settings_render_detail(lv_obj_t * parent, bool interactive)
{
    if(parent == NULL) return;
    lv_obj_clean(parent);

    settings_detail_time_format_value = NULL;
    settings_detail_set_time_value = NULL;
    settings_detail_ble_value = NULL;
    settings_detail_usb_a_check = NULL;
    settings_detail_usb_both_check = NULL;
    settings_detail_boot_off_check = NULL;
    settings_detail_boot_anim1_check = NULL;
    settings_detail_theme_gray_check = NULL;
    settings_detail_theme_aqua_check = NULL;
    settings_detail_theme_win95_check = NULL;
    settings_detail_screen_value = NULL;
    settings_detail_wifi_mode_value = NULL;
    settings_detail_wifi_status_value = NULL;
    settings_detail_volume_value = NULL;
    settings_detail_sound_touch_check = NULL;
    settings_detail_sound_both_check = NULL;
    settings_detail_sound_none_check = NULL;
    settings_time_year_value = NULL;
    settings_time_month_value = NULL;
    settings_time_day_value = NULL;
    settings_time_hour_value = NULL;
    settings_time_minute_value = NULL;
    settings_detail_touch_value = NULL;
    settings_detail_menubar_value = NULL;
    settings_detail_brightness_value = NULL;
    settings_detail_sort_value = NULL;
    settings_detail_order_value = NULL;
    settings_detail_np_bg_theme_check = NULL;
    settings_detail_np_bg_mesh_check = NULL;

    int index = settings_selected;
    if(index < 0 || index >= SETTING_COUNT) index = 0;

    switch(index) {
        case SETTING_AFC_IAP2: {
            ui_afc_status_t st;
            settings_afc_read_status(&st);
            if(st.afc_mode) {
                settings_afc_action_row(parent, "Enter iAP2 Mode", "Reboot",
                                        UI_AFC_ACTION_IPOD_MODE, interactive);
            }
            else {
                settings_afc_action_row(parent, "Enter AFC Mode", "Reboot",
                                        UI_AFC_ACTION_PAIR, interactive);
            }

            char value[96];
            char size[24];
            int total_albums = player_library_album_count();
            snprintf(value, sizeof(value), "%d/%d", st.cached_albums,
                     total_albums);
            settings_make_info_row(parent, "Albums Cached", value,
                                   FONT_H2, FONT_H2, SETTINGS_ROW_H);
            if(st.library_db_bytes > 0) {
                settings_format_bytes(st.library_db_bytes, size, sizeof(size));
            }
            else {
                snprintf(size, sizeof(size), "—");
            }
            settings_make_info_row(parent, "Database Size", size,
                                   FONT_H2, FONT_H2, SETTINGS_ROW_H);
            if(st.cover_cache_bytes > 0) {
                settings_format_bytes(st.cover_cache_bytes, size, sizeof(size));
            }
            else {
                snprintf(size, sizeof(size), "—");
            }
            settings_make_info_row(parent, "Cover Cache Size", size,
                                   FONT_H2, FONT_H2, SETTINGS_ROW_H);
            settings_make_info_row(parent, "Library ID",
                                   st.library_suffix[0] ? st.library_suffix : "—",
                                   FONT_H2, FONT_H2, SETTINGS_ROW_H);
            settings_make_info_row(parent, "iAP2 Status",
                                   st.iap2_status[0] ? st.iap2_status : "—",
                                   FONT_H2, FONT_H2, SETTINGS_ROW_H);
            player_snapshot_t snap;
            player_get_snapshot(&snap);
            char battery[48];
            if(snap.battery >= 0) {
                snprintf(battery, sizeof(battery), "%s  %d%%",
                         snap.charging ? "CHG" : "NOT CHG", snap.battery);
            }
            else {
                snprintf(battery, sizeof(battery), "%s  --%%",
                         snap.charging ? "CHG" : "NOT CHG");
            }
            settings_make_info_row(parent, "Battery", battery,
                                   FONT_H2, FONT_H2, SETTINGS_ROW_H);
            break;
        }
        case SETTING_BLUETOOTH: {
            bool enabled = g_system_config.ble_enabled != 0;
            lv_obj_t * row = settings_make_value_row(
                parent, "Bluetooth", enabled ? "On" : "Off",
                enabled ? COL_GREEN : COL_MUTED, SETTINGS_ROW_H, FONT_H2, FONT_H2,
                &settings_detail_ble_value);
            if(interactive) {
                focus_add_clickable(row, settings_ble_toggle_action, NULL,
                                    NULL, true);
            }
            else {
                touch_add_clickable(row, settings_ble_toggle_touch_cb, NULL);
            }

            ble_remote_state_t state = BLE_REMOTE_OFF;
            ble_remote_status(&state, NULL, 0);
            bool scanning = state == BLE_REMOTE_SCANNING;
            settings_make_separator(parent, "Paired Devices",
                                    "PRESS: CONNECT   LONG_PRS: FORGET");
            int count = ble_remote_paired_count();
            if(count <= 0) {
                settings_make_info_row(parent, "No paired devices", "",
                                       FONT_BODY, FONT_CAPTION,
                                       SETTINGS_ROW_H);
            }
            for(int i = 0; i < count; i++) {
                ble_remote_device_t dev;
                if(!ble_remote_paired_get(i, &dev)) continue;
                lv_obj_t * device = make_row(parent, SETTINGS_ROW_H);
                lv_obj_set_style_pad_left(device, 16, 0);
                lv_obj_t * icon = make_label(device, ICON_BLUETOOTH,
                                             &bootstrap_icons_24, COL_ACCENT);
                lv_obj_set_width(icon, 28);
                lv_obj_t * name = make_label(device,
                                             dev.name[0] ? dev.name : dev.addr,
                                             FONT_H2, COL_TEXT);
                lv_obj_set_flex_grow(name, 1);
                ui_text_fade(name);
                bool connected = state == BLE_REMOTE_CONNECTED;
                lv_obj_t * status = make_label(
                    device, connected ? "Connected" : "Disconnected",
                    FONT_H2, connected ? COL_GREEN : COL_MUTED);
                lv_obj_set_width(status, 170);
                lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
                ui_text_fade(status);
                if(interactive) {
                    focus_add_clickable(device, settings_ble_device_action,
                                        NULL, (void *)(intptr_t)i, true);
                    lv_obj_add_event_cb(device, settings_ble_forget_cb,
                                        LV_EVENT_LONG_PRESSED,
                                        (void *)(intptr_t)i);
                }
            }
            int scan_count = ble_remote_scan_count();
            settings_make_separator(parent, "Search Devices",
                                    "PRESS: PAIR");
            if(scan_count <= 0 && scanning) {
                settings_make_info_row(parent, "Searching...", "",
                                       FONT_BODY, FONT_CAPTION,
                                       SETTINGS_ROW_H);
            }
            for(int i = 0; i < scan_count; i++) {
                ble_remote_device_t dev;
                if(!ble_remote_scan_get(i, &dev)) continue;
                lv_obj_t * device = make_row(parent, SETTINGS_ROW_H);
                lv_obj_set_style_pad_left(device, 16, 0);
                lv_obj_t * icon = make_label(device, ICON_BLUETOOTH,
                                             &bootstrap_icons_24,
                                             COL_MUTED);
                lv_obj_set_width(icon, 28);
                lv_obj_t * name = make_label(device,
                                             dev.name[0] ? dev.name
                                                         : dev.addr,
                                             FONT_H2, COL_TEXT);
                lv_obj_set_flex_grow(name, 1);
                ui_text_fade(name);
                lv_obj_t * status = make_label(device, "PAIR",
                                               FONT_H2, COL_ACCENT);
                lv_obj_set_width(status, 90);
                lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT,
                                            0);
                if(interactive) {
                    focus_add_clickable(device,
                                        settings_ble_scan_connect_action,
                                        NULL, (void *)(intptr_t)i, true);
                }
            }
            break;
        }
        case SETTING_KEY_FEEDBACK: {
            lv_obj_t * vol = settings_make_value_row(
                parent, "Volume", "", COL_TEXT, SETTINGS_ROW_H, FONT_H2,
                FONT_H2, &settings_detail_volume_value);
            char text[8];
            snprintf(text, sizeof(text), "%d%%", g_system_config.volume);
            lv_label_set_text(settings_detail_volume_value, text);
            if(interactive) {
                focus_add_clickable(vol, settings_volume_action,
                                    settings_volume_adjust, NULL, true);
            }
            else {
                touch_add_clickable(vol, settings_volume_touch_cb, NULL);
            }

            /* Inset between the level control and the mode list. */
            lv_obj_t * spacer = lv_obj_create(parent);
            lv_obj_set_size(spacer, LV_PCT(100), 14);
            lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(spacer, 0, 0);
            lv_obj_set_style_pad_all(spacer, 0, 0);
            lv_obj_set_scrollable(spacer, false);
            lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t * heading = make_label(parent, "Play sound on",
                                            FONT_GROUP, COL_MUTED);
            lv_obj_set_style_pad_left(heading, 16, 0);

            int mode = g_system_config.sound_mode;
            lv_obj_t * touch = settings_make_choice_row(
                parent, "Touch", mode == 1,
                &settings_detail_sound_touch_check);
            lv_obj_t * both = settings_make_choice_row(
                parent, "Touch + Key", mode == 2,
                &settings_detail_sound_both_check);
            lv_obj_t * none = settings_make_choice_row(
                parent, "None", mode == 0,
                &settings_detail_sound_none_check);
            if(interactive) {
                focus_add_clickable(touch, settings_key_sound_action, NULL,
                                    (void *)(intptr_t)1, true);
                focus_add_clickable(both, settings_key_sound_action, NULL,
                                    (void *)(intptr_t)2, true);
                focus_add_clickable(none, settings_key_sound_action, NULL,
                                    (void *)(intptr_t)0, true);
            }
            else {
                touch_add_clickable(touch, settings_key_sound_touch_cb,
                                    (void *)(intptr_t)1);
                touch_add_clickable(both, settings_key_sound_touch_cb,
                                    (void *)(intptr_t)2);
                touch_add_clickable(none, settings_key_sound_touch_cb,
                                    (void *)(intptr_t)0);
            }
            break;
        }
        case SETTING_THEME: {
            int theme = g_system_config.theme;
            lv_obj_t * gray = settings_make_choice_row(
                parent, "Gray", theme == 0,
                &settings_detail_theme_gray_check);
            lv_obj_t * aqua = settings_make_choice_row(
                parent, "Aqua", theme == 1,
                &settings_detail_theme_aqua_check);
            lv_obj_t * win95 = settings_make_choice_row(
                parent, "Windows 95", theme == 2,
                &settings_detail_theme_win95_check);
            if(interactive) {
                focus_add_clickable(gray, settings_theme_action, NULL,
                                    (void *)(intptr_t)0, true);
                focus_add_clickable(aqua, settings_theme_action, NULL,
                                    (void *)(intptr_t)1, true);
                focus_add_clickable(win95, settings_theme_action, NULL,
                                    (void *)(intptr_t)2, true);
            }
            else {
                touch_add_clickable(gray, settings_theme_touch_cb,
                                    (void *)(intptr_t)0);
                touch_add_clickable(aqua, settings_theme_touch_cb,
                                    (void *)(intptr_t)1);
                touch_add_clickable(win95, settings_theme_touch_cb,
                                    (void *)(intptr_t)2);
            }
            break;
        }
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

            lv_obj_t * mesh_row = settings_make_choice_row(
                parent, "Cover gradient",
                g_np_bg_mode == NP_BG_COVER_GRADIENT,
                &settings_detail_np_bg_mesh_check);
            if(interactive) {
                focus_add_clickable(mesh_row, settings_np_bg_action, NULL,
                                    (void *)(intptr_t)NP_BG_COVER_GRADIENT,
                                    true);
            }
            else {
                touch_add_clickable(mesh_row, settings_np_bg_touch_cb,
                                    (void *)(intptr_t)NP_BG_COVER_GRADIENT);
            }
            break;
        }
        case SETTING_USB: {
            lv_obj_t * usb_a = settings_make_choice_row(
                parent, "USB-A HighSpeed Only", g_usb_mode == USB_MODE_HOST,
                &settings_detail_usb_a_check);
            lv_obj_t * both = settings_make_choice_row(
                parent, "USB-A HS & USB-C OTG", g_usb_mode == USB_MODE_BOTH,
                &settings_detail_usb_both_check);
            if(interactive) {
                focus_add_clickable(usb_a, settings_usb_mode_action, NULL,
                                    (void *)(intptr_t)0, true);
                focus_add_clickable(both, settings_usb_mode_action, NULL,
                                    (void *)(intptr_t)2, true);
            }
            else {
                touch_add_clickable(usb_a, settings_usb_mode_touch_cb,
                                    (void *)(intptr_t)0);
                touch_add_clickable(both, settings_usb_mode_touch_cb,
                                    (void *)(intptr_t)2);
            }
            lv_obj_t * note = make_label(parent, "Reboot to apply", FONT_CAPTION,
                                         COL_DIM);
            lv_obj_set_style_pad_left(note, 16, 0);
            break;
        }
        case SETTING_BOOT: {
            lv_obj_t * off = settings_make_choice_row(
                parent, "OFF", g_system_config.boot_screen == 0,
                &settings_detail_boot_off_check);
            lv_obj_t * anim1 = settings_make_choice_row(
                parent, "KCN4M1", g_system_config.boot_screen == 1,
                &settings_detail_boot_anim1_check);
            if(interactive) {
                focus_add_clickable(off, settings_boot_screen_action, NULL,
                                    (void *)(intptr_t)0, true);
                focus_add_clickable(anim1, settings_boot_screen_action, NULL,
                                    (void *)(intptr_t)1, true);
            }
            else {
                touch_add_clickable(off, settings_boot_screen_touch_cb,
                                    (void *)(intptr_t)0);
                touch_add_clickable(anim1, settings_boot_screen_touch_cb,
                                    (void *)(intptr_t)1);
            }
            break;
        }
        case SETTING_ABOUT:
            settings_make_value_row(parent, "Compile Time",
                                    settings_compile_time(), COL_MUTED, SETTINGS_ROW_H, FONT_H2, FONT_H2, NULL);
            settings_make_value_row(parent, "Commit", settings_commit(),
                                    COL_MUTED, SETTINGS_ROW_H, FONT_H2, FONT_H2, NULL);
            break;
        case SETTING_DATETIME:
            settings_render_time_editor(parent, interactive);
            break;
        default:
            break;
    }
}

static void settings_detail_timer_cb(lv_timer_t * timer)
{
    lv_timer_pause(timer);
    if(g_current_screen != SCR_SETTINGS) return;
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
    if(g_page_detail_timer != settings_detail_timer) lv_timer_resume(settings_detail_timer);
}

/* The master-detail pages defer right-panel rendering to a 140 ms timer.
 * When a page is left or rebuilt before that timer fires, the callback would
 * otherwise touch widgets that lv_obj_clean() has already deleted.  Pause
 * every page timer and drop its dangling content pointer before teardown. */
static void ui_page_teardown(void)
{
    ui_page_cancel_load();
    if(np_queue_fill_timer) lv_timer_pause(np_queue_fill_timer);
    np_queue_wait_frame = false;
    np_queue_list = np_queue_loading = np_queue_spacer = NULL;
    np_toggle_frame_pending = false;
    ui_marquee_stop();
    albums_save_position();
    albums_cancel_work();
    albums_left_list = NULL;
    if(albums_detail_timer) lv_timer_pause(albums_detail_timer);
    albums_detail_dirty = false;
    albums_right_content = NULL;

    if(artists_detail_timer) lv_timer_pause(artists_detail_timer);
    artists_detail_dirty = false;
    artists_right_content = NULL;

    if(playlists_detail_timer) lv_timer_pause(playlists_detail_timer);
    playlists_detail_dirty = false;
    playlists_right_content = NULL;

    if(settings_detail_timer) lv_timer_pause(settings_detail_timer);
    settings_detail_dirty = false;
    settings_right_content = NULL;
}

static void settings_row_focus(void * user)
{
    int index = (int)(intptr_t)user;
    if(index == settings_selected) return;
    if(settings_selected == SETTING_BLUETOOTH &&
       index != SETTING_BLUETOOTH) {
        ble_remote_scan_stop();
    }
    settings_selected = index;
    settings_time_open = false;
    settings_time_editing = false;
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
    settings_time_open = index == SETTING_DATETIME;
    settings_time_editing = false;
    settings_time_segment = SETTINGS_TIME_YEAR;
    settings_time_blink_on = true;
    settings_update_count();
    settings_detail_open = true;
    settings_restore_focus = -1;
    if(index == SETTING_BLUETOOTH) {
        settings_ble_rev = ble_remote_revision();
        if(g_system_config.ble_enabled) {
            ble_remote_state_t state = BLE_REMOTE_OFF;
            ble_remote_status(&state, NULL, 0);
            if(state != BLE_REMOTE_CONNECTED &&
               state != BLE_REMOTE_CONNECTING) {
                ble_remote_scan_start();
            }
        }
    }
    settings_rebuild();
    if(g_focus_count > 0) {
        focus_select(settings_initial_focus(index));
    }
}

static void settings_enter(void)
{
    settings_detail_open = false;
    settings_time_open = false;
    settings_time_editing = false;
}

static bool settings_back(void)
{
    if(settings_selected == SETTING_BLUETOOTH && settings_detail_open) {
        ble_remote_scan_stop();
    }
    if(settings_time_open && settings_time_editing) {
        settings_time_editing = false;
        settings_time_blink_on = true;
        settings_time_apply_blink();
        return true;
    }
    if(settings_time_open) {
        settings_time_open = false;
        settings_detail_open = false;
        settings_rebuild();
        return true;
    }
    if(settings_detail_open) {
        settings_detail_open = false;
        settings_rebuild();
        return true;
    }
    if(settings_selected > 0) {
        settings_selected = 0;
        settings_update_count();
        if(settings_right_content) settings_schedule_detail();
        return true;
    }
    return false;
}

static void settings_same_key(void)
{
    if(settings_selected == SETTING_BLUETOOTH && settings_detail_open) {
        ble_remote_scan_stop();
    }
    if(settings_time_open) {
        settings_time_open = false;
        settings_detail_open = false;
        settings_rebuild();
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

static void settings_create(lv_obj_t * parent)
{
    if(settings_detail_timer) lv_timer_pause(settings_detail_timer);
    settings_detail_dirty = false;
    settings_right_content = NULL;
    settings_counter.group = NULL;
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
    settings_detail_np_bg_mesh_check = NULL;
    settings_detail_ble_value = NULL;
    settings_detail_usb_a_check = NULL;
    settings_detail_usb_both_check = NULL;
    settings_detail_boot_off_check = NULL;
    settings_detail_boot_anim1_check = NULL;
    settings_detail_theme_gray_check = NULL;
    settings_detail_theme_aqua_check = NULL;
    settings_detail_theme_win95_check = NULL;

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
    lv_obj_set_style_pad_all(left, 16, 0);
    lv_obj_set_style_pad_row(left, 0, 0);

    lv_obj_t * head = make_page_header(left, "Settings", 56);
    counter_create(head, &settings_counter);
    settings_update_count();

    for(int i = 0; i < SETTING_COUNT; i++) {
        lv_obj_t * row = make_row(left, 56);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_set_style_outline_width(row, 0, 0);
        make_label(row, settings_names[i], FONT_H2, COL_TEXT);

        if(i == settings_selected) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT_DARK), 0);
            theme_mark_selected(row, true);
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
    settings_right_content = make_panel_content(right, 16);
    lv_obj_set_style_pad_row(settings_right_content, 10, 0);
    lv_obj_set_scroll_dir(settings_right_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_right_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_elastic(settings_right_content, true);
    lv_obj_set_scrollable(settings_right_content, true);
    if(g_page_loading && !settings_detail_open) {
        make_label(settings_right_content, "Loading...", FONT_CAPTION, COL_MUTED);
        settings_schedule_detail();
        g_page_detail_timer = settings_detail_timer;
    }
    else settings_render_detail(settings_right_content, settings_detail_open);

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
    if(settings_restore_focus >= 0 && settings_restore_focus < g_focus_count) {
        focus_select(settings_restore_focus);
    }
    settings_restore_focus = -1;
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
        .enter = albums_enter,
        .same_key = albums_same_key,
        .key = albums_key,
    },
    [SCR_ARTISTS] = {
        .create = artists_create,
        .tick = NULL,
        .back = artists_back,
        .enter = artists_enter,
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
        .enter = playlists_enter,
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
        .tick = settings_tick,
        .back = settings_back,
        .enter = settings_enter,
        .same_key = settings_same_key,
        .key = settings_key,
    },
};

/* -------------------------------------------------------------------------
 * Dedicated AFC sync screen (boot mode 2: no media controls)
 * ------------------------------------------------------------------------- */
/* Fixed geometry mirrors the 1280 x 720 player, with one clear reading path. */
static lv_obj_t * afc_connection, * afc_trust_hint;
static lv_obj_t * afc_sd_text, * afc_sd_bar, * afc_library_updated;
static lv_obj_t * afc_metrics[3];
static lv_obj_t * afc_progress_title, * afc_progress_detail, * afc_progress_album;
static lv_obj_t * afc_progress_bar, * afc_pct_label;
static lv_obj_t * afc_buttons[4];

static void afc_set_text(lv_obj_t * label, const char * text)
{
    if(label != NULL && strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static lv_obj_t * afc_box(lv_obj_t * parent, int x, int y, int w, int h,
                         uint32_t color)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

static lv_obj_t * afc_text(lv_obj_t * parent, const char * text,
                          const lv_font_t * font, uint32_t color,
                          int x, int y, int width)
{
    lv_obj_t * label = make_label(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    return label;
}

static lv_obj_t * afc_bar(lv_obj_t * parent, int x, int y, int w, int h)
{
    lv_obj_t * bar = lv_bar_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 1000);
    return bar;
}

static void afc_sync_action(void * user)
{
    int action = (int)(intptr_t)user;
    if(action < 0 || action >= 4 || afc_buttons[action] == NULL ||
       lv_obj_has_state(afc_buttons[action], LV_STATE_DISABLED)) return;
    if(g_afc_action_cb != NULL) g_afc_action_cb(action);
}

static lv_obj_t * afc_sync_button(lv_obj_t * parent, const char * text,
                                 int action, int x, int y, int w)
{
    lv_obj_t * button = afc_box(parent, x, y, w, 56, COL_PANEL_ALT);
    lv_obj_set_style_border_color(button, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t * label = make_label(button, text, FONT_BODY, COL_TEXT);
    lv_obj_center(label);
    theme_button(button);
    focus_add_clickable(button, afc_sync_action, NULL,
                        (void *)(intptr_t)action, false);
    afc_buttons[action] = button;
    return button;
}

static void afc_button_enabled(int action, bool enabled)
{
    if(afc_buttons[action] == NULL) return;
    if(enabled) lv_obj_remove_state(afc_buttons[action], LV_STATE_DISABLED);
    else lv_obj_add_state(afc_buttons[action], LV_STATE_DISABLED);
}

static void afc_sync_update(void)
{
    ui_afc_status_t st = {0};
    if(g_afc_status_cb != NULL) g_afc_status_cb(&st);
    char text[160], used[24], total[24];
    bool busy = st.job_active || st.pair_busy;
    afc_button_enabled(UI_AFC_ACTION_PAIR,
                       st.available && st.usb_connected && !busy);
    afc_button_enabled(UI_AFC_ACTION_SYNC,
                       st.connected && st.sd_ready && !busy);
    afc_button_enabled(UI_AFC_ACTION_IPOD_MODE, !busy);

    afc_set_text(afc_connection, st.connected ? "Connected" :
                 st.pair_busy ? "Pairing..." :
                 st.pair_trusted ? "Connecting..." :
                 st.usb_connected ? "Not paired" : "Disconnected");
    afc_set_text(afc_trust_hint, st.connected ? "" :
                 !st.usb_connected ? "Connect to USB-A." :
                 st.pair_trusted ? "" :
                 st.pair_status[0] ? st.pair_status :
                 "Unlock the device, then select Pair / Trust.");
    settings_format_bytes(st.sd_used_bytes, used, sizeof(used));
    settings_format_bytes(st.sd_total_bytes, total, sizeof(total));
    snprintf(text, sizeof(text), "%s of %s used", used, total);
    afc_set_text(afc_sd_text, st.sd_ready ? text : "Insert an SD card to sync.");
    lv_bar_set_value(afc_sd_bar, st.sd_total_bytes ?
        (int)(st.sd_used_bytes * 1000 / st.sd_total_bytes) : 0, LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%d", st.library_albums);
    afc_set_text(afc_metrics[0], text);
    snprintf(text, sizeof(text), "%d", st.library_tracks);
    afc_set_text(afc_metrics[1], text);
    snprintf(text, sizeof(text), "%d", st.thumb_count);
    afc_set_text(afc_metrics[2], text);
    snprintf(text, sizeof(text), "Last library import  %s", st.library_updated);
    afc_set_text(afc_library_updated, st.library_db_bytes ? text : "No library imported yet");

    const char * title = "Ready to sync";
    const char * detail = "Existing covers are reused. Only missing artwork is downloaded.";
    if(!st.connected) {
        title = st.pair_busy ? "Waiting for trust" :
                st.pair_trusted ? "Connecting device" :
                st.usb_connected ? "Pair to sync" : "Waiting for device";
        detail = "";
    }
    else if(!st.sd_ready) {
        title = "An SD card is needed";
        detail = "Insert your card to store the library and album covers.";
    }
    if(st.job_active) {
        title = st.sync_phase == 1 ? "Checking the library" :
                st.sync_phase == 2 ? "Copying album artwork" :
                st.sync_phase == 3 ? "Preparing thumbnails" : "Preparing sync";
        if(st.sync_phase == 1 && st.job_kind == 2) title = "Copying the music database";
        snprintf(text, sizeof(text), "%d of %d processed   /   %d failed",
                 st.sync_attempted, st.sync_total, st.sync_failed);
        detail = st.sync_total ? text : "Keep your iPod connected while sync runs.";
    }
    else if(st.job_finished) {
        title = st.job_failed ? "Sync finished with errors" : "Sync complete";
        detail = st.job_failed ? st.status : "Your album covers are ready. Return to the player when you like.";
    }
    afc_set_text(afc_progress_title, title);
    afc_set_text(afc_progress_detail, detail);
    afc_set_text(afc_progress_album, st.job_active ? st.sync_album : "");
    int percent = st.sync_total > 0 ?
        (int)((int64_t)st.sync_attempted * 100 / st.sync_total) : 0;
    if(percent > 100) percent = 100;
    if(percent < 0) percent = 0;
    if(!st.job_active && !st.job_finished) percent = 0;
    if(st.job_finished && !st.job_failed) percent = 100;
    lv_bar_set_value(afc_progress_bar, percent * 10, LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%d%%", percent);
    afc_set_text(afc_pct_label, st.job_active || st.job_finished ? text : "");
}

static void afc_sync_tick_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    if(ui_apply_theme()) return;
    afc_sync_update();
}

static void ui_afc_sync_build(lv_obj_t * screen)
{
    memset(afc_buttons, 0, sizeof(afc_buttons));
    lv_obj_t * root = afc_box(screen, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
    afc_text(root, "Library Sync", FONT_H1, COL_TEXT, 32, 28, 700);
    lv_obj_t * badge = afc_box(root, 1128, 36, 120, 36, COL_PANEL_ALT);
    lv_obj_t * label = make_label(badge, "AFC MODE", FONT_CAPTION, COL_MUTED);
    lv_obj_center(label);

    lv_obj_t * device = afc_box(root, 32, 104, 328, 508, COL_PANEL);
    afc_text(device, ICON_PHONE, &bootstrap_icons_40, COL_ACCENT, 24, 24, 48);
    afc_text(device, "DEVICE", FONT_CAPTION, COL_MUTED, 86, 33, 218);
    afc_connection = afc_text(device, "Disconnected", FONT_H2,
                              COL_TEXT, 24, 88, 280);
    afc_trust_hint = afc_text(device, "", FONT_CAPTION, COL_MUTED, 24, 136, 280);
    lv_obj_set_height(afc_trust_hint, 72);
    lv_label_set_long_mode(afc_trust_hint, LV_LABEL_LONG_DOT);
    afc_sync_button(device, "Pair / Trust", UI_AFC_ACTION_PAIR, 24, 224, 280);

    afc_box(device, 24, 312, 280, 1, COL_BORDER);
    afc_text(device, ICON_SD_CARD, &bootstrap_icons_24, COL_MUTED, 24, 344, 28);
    afc_text(device, "SD CARD", FONT_CAPTION, COL_MUTED, 64, 348, 240);
    afc_sd_text = afc_text(device, "", FONT_CAPTION, COL_MUTED, 24, 392, 280);
    afc_sd_bar = afc_bar(device, 24, 442, 280, 4);

    lv_obj_t * library = afc_box(root, 384, 104, 864, 180, COL_PANEL);
    afc_text(library, "LIBRARY", FONT_CAPTION, COL_MUTED, 24, 20, 300);
    const char * metrics[] = {"Albums", "Tracks", "Cached covers"};
    for(int i = 0; i < 3; i++) {
        afc_metrics[i] = afc_text(library, "0", FONT_CLOCK_LG,
                                 COL_TEXT, 24 + i * 280, 56, 256);
        afc_text(library, metrics[i], FONT_CAPTION, COL_MUTED, 24 + i * 280, 100, 256);
    }
    afc_library_updated = afc_text(library, "", FONT_CAPTION, COL_MUTED, 24, 144, 816);

    lv_obj_t * sync = afc_box(root, 384, 304, 864, 308, COL_PANEL);
    afc_progress_title = afc_text(sync, "Ready to sync", FONT_H2, COL_TEXT, 24, 20, 718);
    afc_pct_label = afc_text(sync, "", FONT_CLOCK_12, COL_ACCENT, 748, 22, 92);
    lv_obj_set_style_text_align(afc_pct_label, LV_TEXT_ALIGN_RIGHT, 0);
    afc_progress_detail = afc_text(sync, "", FONT_CAPTION, COL_MUTED, 24, 62, 816);
    afc_progress_bar = afc_bar(sync, 24, 112, 816, 6);
    afc_progress_album = afc_text(sync, "", FONT_CAPTION, COL_MUTED, 24, 136, 816);
    lv_label_set_long_mode(afc_progress_album, LV_LABEL_LONG_DOT);
    afc_sync_button(sync, "Sync", UI_AFC_ACTION_SYNC, 24, 192, 816);
    afc_text(sync, "Refresh library database and missing covers", FONT_QUEUE_SUB,
             COL_MUTED, 24, 260, 816);

    afc_sync_button(root, "Exit", UI_AFC_ACTION_IPOD_MODE, 32, 642, 328);
    afc_sync_update();
    focus_select(0);
}

void ui_handle_key_repeat(ui_key_t key, int count)
{
    if(count <= 0) return;
    if(count == 1) {
        ui_handle_key(key);
        return;
    }

    if(!g_afc_sync_mode && !g_ctx_menu_open && !g_page_loading &&
       (key == UI_KEY_UP || key == UI_KEY_DOWN)) {
        const int delta = key == UI_KEY_UP ? -count : count;

        /* The time editor uses Up/Down as value adjustment, not focus
         * movement.  Preserve one adjust per detent. */
        if(g_current_screen == SCR_SETTINGS && settings_time_editing) {
            for(int i = 0; i < count; i++) ui_handle_key(key);
            return;
        }

        if(g_current_screen == SCR_SONGS) {
            songs_select(songs_selected + delta);
            return;
        }
        if(g_current_screen == SCR_ALBUMS && !albums_detail_open) {
            albums_select(albums_selected + delta);
            return;
        }
        if(g_current_screen == SCR_ARTISTS && !artists_detail_open) {
            artists_select(artists_selected + delta);
            return;
        }
        /* Settings and other focus pages: one move to the final position. */
        focus_move(delta);
        return;
    }

    for(int i = 0; i < count; i++) {
        ui_handle_key(key);
    }
}

/* Marquee target for the current page: the selected row's name (or the
 * detail title while a detail pane is open).  NULL when none applies. */
static lv_obj_t * marquee_target_label(void)
{
    switch(g_current_screen) {
        case SCR_ALBUMS:
            if(albums_detail_open) return albums_detail_title;
            for(int slot = 0; slot < ALBUMS_POOL; slot++) {
                if(albums_row_indices[slot] == albums_selected &&
                   !lv_obj_has_flag(albums_rows[slot], LV_OBJ_FLAG_HIDDEN)) {
                    return albums_row_titles[slot];
                }
            }
            return NULL;
        case SCR_ARTISTS:
            if(artists_detail_open) return artists_detail_name;
            for(int slot = 0; slot < ARTISTS_POOL; slot++) {
                if(artists_row_indices[slot] == artists_selected &&
                   !lv_obj_has_flag(artists_rows[slot], LV_OBJ_FLAG_HIDDEN)) {
                    return artists_names[slot];
                }
            }
            return NULL;
        case SCR_PLAYLISTS:
            if(playlists_detail_open) return playlists_detail_title;
            if(playlists_selected >= 0 && playlists_selected < 64) {
                return playlists_name_labels[playlists_selected];
            }
            return NULL;
        case SCR_SONGS:
            for(int slot = 0; slot < SONGS_POOL; slot++) {
                if(songs_row_indices[slot] == songs_selected &&
                   !lv_obj_has_flag(songs_rows[slot], LV_OBJ_FLAG_HIDDEN)) {
                    return songs_titles[slot];
                }
            }
            return NULL;
        default:
            return NULL;
    }
}

static lv_obj_t * g_marquee_key_label;
static int g_marquee_key_screen = -1;
static int g_marquee_key_index = -1;
static bool g_marquee_key_detail;
static char g_marquee_key_text[96];

/* Restarts the marquee whenever the selection (or its text) changes. */
static void ui_marquee_tick(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    if(g_page_loading) return;
    lv_obj_t * target = marquee_target_label();
    int index = g_current_screen == SCR_ALBUMS ? albums_selected
              : g_current_screen == SCR_ARTISTS ? artists_selected
              : g_current_screen == SCR_PLAYLISTS ? playlists_selected
              : g_current_screen == SCR_SONGS ? songs_selected : -1;
    bool detail = g_current_screen == SCR_ALBUMS ? albums_detail_open
                : g_current_screen == SCR_ARTISTS ? artists_detail_open
                : g_current_screen == SCR_PLAYLISTS ? playlists_detail_open
                : false;
    const char * text = target != NULL ? lv_label_get_text(target) : "";

    if(target == g_marquee_key_label && g_current_screen == g_marquee_key_screen &&
       index == g_marquee_key_index && detail == g_marquee_key_detail &&
       strncmp(text, g_marquee_key_text, sizeof(g_marquee_key_text)) == 0) {
        return;
    }
    g_marquee_key_label = target;
    g_marquee_key_screen = (int)g_current_screen;
    g_marquee_key_index = index;
    g_marquee_key_detail = detail;
    snprintf(g_marquee_key_text, sizeof(g_marquee_key_text), "%s", text);
    ui_marquee_start(target);
}

/* Apply on the LVGL timer thread, after input callbacks have unwound. Config
 * publication from the web task only changes the requested ID. Rebuild the
 * widgets once; retain library/art caches, current page, selection and player. */
static bool ui_apply_theme(void)
{
    const ui_theme_t * next = ui_theme_get(g_system_config.theme);
    if(next == g_theme) return false;
    if(g_ctx_menu_open) context_menu_close();
    int saved_focus = g_focus_index;
    g_theme = next;

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_TEXT), 0);
    if(g_afc_sync_mode) {
        focus_clear();
        lv_obj_clean(screen);
        ui_afc_sync_build(screen);
    }
    else {
        lv_obj_delete(g_menubar);
        lv_obj_delete(g_mini);
        memset(g_menu_items, 0, sizeof(g_menu_items));
        memset(g_menu_icons, 0, sizeof(g_menu_icons));
        build_menubar(screen);
        build_mini_now(g_content);
        /* Force title, art and time into the new mini-player widgets even
         * when playback is paused and its revision has not changed. */
        g_mini_track_id = UINT64_MAX;
        g_mini_art_rev = UINT32_MAX;
        g_mini_rev = UINT32_MAX;
        g_mini_elapsed = UINT32_MAX;
        ui_menubar_apply_side();
        lv_obj_set_style_bg_color(g_page, lv_color_hex(COL_MAIN_BG), 0);
        ui_layout_page_area();
        ui_rebuild_page();
        update_clock();
    }
    if(saved_focus >= 0 && saved_focus < g_focus_count) focus_select(saved_focus);
    return true;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */
void ui_init(void)
{
    player_init();
    player_sort_albums(g_sort_field, g_sort_desc);
    g_device_available = player_device_available();
    /* The language font is required; the later whole-library preload is optional. */
    fonts_init();
    /* main binds the thumbnail pack once, after thumb_cache_open(). Starting
     * its background preload here too can allocate two whole-library copies. */
    thumbs_init();

    lv_display_add_event_cb(lv_display_get_default(), ui_tab_refresh_cb,
                             LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(lv_display_get_default(), np_display_refresh_cb,
                             LV_EVENT_REFR_READY, NULL);
    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_TEXT), 0);

    if(g_afc_sync_mode) {
        ui_afc_sync_build(screen);
        lv_timer_create(afc_sync_tick_cb, 500, NULL);
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

    lv_timer_create(ui_tick_cb, 500, NULL);
    lv_timer_create(ui_marquee_tick, 200, NULL);
    lv_timer_create(settings_time_blink_timer_cb, 250, NULL);
}
