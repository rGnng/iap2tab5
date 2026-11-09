/* Real LVGL rendering and navigation, with the production mock library. */
#include "../app/ui/ui.c"

static uint32_t frame[720][1280];
static uint8_t draw_buffer[1280 * 40 * 4];
static int fast_cues,exit_cues;
static void check(bool ok, const char *message)
{ if(!ok) { fprintf(stderr,"FAIL: %s\n",message); exit(1); } }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    for(int y=area->y1;y<=area->y2;y++)
        memcpy(&frame[y][area->x1],pixels+(y-area->y1)*lv_area_get_width(area)*4,
               lv_area_get_width(area)*4);
    lv_display_flush_ready(display);
}
static void pump(int ms)
{ for(int i=0;i<ms;i+=10) { lv_tick_inc(10);lv_timer_handler(); } }
static void feedback(int type) { if(type==UI_FEEDBACK_FAST_SCROLL) fast_cues++;else if(type==UI_FEEDBACK_FAST_SCROLL_EXIT)exit_cues++; }

static void alpha_fade(void)
{
    lv_obj_t *panel=lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel);lv_obj_set_pos(panel,300,200);lv_obj_set_size(panel,200,60);
    lv_obj_set_style_bg_color(panel,lv_color_hex(0x205090),0);
    lv_obj_set_style_bg_grad_color(panel,lv_color_hex(0x30a060),0);
    lv_obj_set_style_bg_grad_dir(panel,LV_GRAD_DIR_HOR,0);
    lv_obj_set_style_bg_opa(panel,LV_OPA_COVER,0);
    lv_obj_t *label=make_label(panel,"MMMMMMMMMMMMMMMMMMMM",FONT_H1,0xffffff);
    lv_obj_set_pos(label,10,5);lv_obj_set_width(label,120);ui_text_fade(label);
    lv_obj_remove_flag(label,LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);pump(100);
    lv_area_t bounds;lv_obj_get_content_coords(label,&bounds);
    uint32_t normal[60][16],background[60][16];
    int h=LV_MIN(60,lv_area_get_height(&bounds));
    for(int y=0;y<h;y++)for(int x=0;x<16;x++)normal[y][x]=frame[bounds.y1+y][bounds.x2-15+x];
    lv_obj_set_hidden(label,true);pump(100);
    for(int y=0;y<h;y++)for(int x=0;x<16;x++)background[y][x]=frame[bounds.y1+y][bounds.x2-15+x];
    lv_obj_set_hidden(label,false);lv_obj_add_flag(label,LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);pump(100);
    int ink=0;
    for(int y=0;y<h;y++)for(int x=0;x<16;x++) {
        uint32_t actual=frame[bounds.y1+y][bounds.x2-15+x];
        if(normal[y][x]!=background[y][x]) ink++;
        for(int shift=0;shift<24;shift+=8) {
            int bg=(background[y][x]>>shift)&255,fg=(normal[y][x]>>shift)&255;
            int expected=bg+(fg-bg)*(15-x)/15;
            check(abs((int)((actual>>shift)&255)-expected)<=4,
                  "Fading glyphs must blend into the live colored background, without a solid mask");
        }
    }
    check(ink>20,"Fade oracle must include real glyph pixels");lv_obj_delete(panel);pump(100);
}

static void artist_viewport(void)
{
    ui_select_screen(SCR_ARTISTS);pump(600);
    artists_select(18);pump(200);
    /* Touch scrolling can leave the selected artist outside the viewport. */
    lv_obj_scroll_to_y(artists_left_list,360,LV_ANIM_OFF);pump(100);
    int saved=lv_obj_get_scroll_y(artists_left_list),selected=artists_selected;
    check(saved>0,"Artist fixture must be scrolled");
    ui_select_screen(SCR_ALBUMS);pump(600);ui_select_screen(SCR_ARTISTS);pump(600);
    check(artists_selected==selected,"Returning to Artists must preserve selection");
    check(lv_obj_get_scroll_y(artists_left_list)==saved,"Returning to Artists must restore the exact viewport, even after touch scroll");
    artists_open((void *)(intptr_t)selected);pump(200);artists_back();pump(200);
    check(lv_obj_get_scroll_y(artists_left_list)==saved,"Artist detail rebuilds must preserve the viewport");
}

