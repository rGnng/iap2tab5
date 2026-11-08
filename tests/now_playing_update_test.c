/* Production LVGL widgets with injected device snapshots. Including the UI
 * keeps its private widgets private in normal builds, while letting this test
 * observe renders and object lifetimes without a separate test-only API. */
#include "player.h"
static void test_snapshot(player_snapshot_t *out);
static uint32_t test_queue_revision(void);
static uint32_t test_artwork_revision(void);
static int test_queue_count(void);
static bool test_artwork_copy(uint8_t *dst, size_t size, artwork_quality_t minimum,
                              artwork_quality_t *actual);
static bool test_song_get(int index, player_track_t *out);
static bool test_artist_song_get(uint64_t id, int index, player_track_t *out);
#define player_get_snapshot test_snapshot
#define player_queue_revision test_queue_revision
#define player_artwork_revision test_artwork_revision
#define player_queue_count test_queue_count
#define player_artwork_copy test_artwork_copy
#define player_song_get test_song_get
#define player_artist_song_get test_artist_song_get
#include "../app/ui/ui.c"
#undef player_get_snapshot
#undef player_queue_revision
#undef player_artwork_revision
#undef player_queue_count
#undef player_artwork_copy
#undef player_song_get
#undef player_artist_song_get
#include <unistd.h>

static player_snapshot_t injected;
static bool inject, art_ready;
static unsigned art_variant;
static artwork_quality_t art_quality={360,360};
static uint32_t queue_rev, art_rev;
static unsigned renders, deleted, copies;
static unsigned global_song_reads, member_song_reads;
static uint32_t observed_background;
static bool force_empty_queue;
static const int shell_points[4][2]={{1,1},{4,270},{1278,1},{1276,270}};
static uint32_t observed_shell[4];
static uint8_t pixels[1280 * 40 * 4];
static void test_snapshot(player_snapshot_t *out)
{ if(inject) *out=injected; else player_get_snapshot(out); }
static uint32_t test_queue_revision(void)
{ return player_queue_revision()+queue_rev; }
static uint32_t test_artwork_revision(void)
{ return art_rev; }
static int test_queue_count(void)
{ return force_empty_queue?0:player_queue_count(); }
static bool test_artwork_copy(uint8_t *dst, size_t size, artwork_quality_t minimum,
                              artwork_quality_t *actual)
{
    copies++;
    *actual=art_ready?art_quality:(artwork_quality_t){0};
    if(!artwork_quality_at_least(*actual,minimum)) return false;
    if(art_ready) {
        uint16_t *pixels=(uint16_t *)dst;
        const uint16_t colors[]={0xf800,0x001f,0x07e0,0xf81f};
        for(size_t i=0;i<size/2;i++) {
            unsigned x=i%360, y=i/360;
            pixels[i]=colors[((x>=180)+2*(y>=180)+art_variant)%4];
        }
    }
    return art_ready;
}
static bool test_song_get(int index, player_track_t *out)
{ global_song_reads++; return player_song_get(index,out); }
static bool test_artist_song_get(uint64_t id, int index, player_track_t *out)
{ member_song_reads++; return player_artist_song_get(id,index,out); }
static void check(bool condition, const char *message)
{ if(!condition) { fprintf(stderr,"FAIL: %s\n",message); exit(1); } }
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    for(unsigned i=0;i<4;i++) {
        int x=shell_points[i][0],y=shell_points[i][1];
        if(a->x1<=x && a->x2>=x && a->y1<=y && a->y2>=y) {
            size_t offset=(size_t)(y-a->y1)*lv_area_get_width(a)+(x-a->x1);
            memcpy(&observed_shell[i],p+offset*4,4);
        }
    }
    if(a->x1<=1100 && a->x2>=1100 && a->y1<=695 && a->y2>=695) {
        size_t offset=(size_t)(695-a->y1)*lv_area_get_width(a)+(1100-a->x1);
        memcpy(&observed_background,p+offset*4,4);
    }
    lv_display_flush_ready(d);
}
static void rendered(lv_event_t *e) { LV_UNUSED(e); renders++; }
static void destroyed(lv_event_t *e) { LV_UNUSED(e); deleted++; }
static void pump(int ms)
{ for(int i=0;i<ms;i+=10) { lv_tick_inc(10); lv_timer_handler(); } }
static void text(lv_obj_t *o, const char *value)
{ check(o && !strcmp(lv_label_get_text(o),value),value); }

