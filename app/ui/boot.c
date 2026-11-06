#include "boot.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 0 = Inter (matches the main UI's tabular numbers), 1 = VT323 pixel font. */
#define BOOT_USE_PIXEL_FONT 0

#if BOOT_USE_PIXEL_FONT
LV_FONT_DECLARE(boot_pixel_32);
#define BOOT_FONT (&boot_pixel_32)
#else
#include "fonts/inter_fonts.h"
#define BOOT_FONT (&inter_tabular_28)
#endif

/* -------------------------------------------------------------------------
 * Layout
 *
 * The reference is a 1280x720 arcade boot screen: pure black, one terminal
 * font, a yellow title, green OK markers and white pending dashes. Inter
 * tabular is the default (same face as the main UI's numbers); VT323 is kept
 * as an optional pixel-font variant. The values below reproduce that line
 * grid at the Tab5 resolution.
 * ------------------------------------------------------------------------- */
#define BOOT_W              1280
#define BOOT_H              720
#define BOOT_X              88
#define BOOT_VERSION_Y      62
#define BOOT_TITLE_Y        92
#define BOOT_TITLE_X        (BOOT_X + 26)
#define BOOT_FIRST_ITEM_Y   130
#define BOOT_ITEM_X         BOOT_TITLE_X
#if BOOT_USE_PIXEL_FONT
#define BOOT_ITEM_STEP      30
#define BOOT_STATUS_X       (BOOT_ITEM_X + 230)
#define BOOT_STATUS_TEXT_X  (BOOT_STATUS_X + 13)
#else
#define BOOT_ITEM_STEP      32
#define BOOT_STATUS_X       (BOOT_ITEM_X + 266)
#define BOOT_STATUS_TEXT_X  (BOOT_STATUS_X + 10)
#endif
#define BOOT_BOTTOM_Y       570
#define BOOT_BOTTOM_STEP    30

#define BOOT_STAGE1_MS      3500u
/* One extra frame so the 60 fps counter can actually show 210. */
#define BOOT_STAGE1_END_MS  (BOOT_STAGE1_MS + 16u)
#define BOOT_STAGE2_END_MS  (BOOT_STAGE1_END_MS + 900u)
#define BOOT_STAGE3_END_MS  (BOOT_STAGE2_END_MS + 1400u)
#define BOOT_TOTAL_MS       BOOT_STAGE3_END_MS

#define BOOT_FPS_MIN        59.9990f
#define BOOT_FPS_MAX        60.0000f
#define BOOT_FPS_DEFAULT    59.9998f

/* -------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */
#define BOOT_COL_BG         0x000000
#define BOOT_COL_TEXT       0xE8E8E8
#define BOOT_COL_DIM        0x8C8C8C
#define BOOT_COL_TITLE      0xF2D64B
#define BOOT_COL_OK         0x54E06B
#define BOOT_COL_CHECK      0xD6F4FF
#define BOOT_COL_PENDING    0xFFFFFF
#define BOOT_COL_WARN       0xF5C542
#define BOOT_COL_FAIL       0xFF4D4D

/* -------------------------------------------------------------------------
 * POST items
 * ------------------------------------------------------------------------- */
typedef struct {
    const char * name;
    uint32_t start_ms;
    uint32_t duration_ms;
    boot_state_t result;
} boot_item_def_t;

/* Durations are deliberately short.  They describe "how long the indicator
 * is allowed to spin", not how long the real hardware takes. */
static boot_item_def_t s_items[BOOT_ITEM_COUNT] = {
    [BOOT_ITEM_USB_INIT]   = { "USB INIT",          60,   80, BOOT_STATE_OK },
    [BOOT_ITEM_SD_CARD]    = { "SD CARD",          160,  180, BOOT_STATE_OK },
    [BOOT_ITEM_TOUCH]      = { "TOUCH",            360,  130, BOOT_STATE_OK },
    [BOOT_ITEM_DISPLAY]    = { "DISPLAY",          510,  140, BOOT_STATE_OK },
    [BOOT_ITEM_PSRAM]      = { "PSRAM",            670,  100, BOOT_STATE_OK },
    [BOOT_ITEM_FLASH]      = { "FLASH",            790,   90, BOOT_STATE_OK },
    [BOOT_ITEM_USB_HOST]   = { "USB HOST",         900,  170, BOOT_STATE_OK },
    [BOOT_ITEM_IAP2_CERT]  = { "IAP2 CERTIFICATE", 1090,  180, BOOT_STATE_OK },
    [BOOT_ITEM_WIFI_C6]    = { "WI-FI C6",        1290,  160, BOOT_STATE_OK },
    [BOOT_ITEM_NETWORK]    = { "NETWORK",         1470,  620, BOOT_STATE_OK },
};

