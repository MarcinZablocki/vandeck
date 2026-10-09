// VanDeck UI on the desktop at another resolution: an 800x480 LCD (--lcd) or e-paper panel (Waveshare 7.5" V2).
//
// The real ui/*.c renders into an 800x480 ARGB buffer; every finished frame goes through an e-ink filter before it
// reaches the SDL window. Keys:
//   --shots DIR renders every page in every mode to BMPs and exits.
//   1 colour (no filter)   2 1-bit threshold   3 1-bit dithered   4 4-level grey
//   I invert (a light theme stand-in)   R simulate panel refreshes   S save a screenshot   Q / Esc quit
// The window title counts panel updates per minute: e-paper wants that number small.
#include "lvgl.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ui/ui.h"
#include "app/model.h"
#include "app/settings.h"
#include "app/sim.h"
#include "esp_stubs.h"
#include "ui/ui_views.h"

#define MAXW 1280
#define MAXH 800
static int W = 800, H = 480;
static bool s_lcd;                  // plain LCD: colour, every frame shown as rendered

enum { MODE_COLOR = 1, MODE_BW, MODE_DITHER, MODE_GRAY4 };
static int s_mode = MODE_BW, s_scale = 1;
static bool s_invert, s_refresh_sim = true;

static uint32_t s_fb[MAXW * MAXH];        // LVGL renders here (XRGB8888)
static uint32_t s_out[MAXW * MAXH];       // what the "panel" shows
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static SDL_Window *s_win;
static bool s_dirty;                // LVGL finished a frame since the last panel update
static uint32_t s_flash_until;      // refresh flash in progress
static uint32_t s_updates[64]; static int s_nupd;   // panel update timestamps (ring)

// Paper-ish tones: black ink isn't quite black and the white is warm grey.
static const uint8_t TONE[4][3] = { { 0x1C, 0x1C, 0x1E }, { 0x6E, 0x6D, 0x69 }, { 0xAC, 0xAA, 0xA4 }, { 0xE6, 0xE4, 0xDE } };
static inline uint32_t tone(int i) { return 0xFF000000u | TONE[i][0] << 16 | TONE[i][1] << 8 | TONE[i][2]; }

static inline int luma(uint32_t p)
{
    int r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
    int y = (r * 77 + g * 150 + b * 29) >> 8;
    return s_invert ? 255 - y : y;
}

static void filter(void)
{
    static int16_t err[2][MAXW + 2];
    switch (s_mode) {
    case MODE_COLOR:
        for (int i = 0; i < W * H; i++) {
            uint32_t p = s_fb[i];
            s_out[i] = s_invert ? 0xFF000000u | (~p & 0xFFFFFF) : 0xFF000000u | p;
        }
        break;
    case MODE_BW:
        for (int i = 0; i < W * H; i++) s_out[i] = tone(luma(s_fb[i]) >= 128 ? 3 : 0);
        break;
    case MODE_GRAY4:
        for (int i = 0; i < W * H; i++) { int y = luma(s_fb[i]); s_out[i] = tone(y < 43 ? 0 : y < 128 ? 1 : y < 213 ? 2 : 3); }
        break;
    case MODE_DITHER:   // Floyd-Steinberg to black/white
        memset(err, 0, sizeof err);
        for (int y = 0; y < H; y++) {
            int16_t *cur = err[y & 1], *nxt = err[!(y & 1)];
            memset(nxt, 0, sizeof err[0]);
            for (int x = 0; x < W; x++) {
                int v = luma(s_fb[y * W + x]) + cur[x + 1] / 16;
                int o = v >= 128 ? 255 : 0, e = v - o;
                s_out[y * W + x] = tone(o ? 3 : 0);
                cur[x + 2] += e * 7; nxt[x] += e * 3; nxt[x + 1] += e * 5; nxt[x + 2] += e;
            }
        }
        break;
    }
}

