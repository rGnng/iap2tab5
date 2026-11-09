/* Real LVGL rendering and navigation, with the production mock library. */
#include "../app/ui/ui.c"

static uint32_t frame[720][1280];
static uint8_t draw_buffer[1280 * 40 * 4];
static int fast_cues,exit_cues;
static lv_indev_t *touch;
static lv_point_t touch_point;
static bool touch_down;
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

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{ LV_UNUSED(indev);data->point=touch_point;data->state=touch_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; }
static void pointer(int x,int y,bool down)
{ touch_point=(lv_point_t){x,y};touch_down=down;lv_indev_read(touch);pump(30); }
static lv_obj_t *first_label(lv_obj_t *obj)
{
    if(lv_obj_check_type(obj,&lv_label_class))return obj;
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);i++) {
        lv_obj_t *found=first_label(lv_obj_get_child(obj,i));if(found)return found;
    }
    return NULL;
}
static void press_control(lv_obj_t *obj,bool styled)
{
    check(obj && lv_obj_is_visible(obj),"Press fixture must have a visible target");
    lv_area_t bounds,before,after;lv_obj_get_coords(obj,&bounds);
    lv_obj_t *label=first_label(obj);
    check(label!=NULL,"Press fixture must include a label");
    lv_state_t state=lv_obj_get_state(label);
    lv_color_t color=lv_obj_get_style_text_color(label,0);
    lv_obj_get_coords(label,&before);
    pointer((bounds.x1+bounds.x2)/2,(bounds.y1+bounds.y2)/2,true);
    if(lv_obj_has_state(obj,LV_STATE_USER_2)!=styled)
        fprintf(stderr,"Press mismatch: screen=%d label=%s bounds=(%ld,%ld)-(%ld,%ld) expected=%d state=%u\n",
            g_current_screen,lv_label_get_text(label),(long)bounds.x1,(long)bounds.y1,(long)bounds.x2,(long)bounds.y2,styled,lv_obj_get_state(obj));
    check(lv_obj_has_state(obj,LV_STATE_USER_2)==styled,
          "Touch targets must show the press state, except Now Playing content");
    if(styled) {
        lv_obj_get_coords(label,&after);
        check(lv_area_is_equal(&before,&after),"Press feedback must not shift text or change row geometry");
        check(lv_color_eq(lv_obj_get_style_text_color(label,0),lv_color_hex(g_theme->on_selection)),
              "Pressed text must stay legible, including nested and already-selected labels");
        if((frame[bounds.y2][(bounds.x1+bounds.x2)/2]&0xffffff)!=g_theme->on_selection)
            fprintf(stderr,"Press pixels: screen=%d label=%s expected=%06lx actual=%06lx\n",g_current_screen,
                lv_label_get_text(label),(unsigned long)g_theme->on_selection,
                (unsigned long)(frame[bounds.y2][(bounds.x1+bounds.x2)/2]&0xffffff));
        check((frame[bounds.y2][(bounds.x1+bounds.x2)/2]&0xffffff)==g_theme->on_selection,
              "A real pointer press must render the square inset outline");
    }
    /* Cancel through the input device so destructive/playback actions do not
     * run; LVGL delivers PRESS_LOST, as it does when a drag takes over. */
    lv_indev_wait_release(touch);
    pointer(touch_point.x,touch_point.y,false);
    check(!lv_obj_has_state(obj,LV_STATE_USER_2),"Cancelling a touch must clear its temporary highlight");
    check(lv_obj_get_state(label)==state && lv_color_eq(lv_obj_get_style_text_color(label,0),color),
          "Cancelling a press must restore the original selection and label color");
}
static void pressed_controls(void)
{
    touch=lv_indev_create();lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch,lv_display_get_default());lv_indev_set_read_cb(touch,read_touch);
    ui_set_touch_enabled(true);ui_attach_touch(touch);
    ui_select_screen(SCR_ALBUMS);pump(600);albums_select(0);pump(100);
    press_control(lv_obj_get_parent(g_status_time),true);
    press_control(g_menu_items[SCR_ALBUMS],true);
    press_control(albums_rows[0],true);
    press_control(lv_obj_get_child(albums_right_content,1),false); /* The scroll viewport itself is not a button. */
    press_control(lv_obj_get_child(lv_obj_get_child(albums_right_content,1),0),true);
    press_control(g_mini,false);
    ui_select_screen(SCR_ARTISTS);pump(600);artists_select(0);pump(100);
    press_control(artists_rows[0],true);
    press_control(lv_obj_get_child(lv_obj_get_child(artists_right_content,1),0),true);
    ui_select_screen(SCR_SONGS);pump(600);songs_select(0);pump(100);
    press_control(songs_rows[0],true);
    ui_select_screen(SCR_PLAYLISTS);pump(600);
    press_control(lv_obj_get_child(playlists_left_list,0),true);
    ui_select_screen(SCR_SETTINGS);pump(600);
    press_control(lv_obj_get_child(settings_left_list,0),true);
    settings_open((void *)(intptr_t)SETTING_SCREEN);pump(200);
    press_control(g_focus[g_focus_index].obj,true);
    ui_select_screen(SCR_BLUETOOTH);pump(600);
    press_control(g_focus[0].obj,true);
    ui_select_screen(SCR_NOW_PLAYING);pump(600);
    press_control(lv_obj_get_parent(np_play_icon),false);
    press_control(np_star_btn,false);
    press_control(np_shuffle_btn,false);
    np_toggle_queue();pump(600);
    press_control(g_focus[np_queue_focus_start].obj,false);
    np_toggle_queue();pump(200);
    np_gradient_visibility(true);pump(100);
    press_control(lv_obj_get_parent(g_status_time),true);
    press_control(g_menu_items[SCR_NOW_PLAYING],true);
    np_gradient_visibility(false);pump(100);
    ui_attach_touch(NULL);lv_indev_delete(touch);touch=NULL;
}

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
               lv_obj_get_style_pad_right(obj,LV_PART_SCROLLBAR)!=8)
                fprintf(stderr,"Scrollbar style failure: screen=%d width=%ld height=%ld state=%d\n",
                    g_current_screen,(long)lv_obj_get_width(obj),(long)lv_obj_get_height(obj),state);
            check(lv_obj_get_style_radius(obj,LV_PART_SCROLLBAR)==0,"Every scrollbar must have square corners");
            check(lv_obj_get_style_pad_right(obj,LV_PART_SCROLLBAR)==8,"Every scrollbar must have an eight-pixel right inset");
            if(lv_obj_get_scroll_dir(obj)&LV_DIR_VER) {
                lv_area_t horizontal,vertical,bounds,column;
                lv_obj_get_scrollbar_area(obj,&horizontal,&vertical);
                lv_obj_get_coords(obj,&bounds);
                /* The parent spans the visible column, including its padding.
                 * Measuring only the list's bounds would miss the old extra
                 * 16 px (22 px on Bluetooth) between list and column edges. */
                lv_obj_get_coords(lv_obj_get_parent(obj),&column);
                check(vertical.x2>=vertical.x1,"A forced vertical scrollbar must have a real draw area");
                check(bounds.x2==column.x2,"The scrolling viewport must reach the visible column edge");
                check(column.x2-vertical.x2==8,"Actual scrollbar geometry must be eight pixels inside the visible column");
                if(state) {
                    pump(300); /* Complete the theme's opacity transition. */
                    int y=(vertical.y1+vertical.y2)/2;
                    uint32_t color=lv_color_to_u32(lv_obj_get_style_bg_color(obj,LV_PART_SCROLLBAR))&0xffffff;
                    check((frame[y][vertical.x2]&0xffffff)==color,
                          "The scrollbar must render at the column inset without being clipped by old list bounds");
                }
            }
        }
        if(!was_scrolled)lv_obj_remove_state(obj,LV_STATE_SCROLLED);
        lv_obj_set_scrollbar_mode(obj,mode);audited_scrollbars++;
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);i++)audit_scrollbars(lv_obj_get_child(obj,i));
}
static void scrollbar_views(void)
{
    for(int side=0;side<2;side++) {
        ui_set_menubar_left(side==0);pump(100);
        for(int screen=SCR_ALBUMS;screen<SCR_COUNT;screen++) {
            ui_select_screen(screen);pump(600);audit_scrollbars(lv_screen_active());
            lv_obj_t *master=screen==SCR_ALBUMS?albums_left_list:
                screen==SCR_ARTISTS?artists_left_list:
                screen==SCR_PLAYLISTS?playlists_left_list:
                screen==SCR_SETTINGS?settings_left_list:NULL;
            if(master)check(lv_obj_get_content_width(master)==SIDE_PANEL_W-32,
                           "Moving the scrollbar must preserve master-list content and row widths");
            if(screen==SCR_SONGS)check(lv_obj_get_content_width(songs_list)==CONTENT_W-32-14,
                                      "Moving the Songs scrollbar must preserve its text and row widths");
            if(screen==SCR_ALBUMS)albums_open((void *)(intptr_t)0);
            if(screen==SCR_ARTISTS)artists_open((void *)(intptr_t)0);
            if(screen==SCR_PLAYLISTS)playlists_open((void *)(intptr_t)0);
            if(screen==SCR_SETTINGS)settings_open((void *)(intptr_t)SETTING_FAST_SCROLL);
            pump(200);audit_scrollbars(lv_screen_active());
        }
        ui_select_screen(SCR_NOW_PLAYING);pump(300);np_toggle_queue();pump(600);
        audit_scrollbars(lv_screen_active());np_toggle_queue();pump(300);
    }
    ui_set_menubar_left(true);pump(100);
    check(audited_scrollbars>=30,"Audit must cover virtual lists, detail panes, Settings and Up Next with either menu position");
}

