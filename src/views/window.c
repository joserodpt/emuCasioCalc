#include "platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_icon.h"
#include "keys.h"
#include "r61524.h"

#define FIRST_GLYPH 0x20
#define GLYPH_COUNT 95

/* Menu text is SDL's 8x8 debug font drawn at 2x, or 1x when the window is
 * at zoom 1 (396 px is too narrow for the bar at 2x). Sizes in window pixels. */
#define TEXT_SCALE (scale >= 2 ? 2 : 1)
#define CHAR_W     (8 * TEXT_SCALE)
#define GLYPH_H    (8 * TEXT_SCALE)
#define BAR_H      (GLYPH_H + 16)
#define ITEM_H     (GLYPH_H + 18)
#define PAD        (6 + 4 * TEXT_SCALE)
#define STATUS_MS  4000  /* how long a status message stays */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen;
static uint16_t frame[PANEL_WIDTH * PANEL_HEIGHT]; /* last presented frame, host RGB565 */
static uint8_t glyphs[GLYPH_COUNT][8];
static bool headless;
static int scale;
/* Which KEYS entry each held scancode pressed (index + 1), so that a key
 * released after its modifier still releases the right calculator key. */
static uint8_t held[SDL_SCANCODE_COUNT];

static Command pending;
static bool paused;
static char status[320];
static uint64_t status_until;  /* 0 = stays */
static G3aInfo app;           /* the add-in being run */
static bool have_app;
static char app_path[1024];

/* Add-in info window */
static SDL_Window *info_win;
static SDL_Renderer *info_ren;
static SDL_Texture *info_icon[2]; /* unselected, selected */
static bool info_hover;           /* mouse over the icon: show the selected one */

/* File > Open runs asynchronously; the dialog's callback may come from
 * another thread, so it hands the path over through an atomic flag. */
static char open_path[1024];
static SDL_AtomicInt open_ready;

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
};

/*---------------------------------------------------------------------------
 * Menus
 *-------------------------------------------------------------------------*/

typedef enum {
    A_NONE, A_SEPARATOR, A_OPEN, A_INFO, A_SCREENSHOT, A_SAVE_ICON, A_QUIT, A_PAUSE, A_RESET,
    A_ZOOM1, A_ZOOM2, A_ZOOM3, A_ZOOM4, A_KEYS, A_ABOUT,
} Action;

typedef struct {
    const char *label, *shortcut;
    Action action;
} Item;

static const Item FILE_ITEMS[] = {
    {"Open add-in...", "Ctrl+O", A_OPEN},
    {"Add-in info...", "Ctrl+I", A_INFO},
    {"", "", A_SEPARATOR},
    {"Save screenshot", "Ctrl+S", A_SCREENSHOT},
    {"Save icon", "", A_SAVE_ICON},
    {"", "", A_SEPARATOR},
    {"Quit", "Ctrl+Q", A_QUIT},
};
static const Item EMU_ITEMS[] = {
    {"Pause", "Ctrl+P", A_PAUSE},
    {"Reset", "Ctrl+R", A_RESET},
};
static const Item VIEW_ITEMS[] = {
    {"Zoom 1x", "Ctrl+1", A_ZOOM1},
    {"Zoom 2x", "Ctrl+2", A_ZOOM2},
    {"Zoom 3x", "Ctrl+3", A_ZOOM3},
    {"Zoom 4x", "Ctrl+4", A_ZOOM4},
};
static const Item HELP_ITEMS[] = {
    {"Keyboard controls", "", A_KEYS},
    {"About", "", A_ABOUT},
};

/* Entries of the bar: drop-down menus, or buttons (no items) that run
 * their action directly. */
#define MENU(t, items) {t, items, (int)(sizeof items / sizeof items[0]), A_NONE}
static const struct {
    const char *title;
    const Item *items;
    int count;
    Action button;
} MENUS[] = {
    MENU("File", FILE_ITEMS),
    MENU("Emulation", EMU_ITEMS),
    MENU("View", VIEW_ITEMS),
    MENU("Help", HELP_ITEMS),
    {"Info", NULL, 0, A_INFO},
};
#define MENU_COUNT ((int)(sizeof MENUS / sizeof MENUS[0]))