static unsigned audited_scrollbars;
static void audit_scrollbars(lv_obj_t *obj)
{
    /* Only explicit vertical viewports; LVGL also defaults decorative objects
     * and buttons to scrollable, even though they never hold a list. */
    if(lv_obj_is_visible(obj) && lv_obj_is_scrollable(obj) &&
       lv_obj_get_scroll_dir(obj)==LV_DIR_VER &&
       lv_obj_get_scrollbar_mode(obj)!=LV_SCROLLBAR_MODE_OFF) {
        lv_scrollbar_mode_t mode=lv_obj_get_scrollbar_mode(obj);
        bool was_scrolled=lv_obj_has_state(obj,LV_STATE_SCROLLED);
        lv_obj_set_scrollbar_mode(obj,LV_SCROLLBAR_MODE_ON);
        for(int state=0;state<2;state++) {
            if(state)lv_obj_add_state(obj,LV_STATE_SCROLLED);
            else lv_obj_remove_state(obj,LV_STATE_SCROLLED);
            if(lv_obj_get_style_radius(obj,LV_PART_SCROLLBAR)!=0 ||
               lv_obj_get_style_pad_right(obj,LV_PART_SCROLLBAR)!=6)
                fprintf(stderr,"Scrollbar style failure: screen=%d width=%ld height=%ld state=%d\n",
                    g_current_screen,(long)lv_obj_get_width(obj),(long)lv_obj_get_height(obj),state);
            check(lv_obj_get_style_radius(obj,LV_PART_SCROLLBAR)==0,"Every scrollbar must have square corners");
            check(lv_obj_get_style_pad_right(obj,LV_PART_SCROLLBAR)==6,"Every scrollbar must have a six-pixel right inset");
            if(lv_obj_get_scroll_dir(obj)&LV_DIR_VER) {
                lv_area_t horizontal,vertical,bounds;
                lv_obj_get_scrollbar_area(obj,&horizontal,&vertical);lv_obj_get_coords(obj,&bounds);
                check(vertical.x2>=vertical.x1,"A forced vertical scrollbar must have a real draw area");
                check(bounds.x2-vertical.x2==6,"Actual scrollbar geometry must be six pixels inside its container");
            }
        }
        if(!was_scrolled)lv_obj_remove_state(obj,LV_STATE_SCROLLED);
        lv_obj_set_scrollbar_mode(obj,mode);audited_scrollbars++;
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);i++)audit_scrollbars(lv_obj_get_child(obj,i));
}
static void scrollbar_views(void)
{
    for(int screen=SCR_ALBUMS;screen<SCR_COUNT;screen++) {
        ui_select_screen(screen);pump(600);audit_scrollbars(lv_screen_active());
        if(screen==SCR_ALBUMS) { albums_open((void *)(intptr_t)0);pump(200);audit_scrollbars(lv_screen_active()); }
        if(screen==SCR_ARTISTS) { artists_open((void *)(intptr_t)0);pump(200);audit_scrollbars(lv_screen_active()); }
    }
    ui_select_screen(SCR_NOW_PLAYING);pump(300);np_toggle_queue();pump(600);
    audit_scrollbars(lv_screen_active());np_toggle_queue();pump(300);
    check(audited_scrollbars>=10,"Audit must cover virtual lists, detail panes, Settings and Up Next");
}