static void check_shell_tint(unsigned index)
{
    const uint16_t *image=(const uint16_t *)g_np_mesh_dsc.data;
    uint16_t pixel=image[shell_points[index][1]*COVER_GRADIENT_WIDTH+shell_points[index][0]];
    unsigned r=pixel>>11,g=(pixel>>5)&63,b=pixel&31;
    uint32_t background=(r*255/31)<<16|(g*255/63)<<8|(b*255/31);
    for(unsigned shift=0;shift<24;shift+=8) {
        unsigned expected=(((background>>shift)&255)*(255-LV_OPA_10)+
            ((COL_MENU_BODY_BG>>shift)&255)*LV_OPA_10)/255;
        check(abs((int)((observed_shell[index]>>shift)&255)-(int)expected)<=2,
              "Sidebar and clock pixels must show the shared gradient with only the faint theme tint");
    }
}

static void check_mode_tints(void)
{
    bool shuffle=injected.shuffle;
    player_repeat_t repeat=injected.repeat;
    for(int active=0;active<2;active++) {
        injected.shuffle=active;
        injected.repeat=active?PLAYER_REPEAT_ONE:PLAYER_REPEAT_OFF;
        injected.revision++;
        np_tick();pump(300);
        ui_mini_update();
        check(lv_color_eq(lv_obj_get_style_text_color(g_mini_shuffle,0),
                lv_color_hex(active?COL_MINI_FG:COL_DIM)) &&
              lv_color_eq(lv_obj_get_style_text_color(g_mini_repeat,0),
                lv_color_hex(active?COL_MINI_FG:COL_DIM)),
              "Mini-player mode icons must share the dim inactive appearance and retain bright active colors");
        lv_obj_t *buttons[2]={np_shuffle_btn,np_repeat_btn};
        for(unsigned i=0;i<2;i++) {
            lv_opa_t expected=active?LV_OPA_20:
                (g_theme->id==UI_THEME_GRAY?LV_OPA_TRANSP:(LV_OPA_10 / 2));
            check(lv_obj_get_style_bg_opa(buttons[i],0)==expected,
                  "Shuffle and repeat backgrounds must remain faint in both active and inactive states");
            check(lv_color_eq(lv_obj_get_style_text_color(lv_obj_get_child(buttons[i],0),0),
                    lv_color_hex(active?COL_ACCENT:COL_DIM)),
                  "Mode icons need contrasting theme colors rather than translucent or white selection text");
            lv_obj_add_state(buttons[i],LV_STATE_PRESSED);
            pump(300);
            check(lv_obj_get_style_bg_opa(buttons[i],0)==LV_OPA_30,
                  "Pressed mode controls must still show the gradient through their backgrounds");
            lv_obj_remove_state(buttons[i],LV_STATE_PRESSED);
            pump(300);
        }
    }
    injected.shuffle=shuffle;injected.repeat=repeat;injected.revision++;
    np_tick();ui_mini_update();pump(300);
}

