/* The main window: the calculator screen under the menu bar, keyboard
 * input, and the platform interface the emulator uses. */
#include <stdio.h>
#include <string.h>

#include "app_icon.h"
#include "keys.h"
#include "r61524.h"
#include "view.h"

#define STATUS_MS    4000  /* how long a flashed status message stays */
#define WIDE_TEXT_W  600   /* windows at least this wide get 2x menu text */

SDL_Window *view_window;
SDL_Renderer *view_renderer;
bool view_headless;
int view_text_scale = 1;
SDL_FRect view_screen;
Command view_pending;
bool view_paused;
G3aInfo view_app;
bool view_have_app;
char view_app_path[1024];
Model view_model;
ModelChoice view_model_choice;
bool view_realistic = true;
int view_speed = 4; /* the 100% step */
bool view_show_speed;
bool view_key_shift;
int view_key_alpha;
int view_battery_cv = BATTERY_CENTIVOLTS;
char view_os_status[64];

static SDL_Texture *screen;    /* the panel, 396 x 224 */
static SDL_Texture *upscaled;  /* the panel at a whole scale, see draw_screen() */
static int upscale;            /* its scale, 0 = not created */
static int window_w;
static uint16_t frame[PANEL_WIDTH * PANEL_HEIGHT]; /* last presented frame, host RGB565 */
static char status[320];
static uint64_t status_until; /* 0 = stays */
/* Which KEYS entry each held scancode pressed (index + 1), so that a key
 * released after its modifier still releases the right calculator key. */
static uint8_t held[SDL_SCANCODE_COUNT];

static const struct { const char *name; SDL_Keycode sdl; } KEYMAP[] = {
    {"EXE", SDLK_RETURN}, {"EXIT", SDLK_ESCAPE}, {"MENU", SDLK_HOME},
    {"SHIFT", SDLK_TAB}, {"ALPHA", SDLK_LALT}, {"OPTN", SDLK_O},
    {"AC", SDLK_DELETE}, {"DEL", SDLK_BACKSPACE},
    {"UP", SDLK_UP}, {"DOWN", SDLK_DOWN}, {"LEFT", SDLK_LEFT}, {"RIGHT", SDLK_RIGHT},
    {"F1", SDLK_F1}, {"F2", SDLK_F2}, {"F3", SDLK_F3},
    {"F4", SDLK_F4}, {"F5", SDLK_F5}, {"F6", SDLK_F6},
    {"0", SDLK_0}, {"1", SDLK_1}, {"2", SDLK_2}, {"3", SDLK_3}, {"4", SDLK_4},
    {"5", SDLK_5}, {"6", SDLK_6}, {"7", SDLK_7}, {"8", SDLK_8}, {"9", SDLK_9},
    {"DP", SDLK_PERIOD}, {"PLUS", SDLK_PLUS}, {"MINUS", SDLK_MINUS},
    {"MULT", SDLK_ASTERISK}, {"DIV", SDLK_SLASH},
    {"LPAR", SDLK_LEFTPAREN}, {"RPAR", SDLK_RIGHTPAREN}, {"EXP", SDLK_E},
    {"VARS", SDLK_V}, {"XOT", SDLK_X}, {"LOG", SDLK_L}, {"LN", SDLK_N},
    {"SIN", SDLK_S}, {"COS", SDLK_C}, {"TAN", SDLK_T}, {"FRAC", SDLK_F},
    {"FD", SDLK_D}, {"COMMA", SDLK_COMMA}, {"ARROW", SDLK_A}, {"SQUARE", SDLK_Q},
    {"POWER", SDLK_P}, {"NEG", SDLK_U},
};


void view_text(SDL_Renderer *r, float x, float y, const char *s, int k)
{
    SDL_SetRenderScale(r, (float)k, (float)k);
    SDL_RenderDebugText(r, x / k, y / k, s);
    SDL_SetRenderScale(r, 1, 1);
}

static int text_scale_for(int width)
{
    return width >= WIDE_TEXT_W ? 2 : 1;
}

/* Fits the screen under the menu bar, keeping its shape, centred. */
static void update_layout(void)
{
    int w, h;
    SDL_GetWindowSize(view_window, &w, &h);
    window_w = w;
    view_text_scale = text_scale_for(w);
    float area_h = (float)(h - BAR_H);
    float k = (float)w / PANEL_WIDTH;
    if (area_h / PANEL_HEIGHT < k)
        k = area_h / PANEL_HEIGHT;
    if (k < 0.25f)
        k = 0.25f;
    view_screen.w = PANEL_WIDTH * k;
    view_screen.h = PANEL_HEIGHT * k;
    view_screen.x = ((float)w - view_screen.w) / 2;
    view_screen.y = BAR_H + (area_h - view_screen.h) / 2;
}