static void present(const uint32_t *px)
{
    SDL_UpdateTexture(s_tex, NULL, px, W * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

static void update_title(void)
{
    uint32_t now = SDL_GetTicks(); int n = 0;
    for (int i = 0; i < 64; i++) if (s_updates[i] && now - s_updates[i] < 60000) n++;
    static const char *MODE[] = { "", "colour", "1-bit", "1-bit dithered", "4-grey" };
    char t[160];
    if (s_lcd) { snprintf(t, sizeof t, "VanDeck LCD sim %dx%d", W, H); SDL_SetWindowTitle(s_win, t); return; }
    snprintf(t, sizeof t, "VanDeck e-paper sim %dx%d | %s%s%s | updates last min: %d%s", W, H,
             MODE[s_mode], s_invert ? " inverted" : "", s_refresh_sim ? " | refresh sim" : "", n, n >= 64 ? "+" : "");
    SDL_SetWindowTitle(s_win, t);
}

// A panel update: with the refresh sim on, flash black then white (as a full refresh does) before the new image.
static void panel_update(void)
{
    filter();
    s_updates[s_nupd++ % 64] = SDL_GetTicks();
    if (s_mode != MODE_COLOR && s_refresh_sim) {
        static uint32_t solid[MAXW * MAXH];
        for (int k = 0; k < 2; k++) {
            uint32_t c = tone(k ? 3 : 0);
            for (int i = 0; i < W * H; i++) solid[i] = c;
            present(solid);
            SDL_Delay(120);
        }
    }
    present(s_out);
    update_title();
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    if (lv_display_flush_is_last(d)) s_dirty = true;
    lv_display_flush_ready(d);
}

static lv_indev_t *s_ptr;
static int s_mx, s_my; static bool s_pressed;
static void ptr_read(lv_indev_t *i, lv_indev_data_t *d)
{
    d->point.x = s_mx; d->point.y = s_my;
    d->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// esp_timer on top of lv_timer (app/sim.c uses it for the demo data).
struct esp_timer { lv_timer_t *t; esp_timer_cb_t cb; void *arg; };
static void et_fire(lv_timer_t *t) { struct esp_timer *e = lv_timer_get_user_data(t); e->cb(e->arg); }
int64_t esp_timer_get_time(void) { return (int64_t)SDL_GetTicks64() * 1000; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{
    struct esp_timer *e = calloc(1, sizeof *e);
    e->cb = a->callback; e->arg = a->arg; *out = e;
    return ESP_OK;
}
static esp_err_t et_start(esp_timer_handle_t e, uint64_t us, int repeat)
{
    if (e->t) lv_timer_delete(e->t);
    e->t = lv_timer_create(et_fire, (uint32_t)(us / 1000), e);
    lv_timer_set_repeat_count(e->t, repeat);
    if (repeat == 1) lv_timer_set_auto_delete(e->t, false);
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t e, uint64_t us) { return et_start(e, us, -1); }
esp_err_t esp_timer_start_once(esp_timer_handle_t e, uint64_t us) { return et_start(e, us, 1); }
esp_err_t esp_timer_stop(esp_timer_handle_t e) { if (e->t) { lv_timer_delete(e->t); e->t = NULL; } return ESP_OK; }

void sim_fill_static(void);

static void save_bmp(const char *name)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(s_out, W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
    SDL_SaveBMP(s, name);
    SDL_FreeSurface(s);
    printf("saved %s\n", name);
}
static void screenshot(void)
{
    char name[64];
    snprintf(name, sizeof name, "vandeck-eink-%ld.bmp", (long)time(NULL));
    save_bmp(name);
}

// Let LVGL run for ms of wall time, so timers, the demo data and animations catch up.
static void run_for(uint32_t ms)
{
    uint32_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) { lv_timer_handler(); SDL_Delay(5); }
}

// --shots DIR: every page in every filter mode, as BMPs, then exit.
static int shots(const char *dir)
{
    static const char *VIEW[] = { "energy", "solar", "tanks", "starlink", "weather" };
    static const char *MODE[] = { "", "colour", "bw", "dither", "gray4" };
    run_for(2500);
    for (int v = 0; v < V_COUNT; v++) {
        ui_show_view(v);
        run_for(400);
        lv_refr_now(NULL);
        for (int m = MODE_COLOR; m <= (s_lcd ? MODE_COLOR : MODE_GRAY4); m++)
            for (int inv = 0; inv < (s_lcd ? 1 : 2); inv++) {
                s_mode = m; s_invert = inv; filter();
                char name[256];
                snprintf(name, sizeof name, "%s/%d-%s-%s%s.bmp", dir, v + 1, VIEW[v], MODE[m], inv ? "-inv" : "");
                save_bmp(name);
            }
    }
    // Energy again, a few frames apart: the battery flow animation.
    ui_show_view(V_ENERGY);
    run_for(400);
    for (int k = 0; k < 4; k++) {
        run_for(350);
        lv_refr_now(NULL);
        s_mode = s_lcd ? MODE_COLOR : MODE_BW; s_invert = false; filter();
        char name[256];
        snprintf(name, sizeof name, "%s/energy-anim-%d.bmp", dir, k + 1);
        save_bmp(name);
    }

    // Settings: open it, then click through its tabs (tab list = body's first child, inside the top-layer root).
    lv_obj_t *root = NULL;
    ui_settings_open();
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); i++) {
        lv_obj_t *o = lv_obj_get_child(lv_layer_top(), i);
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_child_count(o) == 2) root = o;
    }
    lv_obj_t *tl = root ? lv_obj_get_child(lv_obj_get_child(root, 1), 0) : NULL;
    for (uint32_t t = 0; tl && t < lv_obj_get_child_count(tl); t++) {
        lv_obj_send_event(lv_obj_get_child(tl, t), LV_EVENT_CLICKED, NULL);
        run_for(300);
        lv_refr_now(NULL);
        s_mode = MODE_COLOR; s_invert = false; filter();
        char name[256];
        snprintf(name, sizeof name, "%s/settings-%u.bmp", dir, t + 1);
        save_bmp(name);
    }
    // Text entry: the full keyboard (Wi-Fi password) and the keypad (weather location).
    for (int k = 0; k < 2; k++) {
        if (k) ui_entry_open_location(); else ui_entry_open_wifi("Starlink");
        run_for(300);
        lv_refr_now(NULL);
        filter();
        char name[256];
        snprintf(name, sizeof name, "%s/entry-%s.bmp", dir, k ? "location" : "wifi");
        save_bmp(name);
    }
    return 0;
}