static void gradient_lifecycle(void)
{
    settings_np_bg_action((void *)(intptr_t)NP_BG_COVER_GRADIENT);
    injected.track.id+=100000; art_ready=false; art_rev++;
    np_tick(); pump(200);
    check(np_gradient_backdrop && lv_obj_is_hidden(np_gradient_backdrop) && np_gradient_key==0,
          "Missing artwork must leave the theme visible while reserving a gradient widget");
    lv_obj_t *title=np_title;
    art_ready=true; art_rev++; np_tick(); pump(200);
    check(np_title==title,"Late artwork must not rebuild the page");
    check(np_gradient_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "Live RGB565 artwork must generate a visible gradient");
    check(lv_obj_get_parent(np_gradient_backdrop)==lv_screen_active() &&
          lv_obj_get_index(np_gradient_backdrop)==0,
          "One gradient image must sit behind the page and menu");
    check(np_chrome_tinted && lv_obj_get_style_bg_opa(g_page,0)==LV_OPA_TRANSP &&
          lv_obj_get_style_bg_opa(g_menubar,0)==LV_OPA_10 &&
          lv_obj_get_style_bg_opa(lv_obj_get_parent(g_status_time),0)==LV_OPA_TRANSP,
          "Ready gradients must make the shell translucent without a second clock backdrop");
    check_shell_tint(0);check_shell_tint(1);
    check(lv_obj_get_style_bg_opa(np_shuffle_btn,0)==(injected.shuffle?LV_OPA_20:
            (g_theme->id==UI_THEME_GRAY?LV_OPA_TRANSP:(LV_OPA_10 / 2))) &&
          lv_obj_get_style_bg_opa(np_repeat_btn,0)==(injected.repeat!=PLAYER_REPEAT_OFF?LV_OPA_20:
            (g_theme->id==UI_THEME_GRAY?LV_OPA_TRANSP:(LV_OPA_10 / 2))),
          "Late artwork must restyle existing mode controls even without a playback-state change");
    check(lv_obj_get_style_bg_opa(g_menu_items[SCR_NOW_PLAYING],0)==LV_OPA_20,
          "The selected Now Playing menu item must retain only a translucent tint");
    check(lv_color_eq(lv_obj_get_style_text_color(g_menu_icons[SCR_NOW_PLAYING],0),
            lv_color_hex(g_theme->id==UI_THEME_GRAY?COL_MENU_TOP_FG:COL_ACCENT)),
          "The selected menu icon needs a contrasting foreground over its translucent tint");
    ui_menu_update();
    check(lv_obj_get_style_bg_opa(g_menu_items[SCR_NOW_PLAYING],0)==LV_OPA_20,
          "Playback/menu refresh must not restore an opaque selection background");
    check_mode_tints();
    uint64_t original_key=np_gradient_key;
    const uint8_t *original_pixels=g_np_mesh_dsc.data;
    const uint16_t *pixels=(const uint16_t *)original_pixels;
    lv_area_t bounds; lv_obj_get_coords(np_gradient_backdrop,&bounds);
    check(bounds.x1==0 && bounds.y1==0 && bounds.x2==1279 && bounds.y2==719,
          "The gradient must cover the entire screen without content-pane clipping");
    uint16_t expected=pixels[(695-bounds.y1)*COVER_GRADIENT_WIDTH+1100-bounds.x1];
    unsigned r=expected>>11, g=(expected>>5)&63, b=expected&31;
    check(abs((int)((observed_background>>16)&255)-(int)((r<<3)|(r>>2)))<=1 &&
          abs((int)((observed_background>>8)&255)-(int)((g<<2)|(g>>4)))<=1 &&
          abs((int)(observed_background&255)-(int)((b<<3)|(b>>2)))<=1,
          "Cached tint and grain must reach the screen without a second overlay");
    check(pixels[0]!=pixels[(COVER_GRADIENT_HEIGHT/2)*COVER_GRADIENT_WIDTH+COVER_GRADIENT_WIDTH/2],
          "Colored artwork must produce a varying background");
    unsigned before=renders;
    for(int i=0;i<10;i++) np_tick();
    lv_refr_now(lv_display_get_default());
    check(renders==before,"A cached gradient must not keep invalidating the paused screen");
    art_rev++; np_tick(); pump(100);
    check(np_gradient_key==original_key && g_np_mesh_dsc.data==original_pixels,
          "Identical artwork revisions must reuse the existing background");
    art_variant++; art_rev++; np_tick(); pump(100);
    check(np_title==title && np_gradient_key!=original_key &&
          !lv_obj_is_hidden(np_gradient_backdrop),"Changed artwork must update the background in place");
    uint64_t changed_key=np_gradient_key;
    force_empty_queue=true;
    np_toggle_queue(); pump(300);
    text(np_queue_loading,"Queue empty");
    check(lv_obj_get_style_bg_opa(lv_obj_get_parent(np_queue_list),0)==LV_OPA_10,
          "The queue panel must have only a faint theme-colored tint");
    check(np_gradient_key==changed_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "Up Next must reuse the same gradient");
    force_empty_queue=false;np_toggle_queue(); pump(300);
    ui_select_screen(SCR_ALBUMS); pump(400);
    check(!np_gradient_backdrop && !np_gradient_key,"Page exit must detach gradient widgets");
    check(!np_chrome_tinted && lv_obj_get_style_bg_opa(g_page,0)==LV_OPA_COVER &&
          lv_obj_get_style_bg_opa(g_menubar,0)==LV_OPA_COVER &&
          lv_obj_get_style_bg_opa(lv_obj_get_parent(g_status_time),0)==LV_OPA_COVER,
          "Leaving Now Playing must restore the normal page, menu and clock backgrounds");
    check(lv_obj_get_style_bg_opa(g_menu_items[SCR_ALBUMS],0)==LV_OPA_COVER,
          "Other views must retain their ordinary opaque menu selection");
    np_gradient_poll_cb(NULL);
    check(g_current_screen==SCR_ALBUMS,"A late background result must not affect another tab");
    ui_select_screen(SCR_NOW_PLAYING); pump(300);
    check(np_gradient_key==changed_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "Returning to Now Playing must use the cached artwork background");
    ui_set_menubar_left(false);pump(200);
    check_shell_tint(2);check_shell_tint(3);
    check(np_gradient_key==changed_key,"Moving the menu must not regenerate the background");
    ui_set_menubar_left(true);pump(200);
    int original_theme=g_theme->id;
    g_system_config.theme=(original_theme+1)%UI_THEME_COUNT;ui_apply_theme();pump(200);
    check_shell_tint(0);check_shell_tint(1);
    check_mode_tints();
    g_system_config.theme=original_theme;ui_apply_theme();pump(200);
    check_shell_tint(0);check_shell_tint(1);
    settings_np_bg_action((void *)(intptr_t)NP_BG_THEME); ui_rebuild_page(); pump(200);
    check(!np_gradient_backdrop && !np_gradient_key,"Theme mode must omit the cover gradient");
    check(!np_chrome_tinted,"Theme mode must retain the ordinary opaque shell");
    check(lv_obj_get_style_bg_opa(g_menu_items[SCR_NOW_PLAYING],0)==LV_OPA_COVER,
          "Theme background mode must retain its ordinary menu selection");
    ui_system_config_t retired=g_system_config;retired.np_bg_mode=3;
    ui_apply_saved_settings(&retired);ui_rebuild_page();pump(200);
    check(g_np_bg_mode==NP_BG_COVER_GRADIENT && g_system_config.np_bg_mode==2 &&
          settings_initial_focus(SETTING_NOW_PLAYING_BG)==1,
          "The retired album mode must migrate to the remaining Cover gradient choice");
    inject=false;
    settings_np_bg_action((void *)(intptr_t)NP_BG_COVER_GRADIENT); ui_rebuild_page(); pump(200);
    check(np_gradient_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "PNG/demo covers must still support gradients through the shared pipeline");
    settings_np_bg_action((void *)(intptr_t)NP_BG_THEME); ui_rebuild_page(); pump(100);
}