static lv_obj_t *cursor_row(void)
{
    if(g_current_screen==SCR_ALBUMS && !albums_detail_open) {
        if(albums_selected<0)return albums_shuffle_row;
        for(int i=0;i<ALBUMS_POOL;i++)if(albums_row_indices[i]==albums_selected)return albums_rows[i];
    }
    else if(g_current_screen==SCR_ARTISTS && !artists_detail_open) {
        for(int i=0;i<ARTISTS_POOL;i++)if(artists_row_indices[i]==artists_selected)return artists_rows[i];
    }
    else if(g_current_screen==SCR_SONGS) {
        for(int i=0;i<SONGS_POOL;i++)if(songs_row_indices[i]==songs_selected)return songs_rows[i];
    }
    else if(g_focus_index>=0 && g_focus_index<g_focus_count)return g_focus[g_focus_index].obj;
    return NULL;
}

static void check_cursor_band(bool fast)
{
    lv_obj_t *row=cursor_row();check(row!=NULL,"Cursor fixture must have a bound row");
    uint32_t expected=fast?g_theme->fast_scroll_band:COL_ACCENT;
    check(lv_color_eq(lv_obj_get_style_border_color(row,0),lv_color_hex(expected)),
          "The selected row's left band must reflect the scroll mode");
    check(lv_obj_get_style_border_side(row,0)==LV_BORDER_SIDE_LEFT &&
          lv_obj_get_style_border_width(row,0)==3,"Scroll mode must preserve the three-pixel left band");
    lv_obj_update_layout(lv_screen_active());lv_refr_now(lv_display_get_default());
    lv_area_t bounds,viewport;lv_obj_get_coords(row,&bounds);
    lv_obj_get_content_coords(lv_obj_get_parent(row),&viewport);
    int top=LV_MAX(bounds.y1,viewport.y1),bottom=LV_MIN(bounds.y2,viewport.y2);
    check(top<=bottom && top>=0 && bottom<720,"Cursor band must be in the visible viewport");
    check((frame[(top+bottom)/2][bounds.x1+1]&0xffffff)==expected,
          "The real rendered cursor band must show the mode color");
}

