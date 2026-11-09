/* Render the production clock/notification strip through real LVGL fades. */
#include "ble_remote.h"
#include <stdio.h>
#include <string.h>
static bool bluetooth_enabled;
static int paired_count;
static ble_remote_state_t bluetooth_state;
static bool test_ble_enabled(void) { return bluetooth_enabled; }
static int test_ble_paired_count(void) { return paired_count; }
static void test_ble_status(ble_remote_state_t *state, char *text, size_t size)
{
    if(state) *state = bluetooth_state;
    if(text && size) snprintf(text, size, "Test controller");
}
#define ble_remote_enabled test_ble_enabled
#define ble_remote_paired_count test_ble_paired_count
#define ble_remote_status test_ble_status
#include "../app/ui/ui.c"
#undef ble_remote_enabled
#undef ble_remote_paired_count
#undef ble_remote_status

static uint32_t frame[720][1280];
static uint8_t draw_buffer[1280 * 40 * 4];
static bool touch_down;
static lv_point_t touch_point;
static void check(bool ok, const char *message)
{ if(!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); } }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    for(int y = area->y1; y <= area->y2; y++)
        memcpy(&frame[y][area->x1], pixels + (y-area->y1)*lv_area_get_width(area)*4,
               lv_area_get_width(area)*4);
    lv_display_flush_ready(display);
}
static void pump(int ms)
{ for(int i=0; i<ms; i+=10) { lv_tick_inc(10); lv_timer_handler(); } }
static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{ LV_UNUSED(indev); data->point=touch_point; data->state=touch_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; }
static void wait_for(status_content_t content)
{
    for(int ms=0; ms<5000; ms+=10) {
        if(g_status_content==content && g_status_phase==STATUS_HOLD &&
           g_status_target==content) return;
        pump(10);
    }
    check(false, "Notification must reach its expected fully visible content");
}
static void stays_visible(status_content_t content, int ms, const char *message)
{
    for(int elapsed=0;elapsed<ms;elapsed+=10) {
        pump(10);
        check(g_status_content==content && g_status_target==content &&
              g_status_phase==STATUS_HOLD && lv_obj_get_style_opa(g_status_time,0)==255,
              message);
    }
}
static uint32_t icon_hash(void)
{
    lv_area_t bounds; lv_obj_get_coords(g_status_icon, &bounds);
    uint32_t hash=2166136261u;
    for(int y=bounds.y1; y<=bounds.y2; y++)
        for(int x=bounds.x1; x<=bounds.x2; x++) hash=(hash^frame[y][x])*16777619u;
    return hash;
}
static void save_strip(const char *name)
{
    const char *prefix=getenv("TAB5_NOTIFICATION_SNAPSHOTS");
    if(!prefix) return;
    char path[256]; snprintf(path,sizeof(path),"%s-%d-%s.ppm",prefix,g_theme->id,name);
    FILE *file=fopen(path,"wb"); check(file!=NULL,"Notification preview must be writable");
    lv_area_t bounds; lv_obj_get_coords(lv_obj_get_parent(g_status_time),&bounds);
    fprintf(file,"P6\n%ld %ld\n255\n",(long)lv_area_get_width(&bounds),(long)lv_area_get_height(&bounds));
    for(int y=bounds.y1;y<=bounds.y2;y++) for(int x=bounds.x1;x<=bounds.x2;x++) {
        uint32_t pixel=frame[y][x]; uint8_t rgb[]={pixel>>16,pixel>>8,pixel};
        fwrite(rgb,1,3,file);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_display_t *display=lv_display_create(1280,720);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    ui_system_config_t config={.theme=argc>1?atoi(argv[1]):0,.brightness=45,.menubar_left=1,.touch_enabled=1};
    ui_apply_saved_settings(&config); ui_init();
    ui_select_screen(SCR_ALBUMS); pump(600);
    lv_obj_t *area=lv_obj_get_parent(g_status_time);
    lv_area_t original; lv_obj_get_coords(area,&original);
    check(g_status_content==STATUS_CLOCK && lv_obj_is_visible(g_status_time) &&
          !lv_obj_is_visible(g_status_icon), "Idle strip must display only the time");
    save_strip("clock");
    bluetooth_enabled=true; bluetooth_state=BLE_REMOTE_SCANNING;
    status_area_update(); pump(600);
    check(status_area_latest(lv_tick_get())==STATUS_CLOCK,
          "Bluetooth discovery without a pairing must leave the time alone");

    paired_count=1; bluetooth_state=BLE_REMOTE_CONNECTED;
    status_area_update(); pump(110);
    check(g_status_content==STATUS_CLOCK &&
          lv_obj_get_style_opa(g_status_time,0)>0 && lv_obj_get_style_opa(g_status_time,0)<255,
          "The existing time must fade before the connected icon appears");
    wait_for(STATUS_BLUETOOTH_CONNECTED); pump(50);
    check(lv_image_get_src(g_status_icon)==&notification_bluetooth_connecting &&
          !lv_obj_is_visible(g_status_time) && lv_obj_is_visible(g_status_icon),
          "A connected controller must use the requested andrsrxn artwork");
    check(notification_bluetooth_connecting.header.w==32 && notification_bluetooth_x.header.w==32 &&
          notification_arrow_repeat.header.w==32,
          "Notification icons must use the smaller 32 px artwork");
    check(lv_color_eq(lv_obj_get_style_image_recolor(g_status_icon,0),
                     lv_obj_get_style_text_color(g_status_time,0)),
          "Notification ink must match the clock foreground in every theme");
    uint32_t connected_hash=icon_hash();
    save_strip("connected");
    pump(100);
    check(icon_hash()==connected_hash, "The connected icon must remain still");
    /* An icon must preserve the clock's real touch target and long hold. */
    lv_indev_t *touch=lv_indev_create(); lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch,display); lv_indev_set_read_cb(touch,read_touch);
    ui_attach_touch(touch);
    touch_point=(lv_point_t){(original.x1+original.x2)/2,(original.y1+original.y2)/2};
    touch_down=true; lv_indev_read(touch); pump(80);
    check(g_clock_hold_timer!=NULL && lv_indev_get_state(touch)==LV_INDEV_STATE_PRESSED,
          "The icon must pass real pointer input to its clock-area parent");
    check(lv_color_eq(lv_obj_get_style_image_recolor(g_status_icon,0),
                     lv_color_hex(g_theme->on_selection)),
          "Pressed notification icons must keep the clock's legible touch color");
    pump(2000); check(!ui_screen_is_on(), "Holding the notification area must turn off the screen");
    touch_down=false; lv_indev_read(touch); pump(50);
    ui_handle_key(UI_KEY_ENTER); pump(50);
    check(ui_screen_is_on(), "The first key must wake the screen normally");
    ui_attach_touch(NULL); lv_indev_delete(touch);
    wait_for(STATUS_CLOCK);
    check(lv_obj_is_visible(g_status_time) && !lv_obj_is_visible(g_status_icon),
          "A completed Bluetooth notification must return to the clock");
    stays_visible(STATUS_CLOCK,6000,
                  "An unchanged Bluetooth connection must not repeat its notification");

    bluetooth_state=BLE_REMOTE_CONNECTING; status_area_update();
    wait_for(STATUS_BLUETOOTH_DISCONNECTED); pump(50);
    check(lv_image_get_src(g_status_icon)==&notification_bluetooth_x &&
          icon_hash()!=connected_hash, "A lost connection must visibly use bluetooth-x");
    save_strip("disconnected");
    uint32_t since=g_status_events[STATUS_BLUETOOTH_DISCONNECTED].started;
    bluetooth_state=BLE_REMOTE_SCANNING; status_area_update(); pump(100);
    check(g_status_events[STATUS_BLUETOOTH_DISCONNECTED].started==since &&
          g_status_target==STATUS_BLUETOOTH_DISCONNECTED,
          "Reconnect scan/attempt changes must not keep restarting the notification");

    ui_set_library_syncing(true); status_area_update();
    wait_for(STATUS_LIBRARY_SYNC); pump(50);
    uint32_t sync_hash=icon_hash();
    save_strip("sync");
    pump(170);
    check(icon_hash()!=sync_hash && lv_image_get_rotation(g_status_icon)!=0,
          "The sync arrow must actually rotate in the rendered framebuffer");
    stays_visible(STATUS_LIBRARY_SYNC,6000,
                  "An ongoing sync must never cycle through the clock or an older Bluetooth event");

    bluetooth_state=BLE_REMOTE_CONNECTED; status_area_update();
    wait_for(STATUS_BLUETOOTH_CONNECTED);
    check(g_status_events[STATUS_LIBRARY_SYNC].active,
          "A new Bluetooth event must preserve the underlying ongoing sync");
    wait_for(STATUS_LIBRARY_SYNC);
    stays_visible(STATUS_LIBRARY_SYNC,5000,
                  "After Bluetooth finishes, the still-active sync must return and remain visible");

    bluetooth_state=BLE_REMOTE_CONNECTING; status_area_update();
    wait_for(STATUS_BLUETOOTH_DISCONNECTED);
    ui_set_library_syncing(false); status_area_update();
    wait_for(STATUS_CLOCK);
    stays_visible(STATUS_CLOCK,5000,
                  "A sync completed behind a Bluetooth event must never reappear");

    /* A newer sync can also preempt Bluetooth; its predecessor returns only
     * while that brief event is still active. */
    bluetooth_state=BLE_REMOTE_CONNECTED; status_area_update();
    wait_for(STATUS_BLUETOOTH_CONNECTED);
    ui_set_library_syncing(true); status_area_update(); wait_for(STATUS_LIBRARY_SYNC);
    ui_set_library_syncing(false); status_area_update(); wait_for(STATUS_BLUETOOTH_CONNECTED);
    wait_for(STATUS_CLOCK);

    paired_count=0; status_area_update(); pump(5000);
    check(g_status_content==STATUS_CLOCK && status_area_latest(lv_tick_get())==STATUS_CLOCK,
          "Completion and deleting the last pairing must remove stale notifications");
    paired_count=1; bluetooth_state=BLE_REMOTE_CONNECTING; status_area_update(); pump(90);
    bluetooth_enabled=false; status_area_update(); pump(700);
    check(g_status_content==STATUS_CLOCK && g_status_phase==STATUS_HOLD &&
          lv_obj_get_style_opa(g_status_time,0)==255,
          "Disabling Bluetooth midway through a fade must restore the clock");

    ui_set_library_syncing(true); status_area_update(); wait_for(STATUS_LIBRARY_SYNC);
    bluetooth_enabled=true; status_area_update(); wait_for(STATUS_BLUETOOTH_DISCONNECTED);
    bluetooth_enabled=false; status_area_update(); wait_for(STATUS_LIBRARY_SYNC);
    stays_visible(STATUS_LIBRARY_SYNC,3000,
                  "Cancelling Bluetooth must reveal the active sync instead of the clock");

    /* Rebuild while spinning, then clear the event during the new fade. */
    ui_set_library_syncing(true); status_area_update(); wait_for(STATUS_LIBRARY_SYNC);
    config.theme=(config.theme+1)%3; ui_set_system_config(&config); pump(1100);
    wait_for(STATUS_LIBRARY_SYNC);
    check(lv_obj_is_visible(g_status_icon), "Theme rebuilding must recreate active notifications");
    ui_select_screen(SCR_NOW_PLAYING); pump(600);
    np_gradient_visibility(true); pump(50);
    check(lv_color_eq(lv_obj_get_style_image_recolor(g_status_icon,0),lv_color_hex(COL_TEXT)),
          "Artwork-tinted chrome must also tint notification icons");
    np_gradient_visibility(false);
    ui_set_library_syncing(false); status_area_update(); wait_for(STATUS_CLOCK);
    bluetooth_enabled=true; bluetooth_state=BLE_REMOTE_CONNECTED; status_area_update();
    wait_for(STATUS_BLUETOOTH_CONNECTED); wait_for(STATUS_CLOCK);
    config.theme=(config.theme+1)%3; ui_set_system_config(&config); pump(1100);
    wait_for(STATUS_CLOCK);
    stays_visible(STATUS_CLOCK,5000,
                  "Rebuilding the theme must not replay an expired Bluetooth notification");
    lv_area_t final; lv_obj_get_coords(lv_obj_get_parent(g_status_time),&final);
    check(original.x1==final.x1 && original.x2==final.x2 && original.y1==final.y1 && original.y2==final.y2,
          "Notification transitions must not change the clock strip geometry");
    puts("PASS: smaller icons, persistent sync, newest-event priority, active-status restoration, touch and themes");
    return 0;
}
