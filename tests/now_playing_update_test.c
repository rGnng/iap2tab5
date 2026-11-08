/* Production LVGL widgets with injected device snapshots. Including the UI
 * keeps its private widgets private in normal builds, while letting this test
 * observe renders and object lifetimes without a separate test-only API. */
#include "player.h"
static void test_snapshot(player_snapshot_t *out);
static uint32_t test_queue_revision(void);
static uint32_t test_artwork_revision(void);
static bool test_artwork_copy(uint8_t *dst, size_t size);
static bool test_song_get(int index, player_track_t *out);
static bool test_artist_song_get(uint64_t id, int index, player_track_t *out);
#define player_get_snapshot test_snapshot
#define player_queue_revision test_queue_revision
#define player_artwork_revision test_artwork_revision
#define player_artwork_copy test_artwork_copy
#define player_song_get test_song_get
#define player_artist_song_get test_artist_song_get
#include "../app/ui/ui.c"
#undef player_get_snapshot
#undef player_queue_revision
#undef player_artwork_revision
#undef player_artwork_copy
#undef player_song_get
#undef player_artist_song_get

static player_snapshot_t injected;
static bool inject, art_ready;
static unsigned art_variant;
static uint32_t queue_rev, art_rev;
static unsigned renders, deleted, copies;
static unsigned global_song_reads, member_song_reads;
static uint8_t pixels[1280 * 40 * 4];
static void test_snapshot(player_snapshot_t *out)
{ if(inject) *out=injected; else player_get_snapshot(out); }
static uint32_t test_queue_revision(void)
{ return player_queue_revision()+queue_rev; }
static uint32_t test_artwork_revision(void)
{ return art_rev; }
static bool test_artwork_copy(uint8_t *dst, size_t size)
{
    copies++;
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
{ LV_UNUSED(a); LV_UNUSED(p); lv_display_flush_ready(d); }
static void rendered(lv_event_t *e) { LV_UNUSED(e); renders++; }
static void destroyed(lv_event_t *e) { LV_UNUSED(e); deleted++; }
static void pump(int ms)
{ for(int i=0;i<ms;i+=10) { lv_tick_inc(10); lv_timer_handler(); } }
static void text(lv_obj_t *o, const char *value)
{ check(o && !strcmp(lv_label_get_text(o),value),value); }

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
    check(np_gradient_key && !lv_obj_is_hidden(np_gradient_backdrop) &&
          !lv_obj_is_hidden(np_gradient_scrim),"Live RGB565 artwork must generate a visible gradient");
    uint64_t original_key=np_gradient_key;
    const uint8_t *original_pixels=g_np_mesh_dsc.data;
    const uint16_t *pixels=(const uint16_t *)original_pixels;
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
    np_toggle_queue(); pump(300);
    check(np_gradient_key==changed_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "Up Next must reuse the same gradient");
    np_toggle_queue(); pump(300);
    ui_select_screen(SCR_ALBUMS); pump(400);
    check(!np_gradient_backdrop && !np_gradient_key,"Page exit must detach gradient widgets");
    np_gradient_poll_cb(NULL);
    check(g_current_screen==SCR_ALBUMS,"A late background result must not affect another tab");
    ui_select_screen(SCR_NOW_PLAYING); pump(300);
    check(np_gradient_key==changed_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "Returning to Now Playing must use the cached artwork background");
    settings_np_bg_action((void *)(intptr_t)NP_BG_THEME); ui_rebuild_page(); pump(200);
    check(!np_gradient_backdrop && !np_gradient_key,"Theme mode must omit the cover gradient");
    inject=false;
    settings_np_bg_action((void *)(intptr_t)NP_BG_COVER_GRADIENT); ui_rebuild_page(); pump(200);
    check(np_gradient_key && !lv_obj_is_hidden(np_gradient_backdrop),
          "PNG/demo covers must still support gradients through the shared pipeline");
    settings_np_bg_action((void *)(intptr_t)NP_BG_THEME); ui_rebuild_page(); pump(100);
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
    gradient_lifecycle();
    artist_membership();
    puts("PASS: selective updates, delayed metadata/artwork, progress and queue visibility");
    return 0;
}