static void fast_navigation(void)
{
    g_system_config.fast_scroll_hold_ms=150; /* Exercise the shortest selectable hold. */
    g_system_config.fast_scroll_enabled=true;
    g_system_config.fast_scroll_pages=2;g_system_config.sound_mode=2;g_system_config.sound_enabled=true;
    ui_set_feedback_cb(feedback);ui_select_screen(SCR_ALBUMS);pump(600);
    albums_select(0);pump(200);
    lv_obj_t *context;int rows=ui_fast_scroll_rows(&context);
    check(rows==8,"Albums must use eight visible rows per fast notch");
    uint32_t now=lv_tick_get();
    uint32_t overlays=lv_obj_get_child_count(lv_layer_top());
    ui_handle_key_repeat_timed(UI_KEY_DOWN,rows*2,now-500,now);
    check(albums_selected==rows*3-1,"Threshold crossing must apply a page jump once and coalesce rendering");
    check(fast_cues==1 && g_fast_scroll.active,"Entering fast scroll must produce exactly one special cue");
    check(lv_obj_get_child_count(lv_layer_top())==overlays,"Fast scrolling must not create an overlay indicator");
    pump(40);check_cursor_band(true);
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
    ui_fast_scroll_visual_update(NULL);
    check(g_fast_scroll.active && fast_cues==1,
          "A temporarily unbound cursor row must not cancel fast scrolling");
    check(lv_color_eq(lv_obj_get_style_border_color(albums_rows[selected_slot],0),lv_color_hex(g_theme->fast_scroll_band)),
          "Temporary unbinding must not reset the row's fast band color");
    albums_row_indices[selected_slot]=bound;ui_fast_scroll_visual_update(NULL);pump(40);
    check(g_fast_scroll.active,"Binding a virtual row must retain fast mode");check_cursor_band(true);
    int saved_scroll=lv_obj_get_scroll_y(albums_left_list);
    lv_obj_scroll_to_y(albums_left_list,0,LV_ANIM_OFF);ui_fast_scroll_visual_update(NULL);
    check(g_fast_scroll.active,"An offscreen cursor must not cancel fast mode");
    lv_obj_scroll_to_y(albums_left_list,saved_scroll,LV_ANIM_OFF);ui_fast_scroll_visual_update(NULL);pump(40);
    check(g_fast_scroll.active,"Restoring the viewport must retain fast mode");check_cursor_band(true);
    for(int step=0;step<20;step++) {
        pump(50);ui_handle_key(UI_KEY_DOWN);
        if(!g_fast_scroll.active || fast_cues!=1)
            fprintf(stderr,"Sustained spin reset: step=%d selected=%d cues=%d active=%d\n",step,albums_selected,fast_cues,g_fast_scroll.active);
        check(g_fast_scroll.active && fast_cues==1,
              "A continuous page spin must survive recycled rows and reaching the list end");
        check_cursor_band(true);
    }
    albums_select(rows*3-1);
    int old=albums_selected;ui_handle_key(UI_KEY_DOWN);
    check(albums_selected==old+rows && fast_cues==1,"One fast notch must move exactly one page without another entry cue");
    int exits_before=exit_cues;
    ui_handle_key(UI_KEY_UP);
    check(exit_cues==exits_before+1,"A direction reversal must play one exit cue");
    ui_fast_scroll_visual_update(NULL);
    check(exit_cues==exits_before+1,"Inactive indicator refresh must not repeat the exit cue");
    check(albums_selected==old+rows-1 && !g_fast_scroll.active,"A reversal must immediately return to one-row movement");
    check_cursor_band(false);
    for(int i=0;i<14;i++) { pump(30);ui_handle_key(UI_KEY_UP); }
    check(!g_fast_scroll.active,"Reverse spin must first rebuild its own physical velocity");
    pump(30);ui_handle_key(UI_KEY_UP);
    check(g_fast_scroll.active && fast_cues==2,"A sustained reverse spin must re-enter fast mode without losing its first notch");
    ui_handle_key(UI_KEY_UP);ui_handle_key(UI_KEY_UP);pump(40);
    check(albums_selected==-1 && g_fast_scroll.active && fast_cues==2,
          "Reaching Shuffle All must preserve the gesture and not add entry cues");
    ui_fast_scroll_reset();now=lv_tick_get();
    ui_handle_key_repeat_timed(UI_KEY_DOWN,rows*2,now-500,now);pump(100);
    check(g_fast_scroll.active,"A 100 ms pause must preserve fast mode");check_cursor_band(true);
    old=albums_selected;int cues=fast_cues;ui_handle_key(UI_KEY_DOWN);
    check(albums_selected==old+rows && fast_cues==cues,
          "Resuming after 100 ms must keep page steps without repeating the entry cue");
    exits_before=exit_cues;
    pump(200);
    check(exit_cues==exits_before+1,"Idle expiry must play one descending exit cue");
    pump(200);check(exit_cues==exits_before+1,"Idle exit cue must not repeat");
    check(!g_fast_scroll.active,"Pausing must clear fast mode");check_cursor_band(false);
    old=albums_selected;ui_handle_key(UI_KEY_DOWN);check(albums_selected==old+1,"First notch after a pause must be precise");
    ui_fast_scroll_reset();g_system_config.fast_scroll_enabled=false;albums_select(0);
    ui_handle_key_repeat(UI_KEY_DOWN,20);
    check(albums_selected==20 && !g_fast_scroll.active && g_system_config.fast_scroll_pages==2,
          "Disabled must retain one row per notch without losing the activation threshold");
    g_system_config.fast_scroll_enabled=true;ui_fast_scroll_reset();
    ui_select_screen(SCR_SETTINGS);pump(600);settings_open((void *)(intptr_t)SETTING_FAST_SCROLL);pump(200);
    check(g_focus_count==13 && g_focus_index==0,"Fast Scroll must focus its top toggle, three trigger times, five thresholds and four hold times");
    check(!strcmp(lv_label_get_text(settings_fast_trigger_checks[0]),ICON_CHECK),
          "550 ms must be selected by default");
    check(lv_obj_get_index(lv_obj_get_parent(settings_fast_trigger_checks[0])) <
          lv_obj_get_index(lv_obj_get_parent(settings_fast_scroll_checks[0])),
          "Trigger Time choices must appear before the page thresholds");
    for(uint32_t i=0;i<lv_obj_get_child_count(settings_right_content);i++) {
        lv_obj_t *text=first_label(lv_obj_get_child(settings_right_content,i));
        check(!text || strcmp(lv_label_get_text(text),"Off"),"Threshold choices must not contain Off");
    }
    ui_handle_key(UI_KEY_ENTER);
    check(!g_system_config.fast_scroll_enabled && g_system_config.fast_scroll_pages==2 &&
          !strcmp(lv_label_get_text(settings_fast_scroll_enabled_value),"Disabled"),
          "The top control must disable Fast Scroll and preserve its threshold");
    for(int i=0;i<3;i++) {
        ui_handle_key(UI_KEY_DOWN);ui_handle_key(UI_KEY_ENTER);
        check(g_system_config.fast_scroll_trigger_window_ms==settings_fast_trigger_values[i] &&
              !g_system_config.fast_scroll_enabled,
              "Every Trigger Time choice must work while Fast Scroll is disabled");
        for(int j=0;j<3;j++)
            check(!strcmp(lv_label_get_text(settings_fast_trigger_checks[j]),j==i ? ICON_CHECK : ""),
                  "Only the chosen Trigger Time must have a checkmark");
    }
    ui_handle_key(UI_KEY_DOWN);ui_handle_key(UI_KEY_ENTER);
    check(g_system_config.fast_scroll_pages==1 && !g_system_config.fast_scroll_enabled,
          "A threshold can be changed while disabled without re-enabling Fast Scroll");
    ui_handle_key_repeat(UI_KEY_UP,4);ui_handle_key(UI_KEY_ENTER);
    check(g_system_config.fast_scroll_enabled && g_system_config.fast_scroll_pages==1 &&
          !strcmp(lv_label_get_text(settings_fast_scroll_enabled_value),"Enabled"),
          "Re-enabling must retain the chosen threshold");
    check(g_system_config.fast_scroll_trigger_window_ms==1200,"Re-enabling must preserve Trigger Time");
    /* Visit individual options without this navigation becoming a fast spin. */
    for(int i=0;i<9;i++) { pump(160);ui_handle_key(UI_KEY_DOWN); }
    for(int i=0;i<4;i++) {
        if(i) ui_handle_key(UI_KEY_DOWN);
        ui_handle_key(UI_KEY_ENTER);
        check(g_system_config.fast_scroll_hold_ms==settings_fast_hold_values[i],
              "Each Hold Time choice must update the persisted setting");
    }
    ui_handle_key(UI_KEY_BACK);ui_select_screen(SCR_NOW_PLAYING);pump(200);
    check(!g_fast_scroll.active && !g_fast_context,"Changing tabs must discard velocity history");
}