static int open_menu = -1;   /* index of the dropped-down menu, -1 if none */
static int hover_item = -1;
static float mouse_x, mouse_y;

static int title_x(int m)
{
    int x = 0;
    for (int i = 0; i < m; i++)
        x += (int)strlen(MENUS[i].title) * CHAR_W + 2 * PAD;
    return x;
}

static int title_w(int m)
{
    return (int)strlen(MENUS[m].title) * CHAR_W + 2 * PAD;
}

static int dropdown_w(int m)
{
    int w = 0;
    for (int i = 0; i < MENUS[m].count; i++) {
        int iw = (int)(strlen(MENUS[m].items[i].label) + strlen(MENUS[m].items[i].shortcut)) * CHAR_W;
        if (iw > w)
            w = iw;
    }
    return w + 2 * PAD + 3 * PAD + 2 * CHAR_W; /* check mark column + gap */
}

static int title_at(float x, float y)
{
    if (y < 0 || y >= BAR_H)
        return -1;
    for (int m = 0; m < MENU_COUNT; m++)
        if (x >= title_x(m) && x < title_x(m) + title_w(m))
            return m;
    return -1;
}

static int item_at(float x, float y)
{
    if (open_menu < 0)
        return -1;
    int x0 = title_x(open_menu);
    if (x < x0 || x >= x0 + dropdown_w(open_menu) || y < BAR_H)
        return -1;
    int i = (int)(y - BAR_H - 1) / ITEM_H;
    if (i < 0 || i >= MENUS[open_menu].count || MENUS[open_menu].items[i].action == A_SEPARATOR)
        return -1;
    return i;
}

static bool checked(Action a)
{
    if (a >= A_ZOOM1 && a <= A_ZOOM4)
        return scale == (int)(a - A_ZOOM1) + 1;
    return a == A_PAUSE && paused;
}

/*---------------------------------------------------------------------------
 * Drawing
 *-------------------------------------------------------------------------*/

static void color(Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
}