/* -------------------------------------------------------------------------
 * Widgets / state
 * ------------------------------------------------------------------------- */
static lv_obj_t * s_root;
static lv_obj_t * s_stage[3];
static lv_obj_t * s_item_status[BOOT_ITEM_COUNT];
static lv_obj_t * s_elapsed;
static lv_obj_t * s_fps_label;

static lv_timer_t * s_timer;
static uint32_t s_start_tick;
static int s_visible_stage = -1;
static bool s_active;
static bool s_frozen;
static void (*s_done_cb)(void);

static char s_last_status[BOOT_ITEM_COUNT][24];
static char s_last_elapsed[32];
static char s_last_fps[24];

static float s_fps = BOOT_FPS_DEFAULT;
static float s_fps_override;

static char s_version[24] = "IAP2:2026010100";
static char s_compiled[64] = "COMPILED AT JAN 01 2026 00:00:00";

/* -------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */
static int month_number(const char * name)
{
    static const char * months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for(int i = 0; i < 12; i++) {
        if(strncmp(name, months[i], 3) == 0) return i + 1;
    }
    return 1;
}

static void build_stamps(void)
{
    char month[4] = { 0 };
    int day = 1;
    int year = 2026;

    if(sscanf(__DATE__, "%3s %d %d", month, &day, &year) == 3) {
        snprintf(s_version, sizeof(s_version), "IAP2:%04d%02d%02d00",
                 year, month_number(month), day);
        snprintf(s_compiled, sizeof(s_compiled), "COMPILED AT %s %02d %04d %s",
                 month, day, year, __TIME__);
    }
}

static lv_obj_t * make_label(lv_obj_t * parent, int x, int y,
                             const char * text, uint32_t color)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, BOOT_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t * make_stage(lv_obj_t * parent)
{
    lv_obj_t * stage = lv_obj_create(parent);
    lv_obj_set_size(stage, BOOT_W, BOOT_H);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_radius(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_set_scrollable(stage, false);
    lv_obj_set_hidden(stage, true);
    return stage;
}

static void set_label_text_if_changed(lv_obj_t * label, char * cache,
                                      size_t cache_size, const char * text)
{
    if(strncmp(cache, text, cache_size) == 0) return;
    snprintf(cache, cache_size, "%s", text);
    lv_label_set_text(label, text);
}

/* -------------------------------------------------------------------------
 * Widget construction
 * ------------------------------------------------------------------------- */
static void build_post_stage(lv_obj_t * stage)
{
    make_label(stage, BOOT_X, BOOT_VERSION_Y, s_version, BOOT_COL_TEXT);
    make_label(stage, BOOT_TITLE_X, BOOT_TITLE_Y,
               "IAP2TAB5 IPOD CONTROL 3000 STARTUP", BOOT_COL_TITLE);

    for(int i = 0; i < BOOT_ITEM_COUNT; i++) {
        const int y = BOOT_FIRST_ITEM_Y + i * BOOT_ITEM_STEP;
        make_label(stage, BOOT_ITEM_X, y, s_items[i].name, BOOT_COL_TEXT);
        /* The colon is fixed white; only the value after it changes color. */
        make_label(stage, BOOT_STATUS_X, y, ":", BOOT_COL_TEXT);
        s_item_status[i] = make_label(stage, BOOT_STATUS_TEXT_X, y, "---",
                                      BOOT_COL_PENDING);
    }

    s_elapsed = make_label(stage, BOOT_X, BOOT_BOTTOM_Y, "ELAPSED TIME = 0",
                           BOOT_COL_TEXT);
    make_label(stage, BOOT_X, BOOT_BOTTOM_Y + BOOT_BOTTOM_STEP,
               "PLEASE WAIT FOR BOOT", BOOT_COL_OK);
    make_label(stage, BOOT_X, BOOT_BOTTOM_Y + BOOT_BOTTOM_STEP * 2,
               s_compiled, BOOT_COL_TEXT);

    for(int i = 0; i < BOOT_ITEM_COUNT; i++) {
        s_last_status[i][0] = '\0';
    }
    s_last_elapsed[0] = '\0';
}

static void build_app_stage(lv_obj_t * stage)
{
    make_label(stage, BOOT_X, BOOT_VERSION_Y, s_version, BOOT_COL_TEXT);
    make_label(stage, BOOT_TITLE_X, 104, "Initialize App", BOOT_COL_TEXT);
}

static void build_display_stage(lv_obj_t * stage)
{
    /* This screen replaces the version line: the title moves up into the
     * first row and the rest of the block follows it. */
    make_label(stage, BOOT_X, BOOT_VERSION_Y, "DISPLAY CHECKING",
               BOOT_COL_TEXT);
    s_fps_label = make_label(stage, BOOT_TITLE_X, BOOT_VERSION_Y + 40,
                             "FPS = 59.9998", BOOT_COL_OK);
    make_label(stage, BOOT_TITLE_X, BOOT_VERSION_Y + 80,
               "PANEL 1280X720 @ 60HZ", BOOT_COL_DIM);
    s_last_fps[0] = '\0';
}

static void build_ui(void)
{
    /* Use the active screen rather than lv_layer_top(): sim_screenshot()
     * snapshots the active screen, so a top-layer overlay would be missing
     * from headless PNG captures. */
    s_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_root, BOOT_W, BOOT_H);
    lv_obj_move_foreground(s_root);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(BOOT_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);

    for(int i = 0; i < 3; i++) s_stage[i] = make_stage(s_root);
    build_post_stage(s_stage[0]);
    build_app_stage(s_stage[1]);
    build_display_stage(s_stage[2]);

    lv_obj_update_layout(s_root);

    s_visible_stage = -1;
    s_fps = BOOT_FPS_DEFAULT;
}