static void cursor_band_views(void)
{
    const screen_id_t views[]={SCR_ALBUMS,SCR_ARTISTS,SCR_SONGS,SCR_PLAYLISTS,SCR_SETTINGS};
    g_system_config.fast_scroll_enabled=true;g_system_config.fast_scroll_pages=2;
    g_system_config.fast_scroll_trigger_window_ms=550;g_system_config.fast_scroll_hold_ms=300;
    for(unsigned i=0;i<sizeof(views)/sizeof(views[0]);i++) {
        ui_select_screen(views[i]);pump(600);
        lv_obj_t *context;int rows=ui_fast_scroll_rows(&context);
        check(rows>0 && context,"Every list fixture must allow fast scrolling");
        uint32_t now=lv_tick_get();
        ui_handle_key_repeat_timed(UI_KEY_DOWN,rows*2,now-500,now);pump(40);
        check(g_fast_scroll.active,"Every list must enter fast scrolling");check_cursor_band(true);
        ui_fast_scroll_reset();check_cursor_band(false);
    }
    ui_select_screen(SCR_NOW_PLAYING);pump(200);
}

static void settings_widget_lifetime(void)
{
    settings_selected=SETTING_FAST_SCROLL;settings_detail_open=false;
    ui_select_screen(SCR_SETTINGS);pump(600);
    check(settings_fast_scroll_enabled_value!=NULL,"Fast Scroll preview must be rendered before leaving Settings");
    ui_select_screen(SCR_ALBUMS);
    check(settings_fast_scroll_enabled_value==NULL && settings_fast_trigger_checks[0]==NULL && settings_fast_scroll_checks[0]==NULL &&
          settings_fast_hold_checks[0]==NULL,"Leaving Settings must drop references to deleted detail widgets");
    pump(600);ui_select_screen(SCR_SETTINGS);pump(70);
    check(!g_page_loading && settings_detail_dirty && !settings_fast_scroll_enabled_value,
          "Returning to Settings must wait for its deferred detail render with no stale labels");
    settings_tick();pump(400);
    check(settings_fast_scroll_enabled_value && !settings_detail_dirty &&
          !strcmp(lv_label_get_text(settings_fast_scroll_enabled_value),"Enabled"),
          "Fast Scroll preview must refresh normally after returning to Settings");
    lv_obj_send_event(lv_obj_get_parent(settings_fast_trigger_checks[1]),LV_EVENT_CLICKED,NULL);
    pump(100);
    check(g_system_config.fast_scroll_trigger_window_ms==800 &&
          !strcmp(lv_label_get_text(settings_fast_trigger_checks[1]),ICON_CHECK),
          "The preview Trigger Time choices must also work through touch callbacks");
}