static void fill(float x, float y, float w, float h)
{
    SDL_FRect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

/* Debug-font text at an integer scale k (stays crisp). */
static void scaled_text(SDL_Renderer *r, float x, float y, const char *s, int k)
{
    SDL_SetRenderScale(r, (float)k, (float)k);
    SDL_RenderDebugText(r, x / k, y / k, s);
    SDL_SetRenderScale(r, 1, 1);
}

static void text(float x, float y, const char *s)
{
    scaled_text(renderer, x, y, s, TEXT_SCALE);
}

static void redraw(void)
{
    if (headless)
        return;
    if (status_until && SDL_GetTicks() >= status_until) {
        status[0] = '\0';
        status_until = 0;
    }
    color(32, 32, 32);
    SDL_RenderClear(renderer);

    SDL_FRect dst = {0, BAR_H, (float)(PANEL_WIDTH * scale), (float)(PANEL_HEIGHT * scale)};
    SDL_UpdateTexture(screen, NULL, frame, PANEL_WIDTH * 2);
    SDL_RenderTexture(renderer, screen, NULL, &dst);

    /* Menu bar */
    int w = PANEL_WIDTH * scale;
    color(240, 240, 240);
    fill(0, 0, (float)w, BAR_H);
    color(200, 200, 200);
    fill(0, BAR_H - 1, (float)w, 1);
    for (int m = 0; m < MENU_COUNT; m++) {
        bool hot = m == open_menu || (open_menu < 0 && title_at(mouse_x, mouse_y) == m);
        if (MENUS[m].button) {
            color(hot ? 204 : 225, hot ? 228 : 225, hot ? 247 : 225);
            fill((float)title_x(m) + 4, 3, (float)title_w(m) - 8, BAR_H - 7);
            color(170, 170, 170);
            SDL_FRect frame_rect = {(float)title_x(m) + 4, 3, (float)title_w(m) - 8, BAR_H - 7};
            SDL_RenderRect(renderer, &frame_rect);
        } else if (hot) {
            color(204, 228, 247);
            fill((float)title_x(m), 0, (float)title_w(m), BAR_H - 1);
        }
        color(0, 0, 0);
        text((float)(title_x(m) + PAD), (BAR_H - GLYPH_H) / 2.0f, MENUS[m].title);
    }
    char right[340];
    snprintf(right, sizeof right, "%s%s%s", paused ? "PAUSED" : "", paused && status[0] ? "  " : "", status);
    if (right[0]) {
        /* Right of the last title; smaller text, then cut, if it doesn't fit. */
        int room = w - title_x(MENU_COUNT - 1) - title_w(MENU_COUNT - 1) - 2 * PAD;
        int k = (int)strlen(right) * CHAR_W <= room ? TEXT_SCALE : 1;
        int fit = room / (8 * k);
        const char *shown = right;
        char cut[sizeof right];
        if (fit > 3 && (int)strlen(right) > fit) {
            snprintf(cut, sizeof cut, "...%s", right + strlen(right) - (size_t)(fit - 3));
            shown = cut;
        }
        color(paused ? 170 : 60, paused ? 0 : 60, 0);
        scaled_text(renderer, (float)(w - PAD - (int)strlen(shown) * 8 * k), (BAR_H - 8.0f * k) / 2,
                    shown, k);
    }

    /* Drop-down */
    if (open_menu >= 0) {
        int x0 = title_x(open_menu), dw = dropdown_w(open_menu);
        int dh = MENUS[open_menu].count * ITEM_H + 2;
        color(160, 160, 160);
        fill((float)x0, BAR_H, (float)dw, (float)dh);
        color(250, 250, 250);
        fill((float)x0 + 1, BAR_H + 1, (float)dw - 2, (float)dh - 2);
        for (int i = 0; i < MENUS[open_menu].count; i++) {
            const Item *it = &MENUS[open_menu].items[i];
            float y = (float)(BAR_H + 1 + i * ITEM_H);
            if (it->action == A_SEPARATOR) {
                color(210, 210, 210);
                fill((float)x0 + PAD, y + ITEM_H / 2.0f, (float)dw - 2 * PAD, 1);
                continue;
            }
            if (i == hover_item) {
                color(0, 120, 215);
                fill((float)x0 + 1, y, (float)dw - 2, ITEM_H);
                color(255, 255, 255);
            } else {
                color(0, 0, 0);
            }
            float ty = y + (ITEM_H - GLYPH_H) / 2.0f;
            if (checked(it->action))
                text((float)x0 + PAD, ty, "*");
            text((float)(x0 + PAD + 2 * CHAR_W), ty, it->label);
            if (i != hover_item)
                color(110, 110, 110);
            text((float)(x0 + dw - PAD - (int)strlen(it->shortcut) * CHAR_W), ty, it->shortcut);
        }
    }
    SDL_RenderPresent(renderer);
}

/*---------------------------------------------------------------------------
 * Add-in info window
 *-------------------------------------------------------------------------*/

#define INFO_W      640
#define INFO_H      260
#define INFO_ICON_X 20
#define INFO_ICON_Y 64
#define INFO_ICON_S 2   /* icons shown at 2x: 184 x 128 */
#define INFO_TEXT   2   /* debug font at 2x: 16 px characters */
#define INFO_ROW    26

static void info_close(void)
{
    for (int i = 0; i < 2; i++) {
        SDL_DestroyTexture(info_icon[i]);
        info_icon[i] = NULL;
    }
    SDL_DestroyRenderer(info_ren);
    SDL_DestroyWindow(info_win);
    info_ren = NULL;
    info_win = NULL;
}

static SDL_Texture *icon_texture(const uint16_t *pixels)
{
    SDL_Texture *t = SDL_CreateTexture(info_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC,
                                       G3A_ICON_W, G3A_ICON_H);
    if (t) {
        SDL_UpdateTexture(t, NULL, pixels, G3A_ICON_W * 2);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    return t;
}

static bool over_icon(float x, float y)
{
    return x >= INFO_ICON_X && x < INFO_ICON_X + G3A_ICON_W * INFO_ICON_S &&
           y >= INFO_ICON_Y && y < INFO_ICON_Y + G3A_ICON_H * INFO_ICON_S;
}

static void info_text(float x, float y, const char *label, const char *value)
{
    SDL_SetRenderDrawColor(info_ren, 100, 100, 100, 255);
    scaled_text(info_ren, x, y, label, INFO_TEXT);
    SDL_SetRenderDrawColor(info_ren, 0, 0, 0, 255);
    scaled_text(info_ren, x + 9 * 8 * INFO_TEXT, y, value[0] ? value : "-", INFO_TEXT);
}

static void info_redraw(void)
{
    if (!info_win)
        return;
    SDL_SetRenderDrawColor(info_ren, 250, 250, 250, 255);
    SDL_RenderClear(info_ren);

    /* Name in triple size */
    SDL_SetRenderDrawColor(info_ren, 0, 0, 0, 255);
    scaled_text(info_ren, INFO_ICON_X, 18, have_app && app.name[0] ? app.name : "(no name)", 3);

    /* Icon, selected version while hovered */
    SDL_FRect box = {INFO_ICON_X - 1, INFO_ICON_Y - 1, G3A_ICON_W * INFO_ICON_S + 2, G3A_ICON_H * INFO_ICON_S + 2};
    SDL_SetRenderDrawColor(info_ren, 180, 180, 180, 255);
    SDL_RenderRect(info_ren, &box);
    SDL_FRect dst = {INFO_ICON_X, INFO_ICON_Y, G3A_ICON_W * INFO_ICON_S, G3A_ICON_H * INFO_ICON_S};
    if (have_app && app.has_icon) {
        SDL_RenderTexture(info_ren, info_icon[info_hover ? 1 : 0], NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(info_ren, 120, 120, 120, 255);
        scaled_text(info_ren, INFO_ICON_X + 36, INFO_ICON_Y + 56, "no icon", INFO_TEXT);
    }
    SDL_SetRenderDrawColor(info_ren, 120, 120, 120, 255);
    scaled_text(info_ren, INFO_ICON_X, INFO_ICON_Y + G3A_ICON_H * INFO_ICON_S + 12,
                info_hover ? "Selected icon" : "Unselected icon (hover it to see the selected one)", 1);

    /* Header fields */
    char size[32];
    snprintf(size, sizeof size, "%u bytes", have_app ? app.size : 0);
    const char *base = strrchr(app_path, '/');
    const char *file = base ? base + 1 : app_path;
    float x = INFO_ICON_X + G3A_ICON_W * INFO_ICON_S + 20, y = INFO_ICON_Y;
    info_text(x, y, "Internal", have_app ? app.internal : "");
    info_text(x, y += INFO_ROW, "Version", have_app ? app.version : "");
    info_text(x, y += INFO_ROW, "Date", have_app ? app.date : "");
    info_text(x, y += INFO_ROW, "Size", have_app ? size : "");
    info_text(x, y += INFO_ROW, "Header", have_app ? app.filename : "");
    info_text(x, y += INFO_ROW, "File", file);
    SDL_RenderPresent(info_ren);
}

/* Opens the window (or brings it to the front) with the current add-in. */
static void info_open(void)
{
    if (headless)
        return;
    if (!info_win) {
        if (!SDL_CreateWindowAndRenderer("Add-in information", INFO_W, INFO_H, 0, &info_win, &info_ren)) {
            fprintf(stderr, "info window: %s\n", SDL_GetError());
            return;
        }
        for (int i = 0; i < 2; i++)
            info_icon[i] = icon_texture(i ? app.icon_sel : app.icon);
        if (have_app && app.has_icon) {
            SDL_Surface *s = SDL_CreateSurfaceFrom(G3A_ICON_W, G3A_ICON_H, SDL_PIXELFORMAT_RGB565, app.icon,
                                                   G3A_ICON_W * 2);
            SDL_SetWindowIcon(info_win, s);
            SDL_DestroySurface(s);
        }
    }
    SDL_RaiseWindow(info_win);
    info_redraw();
}

/* Events for the info window; returns true if it was one. */
static bool info_event(const SDL_Event *e)
{
    if (!info_win)
        return false;
    SDL_WindowID id = SDL_GetWindowID(info_win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID != id)
            return false;
        info_close();
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        if (e->window.windowID != id)
            return false;
        info_redraw();
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.windowID != id)
            return false;
        if (over_icon(e->motion.x, e->motion.y) != info_hover) {
            info_hover = !info_hover;
            info_redraw();
        }
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (e->window.windowID != id)
            return false;
        if (info_hover) {
            info_hover = false;
            info_redraw();
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return e->button.windowID == id;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.windowID != id)
            return false;
        if (e->key.key == SDLK_ESCAPE)
            info_close();
        return true;
    }
    return false;
}

/*---------------------------------------------------------------------------
 * Actions
 *-------------------------------------------------------------------------*/

void platform_status(const char *msg)
{
    snprintf(status, sizeof status, "%s", msg ? msg : "");
    status_until = 0;
    redraw();
}

static void flash_status(const char *msg)
{
    snprintf(status, sizeof status, "%s", msg);
    status_until = SDL_GetTicks() + STATUS_MS;
}

static void set_scale(int s)
{
    scale = s;
    SDL_SetWindowSize(window, PANEL_WIDTH * scale, BAR_H + PANEL_HEIGHT * scale);
}

/* A file name that doesn't exist yet: <stem>-YYYYMMDD-HHMMSS.bmp */
static void timestamped(char *out, size_t cap, const char *stem)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(out, cap, "%s-%s.bmp", stem, stamp);
}