/* -------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */
static const char * status_text(boot_state_t state, uint32_t t, bool network,
                                char * buf, size_t buf_size)
{
    switch(state) {
        case BOOT_STATE_CHECKING: {
            if(network) {
                const size_t dots = ((t / 124u) % 5u);
                snprintf(buf, buf_size, "CHECKING");
                for(size_t i = 0; i < dots && 8 + i + 1 < buf_size; i++) {
                    buf[8 + i] = '.';
                    buf[9 + i] = '\0';
                }
            }
            else {
                snprintf(buf, buf_size, "CHECKING");
            }
            break;
        }
        case BOOT_STATE_OK:
            snprintf(buf, buf_size, "OK");
            break;
        case BOOT_STATE_WARN:
            snprintf(buf, buf_size, "WARN");
            break;
        case BOOT_STATE_FAIL:
            snprintf(buf, buf_size, "FAIL");
            break;
        case BOOT_STATE_PENDING:
        default:
            snprintf(buf, buf_size, "---");
            break;
    }
    return buf;
}

static uint32_t state_color(boot_state_t state)
{
    switch(state) {
        case BOOT_STATE_OK:     return BOOT_COL_OK;
        case BOOT_STATE_CHECKING: return BOOT_COL_CHECK;
        case BOOT_STATE_WARN:   return BOOT_COL_WARN;
        case BOOT_STATE_FAIL:   return BOOT_COL_FAIL;
        case BOOT_STATE_PENDING:
        default:                return BOOT_COL_PENDING;
    }
}

static void show_stage(int index)
{
    if(index == s_visible_stage) return;
    for(int i = 0; i < 3; i++) {
        if(i == index) lv_obj_set_hidden(s_stage[i], false);
        else lv_obj_set_hidden(s_stage[i], true);
    }
    s_visible_stage = index;
}

static void update_fps(bool frozen)
{
    if(frozen) {
        s_fps = BOOT_FPS_DEFAULT;
    }
    else if(s_fps_override > 0.0f) {
        s_fps = s_fps_override;
    }
    else {
        /* Rapid flicker in the last four decimals: 59.9990 .. 60.0000. */
        s_fps = BOOT_FPS_MIN + (float)(rand() % 11) * 0.0001f;
        if(s_fps > BOOT_FPS_MAX) s_fps = BOOT_FPS_MAX;
    }

    if(s_fps < 30.0f) {
        lv_obj_set_style_text_color(s_fps_label, lv_color_hex(BOOT_COL_FAIL), 0);
    }
    else if(s_fps < 55.0f) {
        lv_obj_set_style_text_color(s_fps_label, lv_color_hex(BOOT_COL_WARN), 0);
    }
    else {
        lv_obj_set_style_text_color(s_fps_label, lv_color_hex(BOOT_COL_OK), 0);
    }

    char text[24];
    snprintf(text, sizeof(text), "FPS = %.4f", (double)s_fps);
    set_label_text_if_changed(s_fps_label, s_last_fps, sizeof(s_last_fps),
                              text);
}

