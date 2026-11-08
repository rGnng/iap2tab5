#include "lvgl.h"

#include "boot.h"
#include "screenshot.h"
#include "ui.h"

#include <SDL2/SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAB5_HOR_RES 1280
#define TAB5_VER_RES 720
#define KEY_QUEUE_SIZE 32
#define LONG_PRESS_MS 650

static ui_key_t g_key_queue[KEY_QUEUE_SIZE];
static int g_key_head;
static int g_key_tail;
static bool g_enter_down;
static bool g_enter_long_fired;
static uint32_t g_enter_down_ms;

static void print_usage(const char * argv0)
{
    printf("IAP2 Tab5 UI simulator (LVGL %d.%d.%d)\n\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    printf("Usage: %s [options]\n\n", argv0);
    printf("  --zoom <factor>       Window zoom, e.g. 0.75 (default: 1.0)\n");
    printf("  --theme <0..2>        Gray, Aqua, or Windows 95\n");
    printf("  --afc <state>         AFC: disconnected, ready, trust, syncing, done, error\n");
    printf("  --screenshot <path>   Render one frame to a PNG and exit\n");
    printf("  --wait <ms>           Wait before the screenshot (default: 1000)\n");
    printf("  --screen <1-6>        Start on a specific screen\n");
    printf("  --menubar <side>      Menubar position: left or right\n");
    printf("  --keys <list>         Inject keys, e.g. \"2,enter\" or \"context\"\n");
    printf("  --boot                Play the KCN4M1 boot animation\n");
    printf("  --no-boot             Skip the boot sequence (default)\n");
    printf("  --boot-freeze <ms>    Freeze the boot sequence at a time offset\n");
    printf("  --headless            Force SDL's offscreen video driver\n");
    printf("  --help                Show this help\n\n");
    printf("Keys: Up/Down = knob, Enter = knob press (hold 650 ms = context menu),\n");
    printf("      7 = play/pause, Backspace = back,\n");
    printf("      PageUp/PageDown = previous/next menubar item, C = context menu;\n");
    printf("      1-6 reserved.\n");
    printf("In interactive mode the mouse acts as the touch panel.\n");
}

static bool env_has_display(void)
{
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL;
#endif
}

static bool map_sdl_key(const SDL_KeyboardEvent * key, ui_key_t * out)
{
    switch(key->keysym.sym) {
        case SDLK_UP:      *out = UI_KEY_UP; return true;
        case SDLK_DOWN:    *out = UI_KEY_DOWN; return true;
        case SDLK_PAGEUP:  *out = UI_KEY_PAGE_UP; return true;
        case SDLK_PAGEDOWN:*out = UI_KEY_PAGE_DOWN; return true;
        case SDLK_LEFTBRACKET:  *out = UI_KEY_SCREEN_PREV; return true;
        case SDLK_RIGHTBRACKET: *out = UI_KEY_SCREEN_NEXT; return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: *out = UI_KEY_ENTER; return true;
        case SDLK_BACKSPACE: *out = UI_KEY_BACK; return true;
        case SDLK_1:
        case SDLK_KP_1:    *out = UI_KEY_1; return true;
        case SDLK_2:
        case SDLK_KP_2:    *out = UI_KEY_2; return true;
        case SDLK_3:
        case SDLK_KP_3:    *out = UI_KEY_3; return true;
        case SDLK_4:
        case SDLK_KP_4:    *out = UI_KEY_4; return true;
        case SDLK_5:
        case SDLK_KP_5:    *out = UI_KEY_5; return true;
        case SDLK_6:
        case SDLK_KP_6:    *out = UI_KEY_6; return true;
        case SDLK_7:
        case SDLK_KP_7:    *out = UI_KEY_7; return true;
        case SDLK_c:       *out = UI_KEY_CONTEXT; return true;
        default: return false;
    }
}

static void queue_key(ui_key_t key)
{
    int next = (g_key_head + 1) % KEY_QUEUE_SIZE;
    if(next == g_key_tail) return;
    g_key_queue[g_key_head] = key;
    g_key_head = next;
}

static bool dequeue_key(ui_key_t * out)
{
    if(g_key_tail == g_key_head) return false;
    *out = g_key_queue[g_key_tail];
    g_key_tail = (g_key_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

static void drain_keys(void)
{
    ui_key_t key;
    while(dequeue_key(&key)) {
        if(boot_ui_active()) {
            /* Any key skips the boot animation. Drop the rest of the burst so
             * a held key does not immediately navigate the application UI. */
            boot_ui_skip();
            while(dequeue_key(&key)) { }
            break;
        }
        ui_handle_key(key);
    }
}

static bool parse_key_name(const char * name, ui_key_t * out)
{
    if(strcasecmp(name, "up") == 0) { *out = UI_KEY_UP; return true; }
    if(strcasecmp(name, "down") == 0) { *out = UI_KEY_DOWN; return true; }
    if(strcasecmp(name, "enter") == 0) { *out = UI_KEY_ENTER; return true; }
    if(strcasecmp(name, "pageup") == 0 || strcasecmp(name, "pgup") == 0) {
        *out = UI_KEY_PAGE_UP; return true;
    }
    if(strcasecmp(name, "pagedown") == 0 || strcasecmp(name, "pgdn") == 0) {
        *out = UI_KEY_PAGE_DOWN; return true;
    }
    if(strcmp(name, "[") == 0 || strcasecmp(name, "prev") == 0) {
        *out = UI_KEY_SCREEN_PREV; return true;
    }
    if(strcmp(name, "]") == 0 || strcasecmp(name, "next") == 0) {
        *out = UI_KEY_SCREEN_NEXT; return true;
    }
    if(strcmp(name, "1") == 0) { *out = UI_KEY_1; return true; }
    if(strcmp(name, "2") == 0) { *out = UI_KEY_2; return true; }
    if(strcmp(name, "3") == 0) { *out = UI_KEY_3; return true; }
    if(strcmp(name, "4") == 0) { *out = UI_KEY_4; return true; }
    if(strcmp(name, "5") == 0) { *out = UI_KEY_5; return true; }
    if(strcmp(name, "6") == 0) { *out = UI_KEY_6; return true; }
    if(strcasecmp(name, "backspace") == 0 ||
       strcasecmp(name, "back") == 0) { *out = UI_KEY_BACK; return true; }
    if(strcmp(name, "7") == 0) { *out = UI_KEY_7; return true; }
    if(strcasecmp(name, "context") == 0 || strcasecmp(name, "menu") == 0 ||
       strcasecmp(name, "c") == 0) {
        *out = UI_KEY_CONTEXT; return true;
    }
    return false;
}

static void inject_keys(const char * keys)
{
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", keys);
    char * save = NULL;
    for(char * token = strtok_r(buffer, ", ", &save); token != NULL;
        token = strtok_r(NULL, ", ", &save)) {
        ui_key_t key;
        if(parse_key_name(token, &key)) {
            ui_handle_key(key);
        }
        else {
            fprintf(stderr, "Unknown key token: %s\n", token);
        }
    }
}

static bool is_enter_key(const SDL_KeyboardEvent * key)
{
    return key->keysym.sym == SDLK_RETURN || key->keysym.sym == SDLK_KP_ENTER;
}

static void poll_held_keys(void)
{
    if(g_enter_down && !g_enter_long_fired &&
       SDL_GetTicks() - g_enter_down_ms >= LONG_PRESS_MS) {
        g_enter_long_fired = true;
        queue_key(UI_KEY_CONTEXT);
    }
}

static int sdl_key_watch(void * userdata, SDL_Event * event)
{
    (void)userdata;
    if(getenv("IAP2TAB5_DEBUG_EVENTS") != NULL) {
        if(event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) {
            fprintf(stderr, "mouse button %d at %d,%d\n", event->button.button,
                    event->button.x, event->button.y);
        }
    }

    if(event->type == SDL_KEYUP) {
        if(is_enter_key(&event->key)) {
            /* A short press activates on release so that holding Enter can be
             * reserved for the context menu. */
            if(g_enter_down && !g_enter_long_fired) queue_key(UI_KEY_ENTER);
            g_enter_down = false;
        }
        return 0;
    }
    if(event->type != SDL_KEYDOWN) return 0;

    /* Let key repeat work for the knob only; Enter and direct-access keys
     * should fire once per physical press. */
    if(event->key.repeat &&
       event->key.keysym.sym != SDLK_UP &&
       event->key.keysym.sym != SDLK_DOWN) {
        return 0;
    }

    if(is_enter_key(&event->key)) {
        g_enter_down = true;
        g_enter_long_fired = false;
        g_enter_down_ms = SDL_GetTicks();
        return 0;
    }

    if(event->key.keysym.sym == SDLK_F12) {
        sim_screenshot("/tmp/opencode/sim_drag.png");
        return 0;
    }

    ui_key_t key;
    if(map_sdl_key(&event->key, &key)) {
        queue_key(key);
    }
    return 0;
}

static const char * sim_afc_state;

static void sim_afc_status(ui_afc_status_t * out)
{
    memset(out, 0, sizeof(*out));
    out->available = sim_afc_state != NULL;
    out->afc_mode = sim_afc_state != NULL;
    out->sd_ready = out->sd_info_ready = true;
    out->usb_connected = sim_afc_state && strcmp(sim_afc_state, "disconnected") != 0;
    out->connected = out->pair_trusted = out->usb_connected && strcmp(sim_afc_state, "trust") != 0;
    out->pair_busy = sim_afc_state && strcmp(sim_afc_state, "trust") == 0;
    out->job_active = sim_afc_state && strcmp(sim_afc_state, "syncing") == 0;
    out->job_finished = sim_afc_state && (strcmp(sim_afc_state, "done") == 0 || strcmp(sim_afc_state, "error") == 0);
    out->job_failed = sim_afc_state && strcmp(sim_afc_state, "error") == 0;
    out->library_albums = 2096;
    out->library_tracks = 14201;
    out->thumb_count = 1824;
    out->library_db_bytes = 1900000;
    out->sd_used_bytes = 872218624;
    out->sd_total_bytes = 31715491840ULL;
    out->sync_phase = 2;
    out->sync_total = 2096;
    out->sync_attempted = 1372;
    out->sync_copied = 1372;
    out->job_kind = 2;
    snprintf(out->library_updated, sizeof(out->library_updated), "2026-11-08 10:32");
    snprintf(out->pair_status, sizeof(out->pair_status), "%s", out->pair_busy ?
        "Tap Trust on the device, then enter its passcode." : out->connected ?
        "" : "Unlock the device before pairing.");
    snprintf(out->sync_album, sizeof(out->sync_album), "TrickRoom - Album Mix");
    snprintf(out->status, sizeof(out->status), "Some files could not be copied. Reconnect and retry.");
}

static void sim_afc_action(int action)
{
    printf("[sim] AFC action %d (simulator)\n", action);
}

static void sim_drop_ipod_cb(lv_timer_t * timer)
{
    lv_timer_del(timer);
    ui_set_device_state(true, false, true);
}

int main(int argc, char ** argv)
{
    float zoom = 1.0f;
    const char * screenshot_path = NULL;
    int wait_ms = 1000;
    int start_screen = 0;
    int theme = 0;
    const char * menubar_side = NULL;
    const char * keys = NULL;
    bool headless = false;
    bool no_boot = false;
    bool boot_requested = false;
    int boot_freeze_ms = -1;

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if(strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            zoom = (float)atof(argv[++i]);
        }
        else if(strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        }
        else if(strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            wait_ms = atoi(argv[++i]);
        }
        else if(strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            start_screen = atoi(argv[++i]);
        }
        else if(strcmp(argv[i], "--menubar") == 0 && i + 1 < argc) {
            menubar_side = argv[++i];
        }
        else if(strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            keys = argv[++i];
        }
        else if(strcmp(argv[i], "--boot") == 0) {
            boot_requested = true;
        }
        else if(strcmp(argv[i], "--no-boot") == 0) {
            no_boot = true;
        }
        else if(strcmp(argv[i], "--boot-freeze") == 0 && i + 1 < argc) {
            boot_freeze_ms = atoi(argv[++i]);
        }
        else if(strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme = atoi(argv[++i]);
        }
        else if(strcmp(argv[i], "--afc") == 0 && i + 1 < argc) {
            sim_afc_state = argv[++i];
        }
        else if(strcmp(argv[i], "--headless") == 0) {
            headless = true;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    if(screenshot_path != NULL && !env_has_display()) {
        headless = true;
    }
#endif
    if(headless) {
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "offscreen",
                                SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software",
                                SDL_HINT_OVERRIDE);
    }

    lv_init();

    lv_display_t * display = lv_sdl_window_create(TAB5_HOR_RES, TAB5_VER_RES);
    if(display == NULL) {
        fprintf(stderr, "Failed to create the SDL window/display.\n");
        return 1;
    }

    lv_sdl_window_set_title(display, "IAP2 Tab5 - 1280x720 UI Simulator");
    if(zoom > 0.1f && zoom < 4.0f) {
        lv_sdl_window_set_zoom(display, zoom);
    }

    /* Mouse = touch panel. Keyboard events are captured by the app-level
     * router below instead of LVGL's keyboard indev. */
    lv_indev_t * touch = lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    SDL_AddEventWatch(sdl_key_watch, NULL);

    ui_system_config_t settings = {.theme = theme, .touch_enabled = 1,
        .volume = 65, .brightness = 70, .menubar_left = 1, .sort_desc = 1, .np_bg_mode = 2, .fast_scroll_pages = 2,
        .sound_enabled = true, .sound_mode = 2, .wifi_status = "WiFi off", .wifi_ip = "--"};
    ui_apply_saved_settings(&settings);
    ui_set_afc_sync_mode(sim_afc_state != NULL);
    ui_set_afc_status_cb(sim_afc_status);
    ui_set_afc_action_cb(sim_afc_action);
    ui_init();
    ui_attach_touch(touch);

    /* The boot sequence is the default startup experience. Explicit debug
     * selections (screen/menubar/keys) keep the old "jump straight to the UI"
     * behaviour so existing screenshot commands still work. */
    bool boot_enabled = (boot_requested || boot_freeze_ms >= 0) && !no_boot;
    if(boot_freeze_ms < 0 &&
       (start_screen > 0 || menubar_side != NULL || keys != NULL)) {
        boot_enabled = false;
    }
    if(boot_enabled) {
        if(boot_freeze_ms >= 0) {
            boot_ui_freeze((uint32_t)boot_freeze_ms);
        }
        else {
            boot_ui_start(NULL);
        }
    }

    /* Debug: exercise the device-availability screens. */
    if(getenv("SIM_SD_BACK") != NULL) {
        ui_set_device_state(false, false, false);
        ui_set_device_state(false, false, true);
    }
    else if(getenv("SIM_NO_IPOD") != NULL || getenv("SIM_NO_SD") != NULL) {
        ui_set_device_state(getenv("SIM_NO_IPOD") != NULL, false,
                            getenv("SIM_NO_SD") == NULL);
    }

    if(getenv("SIM_DROP_IPOD") != NULL) {
        /* Reproduce unplugging the iPod while a library tab is open. */
        lv_timer_create((lv_timer_cb_t)sim_drop_ipod_cb, 700, NULL);
    }

    if(start_screen >= 1 && start_screen <= 6) {
        ui_select_screen(start_screen - 1);
    }
    if(menubar_side != NULL) {
        if(strcasecmp(menubar_side, "right") == 0) ui_set_menubar_left(false);
        else if(strcasecmp(menubar_side, "left") == 0) ui_set_menubar_left(true);
        else fprintf(stderr, "Unknown menubar side: %s\n", menubar_side);
    }
    if(keys != NULL) {
        inject_keys(keys);
    }

    lv_refr_now(display);

    if(screenshot_path != NULL) {
        const uint32_t start = SDL_GetTicks();
        for(;;) {
            const uint32_t elapsed = SDL_GetTicks() - start;
            if(elapsed >= (uint32_t)wait_ms) break;

            uint32_t sleep_ms = lv_timer_handler();
            poll_held_keys();
            drain_keys();
            if(sleep_ms == LV_NO_TIMER_READY) sleep_ms = LV_DEF_REFR_PERIOD;

            const uint32_t remaining = (uint32_t)wait_ms - elapsed;
            if(sleep_ms > remaining) sleep_ms = remaining;
            SDL_Delay(sleep_ms);
        }
        lv_refr_now(display);
        return sim_screenshot(screenshot_path) ? 0 : 1;
    }

    for(;;) {
        uint32_t sleep_ms = lv_timer_handler();
        poll_held_keys();
        drain_keys();
        if(sleep_ms == LV_NO_TIMER_READY) sleep_ms = LV_DEF_REFR_PERIOD;
        if(sleep_ms > 0) SDL_Delay(sleep_ms);
    }

    return 0;
}