static void SDLCALL open_done(void *user, const char *const *files, int filter)
{
    (void)user; (void)filter;
    if (!files || !files[0])
        return; /* cancelled or no dialog available */
    snprintf(open_path, sizeof open_path, "%s", files[0]);
    SDL_SetAtomicInt(&open_ready, 1);
}

static void message(const char *title, const char *body)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, window);
}

static void run_action(Action a)
{
    static const SDL_DialogFileFilter FILTERS[] = {{"Casio fx-CG add-ins", "g3a"}, {"All files", "*"}};
    char path[256];
    switch (a) {
    case A_OPEN:
        SDL_ShowOpenFileDialog(open_done, NULL, window, FILTERS, 2, NULL, false);
        break;
    case A_SCREENSHOT:
        timestamped(path, sizeof path, "screenshot");
        if (platform_save_screenshot(path)) {
            char msg[300];
            snprintf(msg, sizeof msg, "Saved %s", path);
            flash_status(msg);
        }
        break;
    case A_INFO:
        info_open();
        break;
    case A_SAVE_ICON:
        if (!have_app || !app.has_icon) {
            flash_status("This add-in has no icon");
            break;
        }
        snprintf(path, sizeof path, "%s-icon.bmp", app.name[0] ? app.name : "addin");
        for (char *p = path; *p; p++)
            if (*p == '/' || *p == '\\' || *p == ' ')
                *p = '_';
        if (platform_save_bmp(path, app.icon, G3A_ICON_W, G3A_ICON_H)) {
            char msg[300];
            snprintf(msg, sizeof msg, "Saved %s", path);
            flash_status(msg);
        }
        break;
    case A_QUIT: pending = CMD_QUIT; break;
    case A_PAUSE: paused = !paused; break;
    case A_RESET: pending = CMD_RESET; break;
    case A_ZOOM1: case A_ZOOM2: case A_ZOOM3: case A_ZOOM4: set_scale((int)(a - A_ZOOM1) + 1); break;
    case A_KEYS:
        message("Keyboard controls",
                "Arrows: arrows\nEnter: EXE\nEsc: EXIT\nHome: MENU\nTab: SHIFT\nLeft Alt: ALPHA\n"
                "O: OPTN\nBackspace: DEL\nDelete: AC/ON\nF1-F6: F1-F6\n"
                "0-9 . + - * / ( ): the same keys\nE: x10^x");
        break;
    case A_ABOUT:
        message("About emuCasioFxCG",
                "emuCasioFxCG\n\nAn emulator for Casio fx-CG50 add-ins (.g3a):\n"
                "OS syscalls are emulated on the host, and gint add-ins\n"
                "run on an emulated SH7305 and R61524 display.");
        break;
    case A_NONE: case A_SEPARATOR: break;
    }
}