static void artwork_upgrades(void)
{
    injected.track.id+=200000; art_ready=true; art_quality=(artwork_quality_t){120,120};
    art_rev++; np_tick(); pump(200);
    check(g_iap2_cover_quality.width==120,"A thumbnail must display immediately when it is all we have");
    uint16_t before=((uint16_t *)g_iap2_cover_buf)[0];
    art_quality=(artwork_quality_t){600,600}; art_variant++; art_rev++; np_tick(); pump(200);
    check(g_iap2_cover_quality.width==600 && ((uint16_t *)g_iap2_cover_buf)[0]!=before,
          "A later larger source must upgrade the foreground cover without a track change");
    before=((uint16_t *)g_iap2_cover_buf)[0];
    art_quality=(artwork_quality_t){120,120}; art_variant++; art_rev++; np_tick(); pump(200);
    check(g_iap2_cover_quality.width==600 && ((uint16_t *)g_iap2_cover_buf)[0]==before,
          "A later thumbnail must not overwrite a sharper cover");

    char directory[]="/tmp/iap2tab5-artwork-XXXXXX";
    check(mkdtemp(directory)!=NULL,"Create isolated artwork cache");
    ui_set_np_cache_dir(directory);
    uint64_t id=injected.track.id+1;
    char path[192]; np_cache_path(id,path,sizeof(path),false);
    FILE *f=fopen(path,"wb"); check(f!=NULL,"Open cache file");
    check(np_cache_write(f,id,g_iap2_cover_buf,g_iap2_cover_quality),"Persist pixels and source quality");
    fclose(f);
    uint8_t *loaded=malloc(NP_ART_BYTES); check(loaded!=NULL,"Allocate cache test buffer");
    artwork_quality_t quality;
    check(np_cache_load(id,loaded,&quality) && quality.width==600 && quality.height==600 &&
          !memcmp(loaded,g_iap2_cover_buf,NP_ART_BYTES),"Source resolution must survive cache roundtrip");
    injected.track.id=id; art_rev++; np_tick(); pump(200);
    check(g_iap2_cover_quality.width==600 && !memcmp(loaded,g_iap2_cover_buf,NP_ART_BYTES),
          "Revisiting a cached track must retain its sharper cover over a live thumbnail");
    g_iap2_cover_track_id=0; g_iap2_cover_ready=false;
    g_iap2_cover_quality=(artwork_quality_t){0};
    check(iap2_cover_src(id)!=NULL && g_iap2_cover_quality.width==600,
          "A cache restored after memory reset must still reject smaller artwork");

    f=fopen(path,"wb"); check(f!=NULL,"Open legacy cache file");
    fwrite(&id,1,sizeof(id),f); fwrite(loaded,1,NP_ART_BYTES,f); fclose(f);
    check(np_cache_load(id,loaded,&quality) && quality.width==0 && quality.height==0,
          "Legacy caches without source dimensions must remain readable");
    f=fopen(path,"wb"); check(f!=NULL,"Open truncated cache file");
    fwrite(&id,1,sizeof(id),f); fwrite(loaded,1,NP_ART_BYTES-1,f); fclose(f);
    check(!np_cache_load(id,loaded,&quality),"Incomplete image caches must not be displayed");
    f=fopen(path,"wb"); check(f!=NULL,"Open mismatched cache file");
    check(np_cache_write(f,id+1,loaded,(artwork_quality_t){600,600}),"Write mismatched ID"); fclose(f);
    check(!np_cache_load(id,loaded,&quality),"A different track's cache must not pass the ID check");
    free(loaded); unlink(path); rmdir(directory); ui_set_np_cache_dir("/sdcard/np");
    art_quality=(artwork_quality_t){360,360};
}