static void rapid_tab_navigation(void)
{
    ui_select_screen(SCR_NOW_PLAYING);pump(600);
    for(int round=0;round<12;round++) {
        for(int step=0;step<6;step++) {
            ui_handle_key(UI_KEY_SCREEN_NEXT);pump(40+(round%4)*50);
        }
        for(int step=0;step<8;step++) {
            ui_handle_key(UI_KEY_SCREEN_PREV);pump(40+(round%4)*50);
        }
        check(g_current_screen==SCR_NOW_PLAYING,"Repeated previous-tab input must return to Now Playing");
        for(int step=0;step<6;step++) {
            ui_handle_key(UI_KEY_SCREEN_PREV);pump(30+step*20);
        }
        ui_handle_key(UI_KEY_BACK);pump(200);
    }
    ui_select_screen(SCR_ALBUMS);pump(600);
    check(!g_page_loading && albums_left_list && albums_right_content,
          "Albums must remain usable after rapid tab and queue transitions");
    ui_select_screen(SCR_NOW_PLAYING);pump(600);
}

static ui_system_config_t saved_order;
static int order_saves;
static void save_order(const ui_system_config_t *config)
{ saved_order=*config; order_saves++; }

static void sort_order_settings(void)
{
    int original_field=g_sort_field;
    bool original_desc=g_sort_desc;
    ui_set_system_config_changed_cb(save_order);
    ui_select_screen(SCR_SETTINGS); pump(600);
    settings_open((void *)(intptr_t)SETTING_SORT_ORDER); pump(200);
    check(g_focus_count==4 && g_focus_index==0 && settings_detail_sort_value && settings_detail_order_value &&
          settings_detail_song_sort_value && settings_detail_song_order_value,
          "Sort Order must expose independent album and song controls to the knob");
    check(!strcmp(settings_names[SETTING_SORT_ORDER],"Sort Order") &&
          !strcmp(settings_sort_field_name(SORT_FIELD_ADD_DATE),"Date Added"),
          "Sort Order must use the standard Date Added label");
    check(!strcmp(lv_label_get_text(first_label(lv_obj_get_parent(settings_detail_sort_value))),"Albums: Sort By") &&
          !strcmp(lv_label_get_text(first_label(lv_obj_get_parent(settings_detail_order_value))),"Albums: Order") &&
          !strcmp(lv_label_get_text(first_label(lv_obj_get_parent(settings_detail_song_sort_value))),"Songs: Sort By") &&
          !strcmp(lv_label_get_text(first_label(lv_obj_get_parent(settings_detail_song_order_value))),"Songs: Order"),
          "Sort Order must label all four independent controls");
    for(int i=0;i<SORT_FIELD_COUNT;i++) {
        ui_handle_key(UI_KEY_ENTER); pump(100);
        check(g_sort_field==(original_field+i+1)%SORT_FIELD_COUNT &&
              saved_order.sort_field==g_sort_field && order_saves==i+1,
              "Albums Sort By must cycle through Date Added, Name and Year and persist each choice");
    }
    ui_handle_key(UI_KEY_DOWN); ui_handle_key(UI_KEY_ENTER); pump(100);
    check(g_sort_desc!=original_desc && saved_order.sort_desc==g_sort_desc &&
          !strcmp(lv_label_get_text(settings_detail_order_value),settings_sort_order_text()),
          "Order must toggle and persist Ascending or Descending");
    int song_field=g_song_sort_field;
    bool song_desc=g_song_sort_desc;
    ui_handle_key(UI_KEY_DOWN);
    for(int i=0;i<SORT_FIELD_COUNT;i++) {
        ui_handle_key(UI_KEY_ENTER);pump(100);
        check(g_song_sort_field==(song_field+i+1)%SORT_FIELD_COUNT &&
              saved_order.song_sort_field==g_song_sort_field && g_sort_field==original_field &&
              g_sort_desc!=original_desc,
              "Songs Sort By must persist independently of Albums");
    }
    ui_handle_key(UI_KEY_DOWN);ui_handle_key(UI_KEY_ENTER);pump(100);
    check(g_song_sort_desc!=song_desc && saved_order.song_sort_desc==g_song_sort_desc &&
          saved_order.sort_desc==g_sort_desc,
          "Songs Order must persist independently of Albums");
    ui_select_screen(SCR_ALBUMS);
    check(!settings_detail_sort_value && !settings_detail_order_value &&
          !settings_detail_song_sort_value && !settings_detail_song_order_value,
          "Leaving Settings must clear the deleted order controls");
    pump(600); ui_select_screen(SCR_SETTINGS); pump(600);
    check(settings_detail_order_value && settings_detail_sort_value &&
          settings_detail_song_sort_value && settings_detail_song_order_value,
          "Returning to Settings must rebuild the saved order controls");
    lv_obj_send_event(lv_obj_get_parent(settings_detail_order_value),LV_EVENT_CLICKED,NULL); pump(100);
    check(g_sort_desc==original_desc && saved_order.sort_desc==original_desc,
          "Order must also work through the touch preview");
    lv_obj_send_event(lv_obj_get_parent(settings_detail_song_order_value),LV_EVENT_CLICKED,NULL);pump(100);
    check(g_song_sort_desc==song_desc && saved_order.song_sort_desc==song_desc,
          "Songs Order must also work through the touch preview");
    ui_set_system_config_changed_cb(NULL);
    ui_select_screen(SCR_NOW_PLAYING); pump(600);
}