int view_zoom(void)
{
    for (int z = 1; z <= 6; z++)
        if ((int)view_screen.w == PANEL_WIDTH * z && (int)view_screen.h == PANEL_HEIGHT * z)
            return z;
    return 0;
}

/* "Sharp bilinear": scale the panel up by a whole factor with nearest
 * pixels, then smoothly to the final size. Pixels stay crisp at any window
 * size, without the uneven rows and columns of nearest-only scaling. */
static void draw_screen(void)
{
    int k = (int)(view_screen.w / PANEL_WIDTH);
    if (k < 1)
        k = 1;
    if ((float)(PANEL_WIDTH * k) == view_screen.w) { /* exact: one nearest blit */
        SDL_RenderTexture(view_renderer, screen, NULL, &view_screen);
        return;
    }
    if (k != upscale) {
        SDL_DestroyTexture(upscaled);
        upscaled = SDL_CreateTexture(view_renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                     PANEL_WIDTH * k, PANEL_HEIGHT * k);
        SDL_SetTextureScaleMode(upscaled, SDL_SCALEMODE_LINEAR);
        upscale = upscaled ? k : 0;
    }
    if (!upscaled) {
        SDL_RenderTexture(view_renderer, screen, NULL, &view_screen);
        return;
    }
    SDL_SetRenderTarget(view_renderer, upscaled);
    SDL_RenderTexture(view_renderer, screen, NULL, NULL);
    SDL_SetRenderTarget(view_renderer, NULL);
    SDL_RenderTexture(view_renderer, upscaled, NULL, &view_screen);
}

void view_redraw(void)
{
    if (view_headless)
        return;
    if (status_until && SDL_GetTicks() >= status_until) {
        status[0] = '\0';
        status_until = 0;
    }
    SDL_SetRenderDrawColor(view_renderer, 32, 32, 32, 255);
    SDL_RenderClear(view_renderer);
    SDL_UpdateTexture(screen, NULL, frame, PANEL_WIDTH * 2);
    draw_screen();
    menubar_draw(window_w, status);
    SDL_RenderPresent(view_renderer);
}

void view_set_scale(int scale)
{
    int w = PANEL_WIDTH * scale;
    int ts = text_scale_for(w);
    int bar = (8 * ts + 16) + (view_show_speed ? 8 * ts + 14 : 0) + (8 * ts + 14); /* BAR_H at that width */
    SDL_SetWindowSize(view_window, w, bar + PANEL_HEIGHT * scale);
    SDL_SyncWindow(view_window);
    update_layout();
}

void view_show_speed_row(bool show)
{
    if (show == view_show_speed)
        return;
    int w, h, before = BAR_H;
    SDL_GetWindowSize(view_window, &w, &h);
    view_show_speed = show;
    SDL_SetWindowSize(view_window, w, h + BAR_H - before);
    SDL_SyncWindow(view_window);
    update_layout();
}

void view_flash_status(const char *msg)
{
    snprintf(status, sizeof status, "%s", msg);
    status_until = SDL_GetTicks() + STATUS_MS;
}

void platform_flash(const char *msg)
{
    if (!view_headless)
        view_flash_status(msg);
}

void platform_status(const char *msg)
{
    snprintf(status, sizeof status, "%s", msg ? msg : "");
    status_until = 0;
    view_redraw();
}

const char *view_pc_key(int key)
{
    for (size_t i = 0; i < sizeof KEYMAP / sizeof KEYMAP[0]; i++) {
        if (key_find(KEYMAP[i].name) != key)
            continue;
        const char *name = SDL_GetKeyName(KEYMAP[i].sdl);
        if (!strcmp(name, "Return")) return "Enter";
        if (!strcmp(name, "Escape")) return "Esc";
        return name;
    }
    return "-";
}