static void artist_membership(void)
{
    inject=false;
    /* Pick an artist with interleaved global offsets and multiple albums. */
    player_artist_t artist;
    int selected=-1;
    for(int i=1;i<player_artist_count();i++) {
        player_artist_get(i,&artist);
        if(artist.album_count>=2 && artist.song_count>3) { selected=i; break; }
    }
    check(selected>=0,"Mock fixture needs an artist with multiple albums");
    int n=player_song_count(), count=0, groups=0;
    player_track_t *members=malloc((size_t)n*sizeof(*members));
    uint64_t *albums=malloc((size_t)n*sizeof(*albums));
    uint64_t *expected=malloc((size_t)n*sizeof(*expected));
    check(members && albums && expected,"Artist test buffers must allocate");
    for(int i=0;i<n;i++) {
        player_track_t track; player_song_get(i,&track);
        if(track.artist_id!=artist.id) continue;
        members[count++]=track;
        int a=0; while(a<groups && albums[a]!=track.album_id) a++;
        if(a==groups) albums[groups++]=track.album_id;
    }
    int rows=0;
    for(int a=0;a<groups;a++) {
        for(int i=0;i<count;i++) {
            if(members[i].album_id==albums[a]) expected[rows++]=members[i].id;
        }
    }
    artists_selected=selected;
    global_song_reads=member_song_reads=0;
    ui_select_screen(SCR_ARTISTS); pump(800);
    check(global_song_reads==0 && member_song_reads>0,"Artist preview must use membership index without global scans");
    artists_open((void *)(intptr_t)selected); pump(200);
    check(global_song_reads==0,"Opened artist must not scan all library tracks");
    rows=0; void *last_user=NULL;
    for(int i=0;i<g_focus_count;i++) {
        if(g_focus[i].activate!=artists_track_action) continue;
        player_track_t track;
        check(player_artist_song_get(artist.id,(int)(intptr_t)g_focus[i].user,&track),"Artist row must resolve to a member");
        check(rows<count && track.id==expected[rows++],"Artist album grouping and song order must remain unchanged");
        last_user=g_focus[i].user;
    }
    check(rows==count,"Opened artist must show every member");
    artists_track_action(last_user);
    player_snapshot_t snapshot; player_get_snapshot(&snapshot);
    check(snapshot.track.id==expected[count-1],"Artist row action must play the selected song");
    free(members); free(albums); free(expected);
}