static void live_album_focus(void)
{
    ui_select_screen(SCR_NOW_PLAYING);pump(100);
    albums_visited=false;albums_selected=0;albums_left_scroll_y=0;
    g_sort_field=0;g_sort_desc=true;
    player_library_reset();
    for(int i=1;i<=2;i++) {
        player_media_item_t item={.pid=(uint64_t)i,.album_pid=1000u+i,
            .fields=(1u<<0)|(1u<<1)|(1u<<5)|(1u<<6)|(1u<<12)|(1u<<14)};
        snprintf(item.title,sizeof(item.title),"Song %d",i);
        snprintf(item.album,sizeof(item.album),"%c album",'A'+i-1);
        strcpy(item.artist,"Artist");strcpy(item.album_artist,"Artist");
        player_library_update(&item);
    }
    player_library_complete();pump(200);
    check(albums_selected==0,"An unvisited Albums page must keep its initial top selection");
    ui_select_screen(SCR_ALBUMS);pump(200);albums_select(1);pump(200);
    player_album_t selected;check(player_album_get(albums_selected,&selected),"Selected fixture album exists");
    uint64_t selected_id=selected.id;
    ui_select_screen(SCR_NOW_PLAYING);pump(100);
    player_media_item_t item={.pid=3,.album_pid=1003,
        .fields=(1u<<0)|(1u<<1)|(1u<<5)|(1u<<6)|(1u<<12)|(1u<<14)};
    strcpy(item.title,"New song");strcpy(item.album,"New album");
    strcpy(item.artist,"Artist");strcpy(item.album_artist,"Artist");
    player_library_update(&item);pump(200);
    check(player_album_get(albums_selected,&selected) && selected.id==selected_id &&
          albums_selected==2 && albums_left_scroll_y>=ALBUMS_STRIDE,
          "An offscreen catalog update must preserve the album PID and scroll position");
    ui_select_screen(SCR_ALBUMS);pump(200);
    albums_open((void*)(intptr_t)albums_selected);pump(200);
    for(int i=0;i<g_focus_count;i++) if(g_focus[i].activate==albums_track_action) {
        focus_select(i);break;
    }
    item.pid=4;item.album_pid=1004;strcpy(item.album,"Newest album");
    player_library_update(&item);pump(200);
    check(player_album_get(albums_selected,&selected) && selected.id==selected_id &&
          g_focus_index>=0 && g_focus[g_focus_index].activate==albums_track_action,
          "Live updates must preserve the album detail track focus");
}