static uint32_t tick_cb(void) { return SDL_GetTicks(); }

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) s_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) s_mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--invert")) s_invert = true;
        else if (!strcmp(argv[i], "--no-refresh")) s_refresh_sim = false;
        else if (!strcmp(argv[i], "--lcd")) { s_lcd = true; s_mode = MODE_COLOR; s_refresh_sim = false; }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
    }
    const char *shot_dir = NULL;
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "--shots")) shot_dir = argv[i + 1];
    if (s_mode < MODE_COLOR || s_mode > MODE_GRAY4) s_mode = MODE_BW;
    if (s_scale < 1) s_scale = 1;
    if (W < 320 || W > MAXW || H < 240 || H > MAXH) { fprintf(stderr, "--size must be within 320x240..%dx%d\n", MAXW, MAXH); return 1; }

    SDL_Init(SDL_INIT_VIDEO);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    s_win = SDL_CreateWindow("VanDeck", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * s_scale, H * s_scale,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(s_ren, W, H);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d, s_fb, NULL, W * H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);
    s_ptr = lv_indev_create();
    lv_indev_set_type(s_ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_ptr, ptr_read);

    model_init();
    settings_load();
    ui_splash_show();
    ui_init();
    sim_fill_static();
    sim_start();
    ui_splash_hide();
    if (shot_dir) return shots(shot_dir);

    for (bool run = true; run;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: run = false; break;
            case SDL_MOUSEMOTION: {
                float lx, ly; SDL_RenderWindowToLogical(s_ren, ev.motion.x, ev.motion.y, &lx, &ly);
                s_mx = (int)lx; s_my = (int)ly; break;
            }
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                float lx, ly; SDL_RenderWindowToLogical(s_ren, ev.button.x, ev.button.y, &lx, &ly);
                s_mx = (int)lx; s_my = (int)ly; s_pressed = ev.type == SDL_MOUSEBUTTONDOWN; break;
            }
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_q || k == SDLK_ESCAPE) run = false;
                else if (k >= SDLK_1 && k <= SDLK_4) { s_mode = k - SDLK_0; s_dirty = true; }
                else if (k == SDLK_i) { s_invert = !s_invert; s_dirty = true; }
                else if (k == SDLK_r) { s_refresh_sim = !s_refresh_sim; update_title(); }
                else if (k == SDLK_s) screenshot();
                break;
            }
            case SDL_WINDOWEVENT: present(s_out); break;
            }
        }
        lv_timer_handler();
        // Coalesce: an e-paper driver would wait for LVGL to settle rather than push every animation frame.
        static uint32_t s_last_change;
        if (s_dirty) { s_dirty = false; s_last_change = SDL_GetTicks(); s_flash_until = 1; }
        if (s_flash_until && (s_lcd || SDL_GetTicks() - s_last_change > 150)) { s_flash_until = 0; panel_update(); }
        SDL_Delay(5);
    }
    SDL_Quit();
    return 0;
}