static void fast_navigation(void)
{
    g_system_config.fast_scroll_hold_ms=150; /* Exercise the shortest selectable hold. */
    g_system_config.fast_scroll_pages=2;g_system_config.sound_mode=2;g_system_config.sound_enabled=true;
    ui_set_feedback_cb(feedback);ui_select_screen(SCR_ALBUMS);pump(600);
    albums_select(0);pump(200);
    lv_obj_t *context;int rows=ui_fast_scroll_rows(&context);
    check(rows==8,"Albums must use eight visible rows per fast notch");
    uint32_t now=lv_tick_get();
    ui_handle_key_repeat_timed(UI_KEY_DOWN,rows*2,now-750,now);
    check(albums_selected==rows*3-1,"Threshold crossing must apply a page jump once and coalesce rendering");
    check(fast_cues==1 && g_fast_scroll.active,"Entering fast scroll must produce exactly one special cue");
    check(g_fast_marker && !lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),"Fast cursor rails must be visible");
    check(lv_color_eq(lv_obj_get_style_border_color(g_fast_marker,0),
                      lv_color_hex(g_theme->on_selection)),
          "Fast indicator must use a contrasting selection foreground in every theme");
    pump(40);lv_area_t marker;lv_obj_get_coords(g_fast_marker,&marker);
    check((frame[(marker.y1+marker.y2)/2][marker.x1+1]&0xffffff)==g_theme->on_selection,
          "Fast cursor rails must actually render above the selected row");
    /* A queued BLE report can map one tick behind its predecessor because
     * the LVGL tick and report age are sampled at different resolutions. */
    now=lv_tick_get();int previous=albums_selected;
    ui_handle_key_repeat_timed(UI_KEY_DOWN,1,now,now);
    ui_handle_key_repeat_timed(UI_KEY_DOWN,1,now-1,now-1);
    check(g_fast_scroll.active && fast_cues==1 && albums_selected==previous+rows*2,
          "A slightly older mapped timestamp must keep fast mode and page steps");
    int selected_slot=-1;
    for(int i=0;i<ALBUMS_POOL;i++) if(albums_row_indices[i]==albums_selected) selected_slot=i;
    check(selected_slot>=0,"Fixture must have a selected virtual row");
    int bound=albums_row_indices[selected_slot];albums_row_indices[selected_slot]=-1;
    ui_fast_marker_update(NULL);
    check(g_fast_scroll.active && fast_cues==1,
          "A temporarily unbound cursor row must not cancel fast scrolling");
    check(lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),"Unavailable row only hides the indicator");
    albums_row_indices[selected_slot]=bound;ui_fast_marker_update(NULL);pump(40);
    check(g_fast_scroll.active && !lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),
          "Cursor indicator must recover after virtual row binding without another cue");
    int saved_scroll=lv_obj_get_scroll_y(albums_left_list);
    lv_obj_scroll_to_y(albums_left_list,0,LV_ANIM_OFF);ui_fast_marker_update(NULL);
    check(g_fast_scroll.active && lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),
          "A temporarily offscreen selected row must only hide the cursor indicator");
    lv_obj_scroll_to_y(albums_left_list,saved_scroll,LV_ANIM_OFF);ui_fast_marker_update(NULL);pump(40);
    check(g_fast_scroll.active && !lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),
          "Cursor must recover after viewport layout without cancelling fast mode");
    for(int step=0;step<20;step++) {
        pump(50);ui_handle_key(UI_KEY_DOWN);
        if(!g_fast_scroll.active || fast_cues!=1)
            fprintf(stderr,"Sustained spin reset: step=%d selected=%d cues=%d active=%d\n",step,albums_selected,fast_cues,g_fast_scroll.active);
        check(g_fast_scroll.active && fast_cues==1,
              "A continuous page spin must survive recycled rows and reaching the list end");
    }
    albums_select(rows*3-1);
    int old=albums_selected;ui_handle_key(UI_KEY_DOWN);
    check(albums_selected==old+rows && fast_cues==1,"One fast notch must move exactly one page without another entry cue");
    int exits_before=exit_cues;
    ui_handle_key(UI_KEY_UP);
    check(exit_cues==exits_before+1,"A direction reversal must play one exit cue");
    ui_fast_marker_update(NULL);
    check(exit_cues==exits_before+1,"Inactive indicator refresh must not repeat the exit cue");
    check(albums_selected==old+rows-1 && !g_fast_scroll.active,"A reversal must immediately return to one-row movement");
    for(int i=0;i<14;i++) { pump(50);ui_handle_key(UI_KEY_UP); }
    check(!g_fast_scroll.active,"Reverse spin must first rebuild its own physical velocity");
    pump(50);ui_handle_key(UI_KEY_UP);
    check(g_fast_scroll.active && fast_cues==2,"A sustained reverse spin must re-enter fast mode without losing its first notch");
    ui_handle_key(UI_KEY_UP);ui_handle_key(UI_KEY_UP);pump(40);
    check(albums_selected==-1 && g_fast_scroll.active && fast_cues==2,
          "Reaching Shuffle All must preserve the gesture and not add entry cues");
    ui_fast_scroll_reset();now=lv_tick_get();
    ui_handle_key_repeat_timed(UI_KEY_DOWN,rows*2,now-750,now);pump(100);
    check(g_fast_scroll.active && !lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),
          "A 100 ms pause must preserve fast mode and its marker");
    old=albums_selected;int cues=fast_cues;ui_handle_key(UI_KEY_DOWN);
    check(albums_selected==old+rows && fast_cues==cues,
          "Resuming after 100 ms must keep page steps without repeating the entry cue");
    exits_before=exit_cues;
    pump(200);
    check(exit_cues==exits_before+1,"Idle expiry must play one descending exit cue");
    pump(200);check(exit_cues==exits_before+1,"Idle exit cue must not repeat");
    check(!g_fast_scroll.active && lv_obj_has_flag(g_fast_marker,LV_OBJ_FLAG_HIDDEN),"Pausing must clear fast mode and its marker");
    old=albums_selected;ui_handle_key(UI_KEY_DOWN);check(albums_selected==old+1,"First notch after a pause must be precise");
    ui_fast_scroll_reset();g_system_config.fast_scroll_pages=0;albums_select(0);
    ui_handle_key_repeat(UI_KEY_DOWN,20);check(albums_selected==20 && !g_fast_scroll.active,"Off must retain one row per notch");
    g_system_config.fast_scroll_pages=2;ui_fast_scroll_reset();
    ui_select_screen(SCR_SETTINGS);pump(600);settings_open((void *)(intptr_t)SETTING_FAST_SCROLL);pump(200);
    check(g_focus_count==10 && g_focus_index==2,"Fast Scroll headings must be unselectable, with six thresholds and four hold times");
    ui_handle_key(UI_KEY_UP);ui_handle_key(UI_KEY_ENTER);
    check(g_system_config.fast_scroll_pages==1,"Fast Scroll choices must change the saved UI setting");
    ui_handle_key_repeat(UI_KEY_DOWN,5);
    for(int i=0;i<4;i++) {
        if(i) ui_handle_key(UI_KEY_DOWN);
        ui_handle_key(UI_KEY_ENTER);
        check(g_system_config.fast_scroll_hold_ms==settings_fast_hold_values[i],
              "Each Hold Time choice must update the persisted setting");
    }
    ui_handle_key(UI_KEY_BACK);ui_select_screen(SCR_NOW_PLAYING);pump(200);
    check(!g_fast_scroll.active && !g_fast_context,"Changing tabs must discard velocity history");
}

int main(int argc,char **argv)
{
    lv_init();lv_display_t *display=lv_display_create(1280,720);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    ui_system_config_t config={.theme=argc>1?atoi(argv[1]):0,.brightness=45,.menubar_left=1,.fast_scroll_pages=2};
    ui_apply_saved_settings(&config);ui_init();pump(600);
    check(g_system_config.fast_scroll_hold_ms==300,"Missing hold configuration must default to 300 ms");
    alpha_fade();artist_viewport();scrollbar_views();fast_navigation();
    puts("PASS: transparent text fade, Artists viewport restoration, page scrolling and theme cursor feedback");
    return 0;
}
