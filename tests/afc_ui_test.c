/* AFC screen: real pointer/key dispatch through trust and completion states. */
#include "../app/ui/ui.c"

static ui_afc_status_t state;
static int actions[4];
static uint8_t pixels[1280*40*4];
static lv_indev_t *touch;
static lv_point_t point;
static bool pressed;
static void check(bool ok,const char *message)
{ if(!ok) { fprintf(stderr,"FAIL: %s\n",message); exit(1); } }
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *data)
{ LV_UNUSED(area); LV_UNUSED(data); lv_display_flush_ready(display); }
static void status(ui_afc_status_t *out) { *out=state; }
static void action(int id) { check(id>=0 && id<4,"AFC action must be valid"); actions[id]++; }
static void read_touch(lv_indev_t *indev,lv_indev_data_t *data)
{ LV_UNUSED(indev); data->point=point; data->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; }
static void pump(int ms)
{ for(int i=0;i<ms;i+=10) { lv_tick_inc(10); lv_timer_handler(); } }
static bool disabled(int id) { return lv_obj_has_state(afc_buttons[id],LV_STATE_DISABLED); }
static bool focused(int id)
{ return g_focus_index>=0 && g_focus[g_focus_index].obj==afc_buttons[id]; }
static void tap(int id)
{
    lv_area_t bounds; lv_obj_get_coords(afc_buttons[id],&bounds);
    point=(lv_point_t){(bounds.x1+bounds.x2)/2,(bounds.y1+bounds.y2)/2};
    pressed=true; lv_indev_read(touch); pump(50);
    pressed=false; lv_indev_read(touch); pump(50);
}
int main(int argc,char **argv)
{
    lv_init(); lv_display_t *display=lv_display_create(1280,720);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    ui_system_config_t config={.theme=argc>1?atoi(argv[1]):0,.brightness=45,.touch_enabled=1};
    ui_apply_saved_settings(&config); ui_set_afc_sync_mode(true);
    ui_set_afc_status_cb(status); ui_set_afc_action_cb(action);
    state=(ui_afc_status_t){.available=true,.afc_mode=true,.sd_ready=true};
    ui_init(); pump(600);
    touch=lv_indev_create(); lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch,display); lv_indev_set_read_cb(touch,read_touch); ui_attach_touch(touch);
    check(disabled(UI_AFC_ACTION_PAIR),"No USB device must leave Pair / Trust disabled");
    state.usb_connected=true; pump(600);
    check(!disabled(UI_AFC_ACTION_PAIR),"A new attached device must still be able to request Trust");
    tap(UI_AFC_ACTION_PAIR); check(actions[UI_AFC_ACTION_PAIR]==1,"An untrusted device must accept a real Pair / Trust tap");
    state.pair_busy=true; pump(600);
    check(disabled(UI_AFC_ACTION_PAIR),"Pairing must disable repeat trust requests");
    state.pair_busy=false; state.pair_trusted=true; pump(600);
    check(disabled(UI_AFC_ACTION_PAIR),"Approved trust must disable Pair / Trust during AFC setup");
    state.connected=true; state.pair_trusted=false; pump(600);
    check(disabled(UI_AFC_ACTION_PAIR) && !disabled(UI_AFC_ACTION_SYNC),
          "A ready AFC connection must disable pairing and allow Sync");
    tap(UI_AFC_ACTION_PAIR); ui_handle_key(UI_KEY_ENTER); pump(100);
    check(actions[UI_AFC_ACTION_PAIR]==1,"Disabled pairing must reject both pointer and knob activation");
    tap(UI_AFC_ACTION_SYNC); check(actions[UI_AFC_ACTION_SYNC]==1,"Sync must remain available on a connected device");
    state.job_active=true; pump(600);
    check(disabled(UI_AFC_ACTION_PAIR) && disabled(UI_AFC_ACTION_IPOD_MODE),"An active sync must keep pairing and Exit disabled");
    state.job_active=false; state.job_finished=true; pump(600);
    check(focused(UI_AFC_ACTION_IPOD_MODE) && !disabled(UI_AFC_ACTION_IPOD_MODE),
          "Sync completion must move focus to the enabled Exit button");
    ui_handle_key(UI_KEY_UP); pump(1200);
    check(focused(UI_AFC_ACTION_SYNC),"Later completion polls must preserve the user's new selection");
    state.pair_busy=true; pump(600); state.pair_busy=false; pump(600);
    check(focused(UI_AFC_ACTION_SYNC),"Unrelated busy-state updates must not replay a completed sync's focus move");
    state.job_finished=false; state.job_active=true; pump(600);
    state.job_active=false; state.job_finished=true; state.job_failed=true; pump(600);
    check(focused(UI_AFC_ACTION_IPOD_MODE),"A second finished sync must focus Exit again, including an error result");
    config.theme=(config.theme+1)%3; ui_set_system_config(&config); pump(600);
    check(focused(UI_AFC_ACTION_IPOD_MODE),"A theme rebuild of a finished sync must preserve Exit focus");
    ui_handle_key(UI_KEY_ENTER);
    check(actions[UI_AFC_ACTION_IPOD_MODE]==1,"The next knob press after completion must activate Exit");
    state.job_finished=false; state.job_failed=false; state.connected=false; state.usb_connected=false; pump(600);
    state.usb_connected=true; state.pair_trusted=false; pump(600);
    check(!disabled(UI_AFC_ACTION_PAIR),"A replacement device needing trust must re-enable pairing");
    puts("PASS: AFC trust gating, disabled touch/key actions and one-time completion focus");
    return 0;
}