static void live_song_focus(void)
{
    g_song_sort_field=SORT_FIELD_NAME;g_song_sort_desc=false;
    ui_select_screen(SCR_SONGS);pump(300);songs_select(0);pump(100);
    player_track_t selected;check(player_song_get(songs_selected,&selected),"Selected fixture song exists");
    uint64_t selected_id=selected.id;
    ui_select_screen(SCR_SETTINGS);pump(200);
    settings_sort_order_action((void*)(intptr_t)1);pump(200);
    ui_select_screen(SCR_SONGS);pump(300);
    check(player_song_get(songs_selected,&selected) && selected.id==selected_id &&
          songs_selected!=0 && songs_selected_pid==selected_id,
          "Changing song order offscreen must retain the selected PID");
    player_media_item_t update={.pid=selected_id,.fields=1u<<1};
    strcpy(update.title,"ZZZZ renamed song");player_library_update(&update);pump(300);
    check(player_song_get(songs_selected,&selected) && selected.id==selected_id &&
          songs_selected==0 && songs_selected_pid==selected_id,
          "A live song update must rebuild the sorted view and preserve selection");
}

int main(int argc,char **argv)
{
    lv_init();lv_display_t *display=lv_display_create(1280,720);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    ui_system_config_t config={.theme=argc>1?atoi(argv[1]):0,.brightness=45,.menubar_left=1,
        .fast_scroll_enabled=true,.fast_scroll_pages=2,.sort_desc=1,.song_sort_desc=1};
    ui_apply_saved_settings(&config);ui_init();pump(600);
    check(g_system_config.fast_scroll_hold_ms==300,"Missing hold configuration must default to 300 ms");
    check(g_system_config.fast_scroll_trigger_window_ms==550,"Missing trigger configuration must default to 550 ms");
    alpha_fade();artist_viewport();scrollbar_views();pressed_controls();fast_navigation();
    cursor_band_views();settings_widget_lifetime();rapid_tab_navigation();sort_order_settings();
    live_album_focus();
    live_song_focus();
    puts("PASS: transparent text fade, Artists viewport restoration, column scrollbar geometry, page scrolling and theme cursor feedback");
    return 0;
}