static void render_at(uint32_t t)
{
    if(!s_active) return;

    if(t < BOOT_STAGE1_END_MS) {
        show_stage(0);

        char status[24];
        for(int i = 0; i < BOOT_ITEM_COUNT; i++) {
            const boot_item_def_t * item = &s_items[i];
            const uint32_t end = item->start_ms + item->duration_ms;
            boot_state_t state;
            if(t < item->start_ms) state = BOOT_STATE_PENDING;
            else if(t < end) state = BOOT_STATE_CHECKING;
            else state = item->result;

            const uint32_t phase = (i == BOOT_ITEM_NETWORK &&
                                    state == BOOT_STATE_CHECKING)
                                       ? t - item->start_ms
                                       : t;
            status_text(state, phase, i == BOOT_ITEM_NETWORK, status,
                        sizeof(status));
            set_label_text_if_changed(s_item_status[i], s_last_status[i],
                                      sizeof(s_last_status[i]), status);
            lv_obj_set_style_text_color(s_item_status[i],
                                        lv_color_hex(state_color(state)), 0);
        }

        /* The reference screen counts frames, not milliseconds. We cannot
         * count real display frames during boot, so derive a 60 fps frame
         * counter from the monotonic millisecond tick:
         * 3500 ms -> 210 frames. */
        char elapsed[32];
        const uint32_t frames = (t * 60u) / 1000u;
        snprintf(elapsed, sizeof(elapsed), "ELAPSED TIME = %u",
                 (unsigned)frames);
        set_label_text_if_changed(s_elapsed, s_last_elapsed,
                                  sizeof(s_last_elapsed), elapsed);
    }
    else if(t < BOOT_STAGE2_END_MS) {
        show_stage(1);
    }
    else {
        show_stage(2);
        update_fps(s_frozen);
    }
}

/* -------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */
static void stop_ui(void)
{
    if(s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if(s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
        lv_obj_invalidate(lv_screen_active());
    }
    s_stage[0] = s_stage[1] = s_stage[2] = NULL;
    s_elapsed = NULL;
    s_fps_label = NULL;
    s_active = false;
    s_frozen = false;
    s_visible_stage = -1;
}

static void finish(void)
{
    void (*cb)(void) = s_done_cb;
    s_done_cb = NULL;
    stop_ui();
    if(cb != NULL) cb();
}

static void boot_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    if(!s_active || s_frozen) return;

    const uint32_t t = lv_tick_get() - s_start_tick;
    if(t >= BOOT_TOTAL_MS) {
        finish();
        return;
    }
    render_at(t);
}

void boot_ui_set_result(boot_item_t item, boot_state_t state)
{
    if(item < 0 || item >= BOOT_ITEM_COUNT) return;
    s_items[item].result = state;
}

void boot_ui_set_fps(float fps)
{
    s_fps_override = fps;
}

void boot_ui_start(void (*on_done)(void))
{
    if(s_active) stop_ui();

    build_stamps();
    s_done_cb = on_done;
    build_ui();

    s_active = true;
    s_frozen = false;
    s_start_tick = lv_tick_get();
    srand((unsigned)lv_tick_get());

    render_at(0);
    s_timer = lv_timer_create(boot_timer_cb, 16, NULL);
}

void boot_ui_freeze(uint32_t t_ms)
{
    if(s_active) stop_ui();

    build_stamps();
    s_done_cb = NULL;
    build_ui();

    s_active = true;
    s_frozen = true;
    s_start_tick = 0;

    if(t_ms >= BOOT_TOTAL_MS) t_ms = BOOT_TOTAL_MS - 1;
    render_at(t_ms);

}

bool boot_ui_active(void)
{
    return s_active;
}

void boot_ui_skip(void)
{
    if(!s_active) return;
    finish();
}

void boot_ui_stop(void)
{
    if(!s_active) return;
    stop_ui();
}