const char *view_calc_key(int key)
{
    static const struct { const char *name, *label; } LABELS[] = {
        {"AC", "AC/ON"}, {"DP", "."}, {"EXP", "x10^x"}, {"PLUS", "+"}, {"MINUS", "-"},
        {"MULT", "*"}, {"DIV", "/"}, {"LPAR", "("}, {"RPAR", ")"},
        {"XOT", "X,theta,T"}, {"LOG", "log"}, {"LN", "ln"}, {"SIN", "sin"},
        {"COS", "cos"}, {"TAN", "tan"}, {"FRAC", "a b/c"}, {"FD", "F<>D"},
        {"COMMA", ","}, {"ARROW", "->"}, {"SQUARE", "x^2"}, {"POWER", "^"},
        {"NEG", "(-)"},
    };
    for (size_t i = 0; i < sizeof LABELS / sizeof LABELS[0]; i++)
        if (!strcmp(KEYS[key].name, LABELS[i].name))
            return LABELS[i].label;
    return KEYS[key].name;
}

/* KEYS index for an SDL key, or -1. With ALPHA on, letters and space
 * press the key whose alpha function they are. */
static int key_for(SDL_Keycode k)
{
    if (view_key_alpha && ((k >= SDLK_A && k <= SDLK_Z) || k == SDLK_SPACE))
        return key_for_letter(k == SDLK_SPACE ? ' ' : (int)(k - SDLK_A) + 'A');
    for (size_t i = 0; i < sizeof KEYMAP / sizeof KEYMAP[0]; i++)
        if (KEYMAP[i].sdl == k)
            return key_find(KEYMAP[i].name);
    return -1;
}

/* SHIFT and ALPHA as the calculator's keyboard handling sees them. gint
 * add-ins read the keyboard themselves, so the window follows the keys
 * pressed with the OS's rules; in OS add-ins GetKey has the last word
 * (platform_set_key_mode). */
static void track_modifiers(int k)
{
    int code = KEYS[k].getkey;
    if (code == KEY_CTRL_SHIFT) {
        view_key_shift = !view_key_shift;
    } else if (code == KEY_CTRL_ALPHA) {
        view_key_alpha = view_key_shift ? 2 : view_key_alpha ? 0 : 1;
        view_key_shift = false;
    } else {
        view_key_shift = false;
        if (view_key_alpha == 1)
            view_key_alpha = 0;
    }
    view_redraw();
}

/* Handles one event. Returns the KEYS index of a calculator key pressed,
 * else -1. */
static int handle_event(const SDL_Event *e)
{
    if (info_window_event(e) || menubar_event(e))
        return -1;
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: /* the main window: with two windows open, no QUIT comes */
        view_pending = CMD_QUIT;
        break;
    case SDL_EVENT_KEY_UP:
        held[e->key.scancode] = 0;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat) {
            int k = key_for(e->key.key);
            held[e->key.scancode] = (uint8_t)(k + 1);
            if (k >= 0)
                track_modifiers(k);
            return k;
        }
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        update_layout();
        view_redraw();
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        view_redraw();
        break;
    }
    return -1;
}

/* Work that doesn't come as an event: the Open dialog, expiring status. */
static void housekeeping(void)
{
    menubar_poll();
    if (status_until && SDL_GetTicks() >= status_until)
        view_redraw();
}

bool platform_init(int scale, bool headless)
{
    view_headless = headless;
    if (headless)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (!font_init()) {
        fprintf(stderr, "font: %s\n", SDL_GetError());
        return false;
    }
    if (headless)
        return true;
    int w = PANEL_WIDTH * scale;
    view_text_scale = text_scale_for(w);
    if (!SDL_CreateWindowAndRenderer("emuCasioFxCG", w, BAR_H + PANEL_HEIGHT * scale, SDL_WINDOW_RESIZABLE,
                                     &view_window, &view_renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return false;
    }
    static uint32_t icon[APP_ICON_SIZE * APP_ICON_SIZE];
    app_icon_pixels(icon);
    SDL_Surface *s = SDL_CreateSurfaceFrom(APP_ICON_SIZE, APP_ICON_SIZE, SDL_PIXELFORMAT_ARGB8888, icon,
                                           APP_ICON_SIZE * 4);
    SDL_SetWindowIcon(view_window, s);
    SDL_DestroySurface(s);
    screen = SDL_CreateTexture(view_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, PANEL_WIDTH,
                               PANEL_HEIGHT);
    SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_NEAREST);
    SDL_SetWindowMinimumSize(view_window, PANEL_WIDTH, (8 + 16) + (8 + 14) + PANEL_HEIGHT); /* 1x screen and bar */
    update_layout();
    return screen != NULL;
}

