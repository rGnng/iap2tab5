/* Exercise real LVGL pointer hit testing and scrolling against the production
 * UI and mock library. No window, display server, or hardware is required. */
#include "lvgl.h"
#include "player.h"
#include "ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_indev_t * touch;
static lv_point_t point;
static lv_indev_state_t pressed = LV_INDEV_STATE_RELEASED;
static uint8_t draw_buffer[1280 * 40 * 4];

static void check(bool condition, const char * message)
{
    if(!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void flush(lv_display_t * display, const lv_area_t * area, uint8_t * pixels)
{
    LV_UNUSED(area);
    LV_UNUSED(pixels);
    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t * indev, lv_indev_data_t * data)
{
    LV_UNUSED(indev);
    data->point = point;
    data->state = pressed;
}

static void pump(int ms)
{
    for(int i = 0; i < ms; i += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static void pointer(int x, int y, bool down)
{
    point = (lv_point_t){x, y};
    pressed = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(touch);
    pump(20);
}

static lv_obj_t * find_label(lv_obj_t * parent, const char * text)
{
    if(lv_obj_check_type(parent, &lv_label_class) &&
       strcmp(lv_label_get_text(parent), text) == 0) return parent;
    for(uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t * found = find_label(lv_obj_get_child(parent, i), text);
        if(found) return found;
    }
    return NULL;
}

static lv_obj_t * album_list(void)
{
    lv_obj_t * title = find_label(lv_screen_active(), "Albums");
    check(title != NULL, "Albums page must be visible");
    lv_obj_t * content = lv_obj_get_parent(lv_obj_get_parent(title));
    lv_obj_t * list = lv_obj_get_child(content, 1);
    check(list != NULL && lv_obj_is_scrollable(list), "Albums list must be scrollable");
    return list;
}

static lv_obj_t * middle_row(lv_obj_t * list)
{
    lv_area_t viewport;
    lv_obj_get_coords(list, &viewport);
    int middle = (viewport.y1 + viewport.y2) / 2;
    lv_obj_t * best = NULL;
    int distance = 10000;
    /* Skip the extent spacer and Shuffle All. Remaining children are pooled rows. */
    for(uint32_t i = 2; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t * row = lv_obj_get_child(list, i);
        if(lv_obj_is_hidden(row)) continue;
        lv_area_t area;
        lv_obj_get_coords(row, &area);
        if(area.y1 < viewport.y1 || area.y2 > viewport.y2) continue;
        int d = abs((area.y1 + area.y2) / 2 - middle);
        if(d < distance) { distance = d; best = row; }
    }
    check(best != NULL, "A fully visible album row must exist");
    return best;
}

static lv_point_t centre(lv_obj_t * obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
}

static void drag(const char * name, int region, bool down)
{
    lv_obj_t * list = album_list();
    int stride = lv_obj_get_height(lv_obj_get_child(list, 1));
    lv_obj_scroll_to_y(list, down ? stride * 8 : stride, LV_ANIM_OFF);
    pump(200);
    int before = lv_obj_get_scroll_top(list);
    lv_obj_t * row = middle_row(list);
    lv_obj_t * text = lv_obj_get_child(row, 1);
    lv_point_t start = region == 0 ? centre(lv_obj_get_child(row, 0)) :
                       region == 1 ? centre(lv_obj_get_child(text, 0)) :
                       region == 2 ? centre(lv_obj_get_child(text, 1)) : centre(row);
    if(region == 3) {
        lv_area_t area;
        lv_obj_get_coords(row, &area);
        start.x = area.x1 + 4;
    }
    player_snapshot_t previous, current;
    player_get_snapshot(&previous);
    pointer(start.x, start.y, true);
    for(int step = 1; step <= 8; step++) {
        pointer(start.x, start.y + (down ? 1 : -1) * 20 * step, true);
    }
    int moved = lv_obj_get_scroll_top(list) - before;
    printf("%s %s: scrolled %d px\n", name, down ? "down" : "up", moved);
    fflush(stdout);
    check(down ? moved < -stride : moved > stride, "Pointer drag must scroll the album list");
    for(uint32_t i=1;i<lv_obj_get_child_count(list);i++) {
        check(!lv_obj_has_state(lv_obj_get_child(list,i),LV_STATE_USER_2),
              "Scrolling must cancel the row press highlight before the finger lifts");
    }
    pump(100); /* Lift after stopping, so inertia does not make the test nondeterministic. */
    pointer(point.x, point.y, false);
    pump(200);
    check(lv_obj_is_valid(list), "Dragging must not open an album or rebuild the page");
    player_get_snapshot(&current);
    check(current.track.id == previous.track.id, "Dragging must not start playback");
}

static int selection_marks(lv_obj_t * list)
{
    int focused = 0;
    for(uint32_t i = 2; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t * row = lv_obj_get_child(list, i);
        if(!lv_obj_is_hidden(row) && lv_obj_get_style_border_width(row, 0) == 3) focused++;
    }
    return focused;
}

static void tap_title(void)
{
    ui_handle_key(UI_KEY_2);
    pump(300);
    check(selection_marks(album_list()) == 1, "Album list starts with a visible selection");
    lv_obj_t * row = middle_row(album_list());
    lv_obj_t * text = lv_obj_get_child(row, 1);
    lv_point_t p = centre(lv_obj_get_child(text, 0));
    pointer(p.x, p.y, true);
    pointer(p.x, p.y, false);
    pump(300);
    check(selection_marks(album_list()) == 0, "A tap on an album title must open its detail view");
    ui_handle_key(UI_KEY_BACK);
    pump(300);
    check(selection_marks(album_list()) == 1, "Back must return focus to the album list");
    puts("tap: album opens and Back returns");
}

static lv_obj_t * menu_tile(int slot)
{
    lv_obj_t *menu = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *tiles = lv_obj_get_child(menu, 1);
    check(lv_obj_get_child_count(tiles) == 6, "Menubar must contain six navigation tiles");
    return lv_obj_get_child(tiles, slot);
}

static void expect_active(int slot)
{
    check(lv_obj_get_style_bg_opa(menu_tile(slot), 0) == LV_OPA_COVER,
          "Expected navigation tile must remain active");
}

static void tap_tile(int slot)
{
    lv_point_t p = centre(menu_tile(slot));
    pointer(p.x, p.y, true); pointer(p.x, p.y, false); pump(300);
}

static void blocked_navigation(void)
{
    check(!ui_library_available(), "Library must be unavailable with a prerequisite missing");
    expect_active(0);
    for(int slot = 1; slot <= 4; slot++) {
        check(lv_obj_has_state(menu_tile(slot), LV_STATE_DISABLED), "Library tiles must be disabled");
        tap_tile(slot); expect_active(0);
        ui_handle_key((ui_key_t)(UI_KEY_1 + slot)); pump(200); expect_active(0);
        ui_select_screen(slot); pump(200); expect_active(0);
    }
    ui_handle_key(UI_KEY_SCREEN_NEXT); pump(300); expect_active(5);
    ui_handle_key(UI_KEY_SCREEN_PREV); pump(300); expect_active(0);
    tap_tile(5); expect_active(5);
    tap_tile(0); expect_active(0);
}

static void device_gating(void)
{
    ui_set_device_state(true, true, true); pump(600);
    for(int screen = 1; screen <= 4; screen++) {
        check(ui_library_available(), "Connected iPod and SD must enable browsing");
        check(!lv_obj_has_state(menu_tile(screen), LV_STATE_DISABLED), "Library tile must re-enable");
        ui_select_screen(screen); pump(300); expect_active(screen);
        if(screen == 1) ui_handle_key(UI_KEY_CONTEXT);
        ui_set_device_state(true, false, true); pump(600); expect_active(0);
        blocked_navigation();
        ui_set_device_state(true, true, true); pump(600);
    }

    /* Input and deferred loads must respect a disconnect before the 500 ms
     * UI tick has had a chance to process its dirty-state flag. */
    ui_select_screen(1); pump(300); ui_set_device_state(true, false, true);
    ui_handle_key(UI_KEY_ENTER); expect_active(0);
    ui_set_device_state(true, true, true); pump(600);
    ui_select_screen(1); pump(300); ui_set_device_state(true, false, true);
    ui_handle_key_repeat(UI_KEY_DOWN, 4); expect_active(0);
    ui_set_device_state(true, true, true); pump(600);
    ui_select_screen(1); ui_set_device_state(true, false, true); pump(100); expect_active(0);

    /* A removed SD card stays unavailable after insertion until reboot,
     * because the cached library and open files belong to the old mount. */
    ui_set_device_state(true, true, true); pump(600);
    ui_select_screen(2); pump(300);
    ui_set_device_state(true, true, false); pump(600); blocked_navigation();
    ui_set_device_state(true, false, false); pump(600); blocked_navigation();
    ui_set_device_state(true, true, true); pump(600); blocked_navigation();
    puts("PASS: missing-device navigation, disconnect fallback and SD reboot gate");
}

int main(int argc, char ** argv)
{
    lv_init();
    lv_display_t * display = lv_display_create(1280, 720);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch, display);
    lv_indev_set_read_cb(touch, read_touch);
    ui_system_config_t settings = {.theme = argc > 1 ? atoi(argv[1]) : 0,
        .touch_enabled = 1, .volume = 65, .brightness = 70, .menubar_left = 1,
        .sort_desc = 1, .wifi_status = "WiFi off", .wifi_ip = "--"};
    ui_apply_saved_settings(&settings);
    ui_init();
    ui_attach_touch(touch);
    ui_select_screen(1);
    pump(600);
    const char * names[] = {"artwork", "title", "artist", "padding"};
    for(int region = 0; region < 4; region++) {
        drag(names[region], region, false);
        drag(names[region], region, true);
    }
    tap_title();
    device_gating();
    puts("PASS: album touch scrolling and tapping");
    return 0;
}