/* Ctrl+key shortcuts. */
static bool shortcut(SDL_Keycode k)
{
    static const struct { SDL_Keycode key; Action a; } KEYS_[] = {
        {SDLK_O, A_OPEN}, {SDLK_I, A_INFO}, {SDLK_S, A_SCREENSHOT}, {SDLK_Q, A_QUIT}, {SDLK_P, A_PAUSE},
        {SDLK_R, A_RESET}, {SDLK_1, A_ZOOM1}, {SDLK_2, A_ZOOM2}, {SDLK_3, A_ZOOM3}, {SDLK_4, A_ZOOM4},
    };
    for (size_t i = 0; i < sizeof KEYS_ / sizeof KEYS_[0]; i++) {
        if (KEYS_[i].key == k) {
            run_action(KEYS_[i].a);
            return true;
        }
    }
    return false;
}

/* KEYS index for an SDL key, or -1. */
static int key_for(SDL_Keycode k)
{
    for (size_t i = 0; i < sizeof KEYMAP / sizeof KEYMAP[0]; i++)
        if (KEYMAP[i].sdl == k)
            return key_find(KEYMAP[i].name);
    return -1;
}

/* Handles one event. Returns the KEYS index of a calculator key pressed,
 * else -1. */
static int handle_event(const SDL_Event *e)
{
    if (info_event(e))
        return -1;
    switch (e->type) {
    case SDL_EVENT_QUIT:
        pending = CMD_QUIT;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: /* the main window (with two windows open, no QUIT comes) */
        pending = CMD_QUIT;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x = e->motion.x;
        mouse_y = e->motion.y;
        if (open_menu >= 0) {
            int m = title_at(mouse_x, mouse_y);
            if (m >= 0 && !MENUS[m].button)
                open_menu = m;
            hover_item = item_at(mouse_x, mouse_y);
        }
        redraw();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            int m = title_at(e->button.x, e->button.y);
            int i = item_at(e->button.x, e->button.y);
            if (m >= 0 && MENUS[m].button) {
                open_menu = hover_item = -1;
                redraw();
                run_action(MENUS[m].button);
            } else if (m >= 0) {
                open_menu = open_menu == m ? -1 : m;
                hover_item = -1;
            } else if (i >= 0) {
                Action a = MENUS[open_menu].items[i].action;
                open_menu = hover_item = -1;
                redraw();
                run_action(a);
            } else {
                open_menu = hover_item = -1;
            }
            redraw();
        }
        break;
    case SDL_EVENT_KEY_UP:
        held[e->key.scancode] = 0;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat)
            break;
        if (open_menu >= 0) { /* keys close the menu instead of reaching the calculator */
            open_menu = hover_item = -1;
            redraw();
            break;
        }
        if ((e->key.mod & SDL_KMOD_CTRL) && shortcut(e->key.key)) {
            redraw();
            break;
        }
        int k = key_for(e->key.key);
        held[e->key.scancode] = (uint8_t)(k + 1);
        return k;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
        redraw();
        break;
    }
    return -1;
}