void platform_shutdown(void)
{
    info_window_close();
    SDL_DestroyTexture(upscaled);
    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(view_renderer);
    SDL_DestroyWindow(view_window);
    SDL_Quit();
}

void platform_present(const uint16_t *fb)
{
    memcpy(frame, fb, sizeof frame);
    view_redraw();
}

bool platform_save_bmp(const char *path, const uint16_t *pixels, int w, int h)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB565, (void *)pixels, w * 2);
    bool ok = s && SDL_SaveBMP(s, path);
    if (!ok)
        fprintf(stderr, "%s: %s\n", path, SDL_GetError());
    SDL_DestroySurface(s);
    return ok;
}

bool platform_save_screenshot(const char *path)
{
    return platform_save_bmp(path, frame, PANEL_WIDTH, PANEL_HEIGHT);
}

void platform_set_app(const G3aInfo *info, const char *path, Model model)
{
    view_app = *info;
    view_have_app = true;
    view_model = model;
    snprintf(view_app_path, sizeof view_app_path, "%s", path);
    if (view_headless)
        return;
    char title[64];
    snprintf(title, sizeof title, "%s - %s - emuCasioFxCG", view_app.name[0] ? view_app.name : "add-in",
             model_name(model));
    SDL_SetWindowTitle(view_window, title);
    platform_set_key_mode(false, 0);
    platform_set_os_status("");
    info_window_refresh();
}

void platform_set_key_mode(bool shift, int alpha)
{
    if (view_headless || (shift == view_key_shift && alpha == view_key_alpha))
        return;
    view_key_shift = shift;
    view_key_alpha = alpha;
    view_redraw();
}

void platform_set_os_status(const char *msg)
{
    if (!strcmp(msg, view_os_status))
        return;
    snprintf(view_os_status, sizeof view_os_status, "%s", msg);
    view_redraw();
}

int platform_battery(void)
{
    return view_battery_cv;
}

void platform_set_battery(int centivolts)
{
    view_battery_cv = centivolts;
    if (view_renderer) /* --battery comes before the window exists */
        view_redraw();
}

double platform_speed(void)
{
    return menubar_speed();
}

void platform_set_speed(double speed)
{
    menubar_set_speed(speed);
}

bool platform_realistic(void)
{
    return view_realistic;
}

void platform_set_realistic(bool realistic)
{
    view_realistic = realistic;
}

ModelChoice platform_model_choice(void)
{
    return view_model_choice;
}

void platform_set_model_choice(ModelChoice choice)
{
    view_model_choice = choice;
}

void platform_show_info(void)
{
    info_window_open();
}

bool platform_poll(bool *down)
{
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handle_event(&e);
    housekeeping();
    for (int i = 0; i < KEY_COUNT; i++)
        down[i] = false;
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (held[sc])
            down[held[sc] - 1] = true;
    return view_pending == CMD_NONE;
}

int platform_wait_key(void)
{
    while (view_pending == CMD_NONE) {
        SDL_Event e;
        /* Wake up regularly for the Open dialog and status messages. */
        if (SDL_WaitEventTimeout(&e, 100)) {
            int k = handle_event(&e);
            if (k >= 0 && !view_paused)
                return k;
        }
        housekeeping();
    }
    return -1;
}

int platform_wait_key_timeout(uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (view_pending == CMD_NONE) {
        uint64_t now = SDL_GetTicks();
        if (now >= end)
            return -2;
        SDL_Event e;
        uint64_t left = end - now;
        if (SDL_WaitEventTimeout(&e, (Sint32)(left < 100 ? left : 100))) {
            int k = handle_event(&e);
            if (k >= 0 && !view_paused)
                return k;
        }
        housekeeping();
    }
    return -1;
}

bool platform_idle(uint32_t ms)
{
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, (Sint32)ms)) {
        handle_event(&e);
        while (SDL_PollEvent(&e))
            handle_event(&e);
    }
    housekeeping();
    return view_pending == CMD_NONE;
}

Command platform_take_command(void)
{
    Command c = view_pending;
    view_pending = CMD_NONE;
    return c;
}

bool platform_has_command(void)
{
    return view_pending != CMD_NONE;
}

const char *platform_open_path(void)
{
    return menubar_open_path();
}

bool platform_paused(void)
{
    return view_paused;
}

void platform_delay_ms(uint32_t ms)
{
    SDL_Delay(ms);
}

uint64_t platform_ticks_ms(void)
{
    return SDL_GetTicks();
}