int main(int argc, char **argv)
{
    lv_init();
    lv_display_t *d=lv_display_create(1280,720);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    lv_display_add_event_cb(d,rendered,LV_EVENT_RENDER_START,NULL);
    ui_system_config_t cfg={.theme=argc>1?atoi(argv[1]):0,.brightness=40,.menubar_left=1};
    ui_apply_saved_settings(&cfg); ui_init(); pump(600);
    player_get_snapshot(&injected);
    if(injected.status==PLAYER_PLAYING) player_play_pause();
    injected.status=PLAYER_PAUSED;
    strcpy(injected.track.title,"Performance test");
    strcpy(injected.track.artist,"Artist");
    strcpy(injected.track.album,"Album");
    injected.elapsed_sec=72; injected.duration_sec=240;
    inject=true; np_tick(); pump(600);
    unsigned before=renders;
    for(int i=0;i<20;i++) { np_tick(); pump(100); }
    printf("Unchanged paused snapshot: %u renders in 20 ticks\n",renders-before);
    check(renders==before,"Unchanged paused metadata must not schedule renders");

    lv_obj_add_event_cb(np_title,destroyed,LV_EVENT_DELETE,NULL);
    queue_rev++; np_tick(); pump(100);
    check(deleted==0,"Hidden queue updates must not rebuild Now Playing");
    check(np_last_queue_rev==test_queue_revision(),"Hidden queue revision must still advance");

    /* Device metadata can arrive piecemeal with unchanged PID and revision. */
    strcpy(injected.track.artist,"New artist");
    strcpy(injected.track.album,"New album");
    np_tick(); text(np_artist,"New artist"); text(np_album,"New album");
    injected.track.title[0]='\0'; np_tick();
    check(lv_obj_has_flag(np_title,LV_OBJ_FLAG_HIDDEN) &&
          lv_obj_has_flag(np_star_btn,LV_OBJ_FLAG_HIDDEN),"Empty title must hide title and star");
    text(np_artist,""); text(np_album,"");
    strcpy(injected.track.title,"Restored"); np_tick();
    check(!lv_obj_has_flag(np_title,LV_OBJ_FLAG_HIDDEN) &&
          !lv_obj_has_flag(np_star_btn,LV_OBJ_FLAG_HIDDEN),"Late title must restore controls");
    text(np_artist,"New artist"); text(np_album,"New album");

    injected.elapsed_sec=73; np_tick();
    text(np_time_cur,"1:13"); check(lv_bar_get_value(np_progress)==73,"Progress must advance");
    injected.duration_sec=60; np_tick();
    text(np_time_dur,"1:00"); check(lv_bar_get_value(np_progress)==60,"Shorter duration must clamp progress");
    injected.duration_sec=240; np_tick();
    check(lv_bar_get_value(np_progress)==73,"Longer duration must restore unchanged elapsed value");
    injected.status=PLAYER_PLAYING; injected.shuffle=true;
    injected.repeat=PLAYER_REPEAT_ONE; injected.starred=true; np_tick();
    text(np_play_icon,ICON_PAUSE); text(np_repeat_icon,ICON_REPEAT_ONE); text(np_star_icon,ICON_STAR_FILL);
    check(lv_color_eq(lv_obj_get_style_text_color(np_shuffle_icon,0),
          lv_color_hex(g_theme->id==UI_THEME_GRAY?COL_ACCENT:g_theme->on_selection)),"Shuffle must highlight");

    injected.real_device=true; art_rev++; np_tick();
    unsigned old_copies=copies;
    art_ready=true; art_rev++; np_tick();
    check(copies>old_copies && lv_image_get_src(np_cover_widget)!=NULL,
          "Late artwork must update independently of snapshot metadata");
    old_copies=copies; np_tick(); check(copies==old_copies,"Unchanged artwork must not be copied again");

    np_toggle_queue(); pump(600);
    text(np_meta,"New artist - New album");
    text(np_play_icon,ICON_PAUSE); text(np_star_icon,ICON_STAR_FILL);
    strcpy(injected.track.album,"Queue album"); np_tick(); text(np_meta,"New artist - Queue album");
    injected.track.artist[0]='\0'; np_tick(); text(np_meta,"Queue album");
    lv_obj_add_event_cb(np_title,destroyed,LV_EVENT_DELETE,NULL);
    unsigned old_deleted=deleted; queue_rev++; np_tick(); pump(600);
    check(deleted==old_deleted+1,"Visible queue changes must refresh queue rows");
    np_toggle_queue(); pump(600);
    text(np_album,"Queue album"); text(np_star_icon,ICON_STAR_FILL);
    artwork_upgrades();
    gradient_lifecycle();
    artist_membership();
    puts("PASS: selective updates, delayed metadata/artwork, progress and queue visibility");
    return 0;
}