/* Turns a finished Open dialog into a command. */
static void check_open(void)
{
    if (SDL_GetAtomicInt(&open_ready)) {
        SDL_SetAtomicInt(&open_ready, 0);
        pending = CMD_OPEN;
    }
}

/*---------------------------------------------------------------------------
 * Interface
 *-------------------------------------------------------------------------*/

/* Borrows SDL's built-in 8x8 debug font by rendering it to a surface once. */
static bool build_font(void)
{
    SDL_Surface *s = SDL_CreateSurface(GLYPH_COUNT * 8, 8, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return false;
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(s);
    if (!r) {
        SDL_DestroySurface(s);
        return false;
    }
    char chars[GLYPH_COUNT + 1];
    for (int i = 0; i < GLYPH_COUNT; i++)
        chars[i] = (char)(FIRST_GLYPH + i);
    chars[GLYPH_COUNT] = '\0';
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDebugText(r, 0, 0, chars);
    SDL_RenderPresent(r);

    for (int g = 0; g < GLYPH_COUNT; g++) {
        for (int y = 0; y < 8; y++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)s->pixels + y * s->pitch);
            uint8_t bits = 0;
            for (int x = 0; x < 8; x++)
                if (row[g * 8 + x] & 0x00808080)
                    bits |= (uint8_t)(0x80 >> x);
            glyphs[g][y] = bits;
        }
    }
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(s);
    return true;
}

