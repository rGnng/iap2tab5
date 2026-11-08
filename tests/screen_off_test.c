/* Actual LVGL touch dispatch, clock hold, wake consumption and idle timer. */
#include "ui.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_indev_t * touch;
static lv_point_t point;
static bool pressed, backlight = true;
static int saved_minutes = -1;
static uint8_t pixels[1280 * 40 * 4];
static void check(bool condition, const char *message)
{
    if(!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{ LV_UNUSED(a); LV_UNUSED(p); lv_display_flush_ready(d); }
static void read_touch(lv_indev_t *d, lv_indev_data_t *data)
{ LV_UNUSED(d); data->point = point; data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void pump(int ms)
{ for(int i=0; i<ms; i+=10) { lv_tick_inc(10); lv_timer_handler(); } }
static void pointer(int x, int y, bool down)
{ point = (lv_point_t){x,y}; pressed=down; lv_indev_read(touch); pump(20); }
static void power(bool on) { backlight=on; }
static void save(const ui_system_config_t *cfg) { saved_minutes=cfg->screen_off_minutes; }
static lv_obj_t *label(lv_obj_t *o, const char *text)
{
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text)) return o;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++) {
        lv_obj_t *found=label(lv_obj_get_child(o,i),text); if(found) return found;
    }
    return NULL;
}
int main(void)
{
    lv_init();
    lv_display_t *d=lv_display_create(1280,720);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    touch=lv_indev_create(); lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch,d); lv_indev_set_read_cb(touch,read_touch);
    ui_system_config_t cfg={.touch_enabled=1,.menubar_left=1,.brightness=70,.volume=65};
    ui_apply_saved_settings(&cfg); ui_set_screen_power_cb(power); ui_init(); ui_attach_touch(touch); pump(600);
    ui_set_system_config_changed_cb(save);
    ui_select_screen(6); pump(300);
    ui_handle_key_repeat(UI_KEY_DOWN,5); ui_handle_key(UI_KEY_ENTER); pump(300);
    check(label(lv_screen_active(),"Screen Off") && label(lv_screen_active(),"30 min") &&
          label(lv_screen_active(),"1 hour") && label(lv_screen_active(),"Off"),"Screen Off choices must be available");
    ui_handle_key(UI_KEY_UP); ui_handle_key(UI_KEY_ENTER);
    check(saved_minutes==60,"Default Off selection moves up to 1 hour and persists");
    ui_handle_key(UI_KEY_UP); ui_handle_key(UI_KEY_ENTER);
    check(saved_minutes==30,"30 min choice must persist");
    ui_handle_key_repeat(UI_KEY_DOWN,2); ui_handle_key(UI_KEY_ENTER);
    check(saved_minutes==0,"Off choice must persist");
    ui_select_screen(0); pump(300);

    pointer(54,35,true); pump(1800);
    check(ui_screen_is_on(),"Clock must stay on before two seconds");
    pointer(54,35,false); pump(400);
    check(ui_screen_is_on(),"Short clock press must cancel the timer");
    pointer(54,35,true); pointer(200,35,true); pump(2200); pointer(200,35,false);
    check(ui_screen_is_on(),"Dragging outside the clock must not turn off the screen");
    pointer(54,35,true); pump(2000);
    check(!ui_screen_is_on() && !backlight,"Two-second hold must blank and disable backlight");
    pump(2500); pointer(54,35,false); pump(1100);
    check(!ui_screen_is_on(),"Held finger, release and playing must not wake a manual blank");

    player_snapshot_t before,after; player_get_snapshot(&before);
    ui_handle_key(UI_KEY_7); player_get_snapshot(&after);
    check(ui_screen_is_on() && backlight,"A key must wake the backlight");
    check(before.status==after.status,"Wake key must not toggle playback");
    ui_handle_key(UI_KEY_2); pump(400);
    check(label(lv_screen_active(),"Albums")!=NULL,"Albums must be shown");
    ui_screen_off(); ui_handle_key_repeat(UI_KEY_BACK,3); pump(200);
    check(ui_screen_is_on() && label(lv_screen_active(),"Albums"),"Wake burst must not navigate Back");

    ui_screen_off(); pointer(54,130,true); pointer(54,130,false); pump(300);
    check(ui_screen_is_on() && label(lv_screen_active(),"Albums"),"Wake touch must not activate Now Playing tile");
    pointer(54,130,true); pointer(54,130,false); pump(300);
    check(label(lv_screen_active(),"Albums")==NULL,"Second touch should navigate normally");

    player_get_snapshot(&after); if(after.status==PLAYER_PLAYING) player_play_pause();
    cfg.screen_off_minutes=30; ui_set_system_config(&cfg); pump(1100);
    lv_tick_inc(1801000); lv_timer_handler();
    check(!ui_screen_is_on(),"Idle LVGL timer must blank a paused player");
    ui_user_activity();
    cfg.screen_off_minutes=0; ui_set_system_config(&cfg);
    lv_tick_inc(3601000); lv_timer_handler();
    check(ui_screen_is_on(),"Off setting must disable automatic screen blanking");
    puts("PASS: screen timer, clock hold, backlight and wake-only input");
    return 0;
}
