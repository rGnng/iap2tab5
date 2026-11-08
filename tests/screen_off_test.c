/* Actual LVGL touch dispatch, clock hold, wake consumption and idle timer. */
#include "ui.h"
#include "player.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_indev_t * touch;
static lv_point_t point;
static bool pressed, backlight = true;
static int saved_minutes = -1;
static int saved_brightness = -1, saves;
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
static void save(const ui_system_config_t *cfg)
{ saved_minutes=cfg->screen_off_minutes; saved_brightness=cfg->brightness; saves++; }
static lv_obj_t *label(lv_obj_t *o, const char *text)
{
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text)) return o;
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++) {
        lv_obj_t *found=label(lv_obj_get_child(o,i),text); if(found) return found;
    }
    return NULL;
}
static void tap_label(const char *text)
{
    lv_obj_t *o=label(lv_screen_active(),text);
    check(o!=NULL,"Touch target must exist");
    lv_area_t a; lv_obj_get_coords(o,&a);
    int x=(a.x1+a.x2)/2, y=(a.y1+a.y2)/2;
    pointer(x,y,true); pointer(x,y,false); pump(100);
}
static bool checked(const char *text)
{
    lv_obj_t *o=label(lv_screen_active(),text);
    return o && label(lv_obj_get_parent(o),ICON_CHECK)!=NULL;
}
int main(int argc, char **argv)
{
    lv_init();
    lv_display_t *d=lv_display_create(1280,720);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    touch=lv_indev_create(); lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch,d); lv_indev_set_read_cb(touch,read_touch);
    ui_system_config_t cfg={.theme=argc>1?atoi(argv[1]):0,.touch_enabled=1,.menubar_left=1,.brightness=70,.volume=65};
    ui_apply_saved_settings(&cfg); ui_set_screen_power_cb(power); ui_init(); ui_attach_touch(touch); pump(600);
    ui_set_system_config_changed_cb(save);
    ui_select_screen(6); pump(300);
    ui_handle_key_repeat(UI_KEY_DOWN,5); ui_handle_key(UI_KEY_ENTER); pump(300);
    check(label(lv_screen_active(),"Screen") && label(lv_screen_active(),"Brightness") &&
          label(lv_screen_active(),"Idle Timer") && !label(lv_screen_active(),"Screen Off"),"Screen must contain both inset groups");
    check(label(lv_screen_active(),"Low") && label(lv_screen_active(),"Mid") &&
          label(lv_screen_active(),"High"),"All brightness presets must be available");
    check(label(lv_screen_active(),"30 min") && label(lv_screen_active(),"1 hour") &&
          label(lv_screen_active(),"Off"),"All idle choices must remain available");
    check(saved_brightness==-1,"Opening Screen must not change custom brightness");
    tap_label("Brightness"); tap_label("Idle Timer");
    check(saves==0,"Inset headings must not activate actions");
    check(!lv_obj_is_clickable(lv_obj_get_parent(label(lv_screen_active(),"Brightness"))) &&
          !lv_obj_is_clickable(lv_obj_get_parent(label(lv_screen_active(),"Idle Timer"))),"Inset headings must not be clickable");
    /* Initial focus is the nearest preset (Mid for an existing 70%). */
    ui_handle_key(UI_KEY_UP); ui_handle_key(UI_KEY_ENTER);
    check(saved_brightness==5 && checked("Low") && !checked("Mid"),"Low must select and persist 5%");
    ui_handle_key(UI_KEY_DOWN); ui_handle_key(UI_KEY_ENTER);
    check(saved_brightness==45 && checked("Mid") && !checked("Low"),"Mid must select and persist 45%");
    ui_handle_key(UI_KEY_DOWN); ui_handle_key(UI_KEY_ENTER);
    check(saved_brightness==100 && checked("High"),"High must select and persist 100%");
    ui_handle_key(UI_KEY_DOWN); ui_handle_key(UI_KEY_ENTER);
    check(saved_minutes==30,"30 min choice must persist");
    check(saved_brightness==100,"Navigation must skip the Idle Timer heading");
    ui_handle_key(UI_KEY_DOWN); ui_handle_key(UI_KEY_ENTER);
    check(saved_minutes==60,"1 hour choice must persist");
    ui_handle_key(UI_KEY_DOWN); ui_handle_key(UI_KEY_ENTER);
    check(saved_minutes==0,"Off choice must persist");
    tap_label("Mid"); check(saved_brightness==45 && checked("Mid"),"Touch must apply the same brightness presets");
    /* A web brightness update must survive a later idle timer change. */
    cfg.brightness=17; ui_set_system_config(&cfg); pump(600);
    check(!checked("Low") && !checked("Mid") && !checked("High"),"Custom external brightness must not falsely check a preset");
    tap_label("30 min");
    check(saved_minutes==30 && saved_brightness==17,"Idle choice must preserve external brightness");
    cfg.brightness=0; ui_set_system_config(&cfg); pump(600);
    check(checked("Low"),"A zero brightness setting must bounce to Low");
    tap_label("1 hour");
    check(saved_minutes==60 && saved_brightness==5,"Zero brightness must persist as 5%, not blank the screen");
    tap_label("Off");
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