bool platform_init(int window_scale, bool no_window)
{
    headless = no_window;
    scale = window_scale;
    if (headless)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if (!build_font()) {
        fprintf(stderr, "font: %s\n", SDL_GetError());
        return false;
    }
    if (headless)
        return true;
    if (!SDL_CreateWindowAndRenderer("emuCasioFxCG", PANEL_WIDTH * scale, BAR_H + PANEL_HEIGHT * scale, 0,
                                     &window, &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return false;
    }
    static uint32_t icon[APP_ICON_SIZE * APP_ICON_SIZE];
    app_icon_pixels(icon);
    SDL_Surface *s = SDL_CreateSurfaceFrom(APP_ICON_SIZE, APP_ICON_SIZE, SDL_PIXELFORMAT_ARGB8888, icon,
                                           APP_ICON_SIZE * 4);
    SDL_SetWindowIcon(window, s);
    SDL_DestroySurface(s);
    screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, PANEL_WIDTH, PANEL_HEIGHT);
    SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_NEAREST);
    return screen != NULL;
}

void platform_shutdown(void)
{
    info_close();
    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void platform_present(const uint16_t *fb)
{
    memcpy(frame, fb, sizeof frame);
    redraw();
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

void platform_set_app(const G3aInfo *info, const char *path)
{
    app = *info;
    have_app = true;
    snprintf(app_path, sizeof app_path, "%s", path);
    if (headless)
        return;
    char title[64];
    snprintf(title, sizeof title, "%s - emuCasioFxCG", app.name[0] ? app.name : "add-in");
    SDL_SetWindowTitle(window, title);
    if (info_win) { /* show the new add-in */
        info_close();
        info_open();
    }
}

bool platform_poll(bool *down)
{
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handle_event(&e);
    check_open();
    if (status_until && SDL_GetTicks() >= status_until)
        redraw();
    for (int i = 0; i < KEY_COUNT; i++)
        down[i] = false;
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (held[sc])
            down[held[sc] - 1] = true;
    return pending == CMD_NONE;
}

int platform_wait_key(void)
{
    while (pending == CMD_NONE) {
        SDL_Event e;
        /* Wake up regularly for the Open dialog and status messages. */
        if (SDL_WaitEventTimeout(&e, 100)) {
            int k = handle_event(&e);
            if (k >= 0 && !paused)
                return k;
        }
        check_open();
        if (status_until && SDL_GetTicks() >= status_until)
            redraw();
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
    check_open();
    if (status_until && SDL_GetTicks() >= status_until)
        redraw();
    return pending == CMD_NONE;
}

Command platform_take_command(void)
{
    Command c = pending;
    pending = CMD_NONE;
    return c;
}

bool platform_has_command(void)
{
    return pending != CMD_NONE;
}

const char *platform_open_path(void)
{
    return open_path;
}

bool platform_paused(void)
{
    return paused;
}

void platform_delay_ms(uint32_t ms)
{
    SDL_Delay(ms);
}

uint64_t platform_ticks_ms(void)
{
    return SDL_GetTicks();
}

const uint8_t *platform_glyph(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < FIRST_GLYPH || u >= FIRST_GLYPH + GLYPH_COUNT)
        u = '?';
    return glyphs[u - FIRST_GLYPH];
}
