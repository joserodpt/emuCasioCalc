/* The menu bar at the top of the main window: drop-down menus, buttons,
 * Ctrl shortcuts and the actions behind them. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "view.h"

typedef enum {
    A_NONE, A_SEPARATOR, A_OPEN, A_INFO, A_SCREENSHOT, A_SAVE_ICON, A_QUIT, A_PAUSE, A_RESET,
    A_ZOOM1, A_ZOOM2, A_ZOOM3, A_ZOOM4, A_KEYS, A_ABOUT,
    A_MODEL_AUTO, A_MODEL_CG20, A_MODEL_CG50, A_TIMING_REAL, A_TIMING_FAST,
    A_FASTER, A_SLOWER, A_NORMAL_SPEED, A_CUSTOM_SPEED,
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
    {"", "", A_SEPARATOR},
    {"Model: automatic", "", A_MODEL_AUTO},
    {"Model: fx-CG10/20", "", A_MODEL_CG20},
    {"Model: fx-CG50", "", A_MODEL_CG50},
    {"", "", A_SEPARATOR},
    {"Timing: realistic", "", A_TIMING_REAL},
    {"Timing: fast", "", A_TIMING_FAST},
    {"", "", A_SEPARATOR},
    {"Speed: normal (100%)", "Ctrl+0", A_NORMAL_SPEED},
    {"Speed: custom...", "", A_CUSTOM_SPEED},
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

/* Speed steps of the toolbar slider: emulated time / real time, 0 = as
 * fast as possible. */
static const double SPEEDS[] = {0.1, 0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4, 0};
#define SPEED_COUNT   ((int)(sizeof SPEEDS / sizeof SPEEDS[0]))
#define SPEED_NORMAL  4
#define SPEED_LABEL_X PAD                           /* "Speed" */
#define TRACK_X       (PAD + 6 * CHAR_W)            /* slider track */
#define TRACK_W       (100 * TEXT_SCALE)
#define VALUE_X       (TRACK_X + TRACK_W + 2 * PAD) /* "100%" */

static bool dragging;        /* the speed slider's knob */

static int open_menu = -1;   /* index of the dropped-down menu, -1 if none */
static int hover_item = -1;
static float mouse_x, mouse_y;

/* File > Open runs asynchronously; the dialog's callback may come from
 * another thread, so it hands the path over through an atomic flag. */
static char open_path[1024];
static SDL_AtomicInt open_ready;

/*---------------------------------------------------------------------------
 * Layout
 *-------------------------------------------------------------------------*/

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
    return w + 5 * PAD + 2 * CHAR_W; /* check mark column + gap */
}

static int title_at(float x, float y)
{
    if (y < 0 || y >= MENU_H)
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
    if (x < x0 || x >= x0 + dropdown_w(open_menu) || y < MENU_H)
        return -1;
    int i = (int)(y - MENU_H - 1) / ITEM_H;
    if (i < 0 || i >= MENUS[open_menu].count || MENUS[open_menu].items[i].action == A_SEPARATOR)
        return -1;
    return i;
}

static bool checked(Action a)
{
    if (a >= A_ZOOM1 && a <= A_ZOOM4)
        return view_zoom() == (int)(a - A_ZOOM1) + 1;
    if (a >= A_MODEL_AUTO && a <= A_MODEL_CG50)
        return view_model_choice == (ModelChoice)(a - A_MODEL_AUTO);
    if (a == A_NORMAL_SPEED || a == A_CUSTOM_SPEED)
        return view_show_speed == (a == A_CUSTOM_SPEED);
    if (a == A_TIMING_REAL || a == A_TIMING_FAST)
        return view_realistic == (a == A_TIMING_REAL);
    return a == A_PAUSE && view_paused;
}

/*---------------------------------------------------------------------------
 * Drawing
 *-------------------------------------------------------------------------*/

static void color(Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(view_renderer, r, g, b, 255);
}

static void fill(float x, float y, float w, float h)
{
    SDL_FRect rect = {x, y, w, h};
    SDL_RenderFillRect(view_renderer, &rect);
}

static void text(float x, float y, const char *s)
{
    view_text(view_renderer, x, y, s, TEXT_SCALE);
}

/* Status on the right: in the speed row next to the slider when it is
 * shown, else in the menu row after the titles. Smaller text, then cut, if
 * it doesn't fit. */
static void draw_status(int width, const char *right)
{
    int room = view_show_speed ? width - VALUE_X - 5 * CHAR_W - 2 * PAD
                               : width - title_x(MENU_COUNT - 1) - title_w(MENU_COUNT - 1) - 2 * PAD;
    float row = view_show_speed ? MENU_H : 0, row_h = view_show_speed ? TOOL_H : MENU_H;
    int k = (int)strlen(right) * CHAR_W <= room ? TEXT_SCALE : 1;
    int fit = room / (8 * k);
    if (fit <= 3)
        return;
    char cut[400];
    const char *shown = right;
    if ((int)strlen(right) > fit) {
        snprintf(cut, sizeof cut, "...%s", right + strlen(right) - (size_t)(fit - 3));
        shown = cut;
    }
    color(view_paused ? 170 : 60, view_paused ? 0 : 60, 0);
    view_text(view_renderer, (float)(width - PAD - (int)strlen(shown) * 8 * k), row + (row_h - 8.0f * k) / 2,
              shown, k);
}

static void draw_dropdown(void)
{
    int x0 = title_x(open_menu), dw = dropdown_w(open_menu);
    int dh = MENUS[open_menu].count * ITEM_H + 2;
    color(160, 160, 160);
    fill((float)x0, MENU_H, (float)dw, (float)dh);
    color(250, 250, 250);
    fill((float)x0 + 1, MENU_H + 1, (float)dw - 2, (float)dh - 2);
    for (int i = 0; i < MENUS[open_menu].count; i++) {
        const Item *it = &MENUS[open_menu].items[i];
        float y = (float)(MENU_H + 1 + i * ITEM_H);
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

double menubar_speed(void)
{
    return SPEEDS[view_speed];
}

void menubar_set_speed(double speed)
{
    int best = SPEED_NORMAL;
    for (int i = 0; i < SPEED_COUNT; i++) {
        double d = SPEEDS[i] - speed, b = SPEEDS[best] - speed;
        if (d * d < b * b)
            best = i;
    }
    view_speed = best;
    view_show_speed = best != SPEED_NORMAL; /* before the window exists: it is created with the row */
}

static void speed_label(char *out, size_t cap)
{
    if (SPEEDS[view_speed] == 0)
        snprintf(out, cap, "Max");
    else
        snprintf(out, cap, "%d%%", (int)(SPEEDS[view_speed] * 100 + 0.5));
}

static int knob_x(int index)
{
    return TRACK_X + index * TRACK_W / (SPEED_COUNT - 1);
}

/* Slider step nearest to window x. */
static int speed_at(float x)
{
    int i = (int)((x - TRACK_X) * (SPEED_COUNT - 1) / TRACK_W + 0.5f);
    return i < 0 ? 0 : i >= SPEED_COUNT ? SPEED_COUNT - 1 : i;
}

static bool on_slider(float x, float y)
{
    return view_show_speed && y >= MENU_H && y < BAR_H && x >= TRACK_X - 2 * PAD &&
           x <= TRACK_X + TRACK_W + 2 * PAD;
}

/* The speed row under the menus (Emulation > Speed: custom): slider on
 * the left; the status goes on its right. */
static void draw_toolbar(int width)
{
    float mid = MENU_H + TOOL_H / 2.0f;
    color(232, 232, 232);
    fill(0, MENU_H, (float)width, TOOL_H);
    color(200, 200, 200);
    fill(0, BAR_H - 1, (float)width, 1);

    color(0, 0, 0);
    text(SPEED_LABEL_X, mid - GLYPH_H / 2.0f, "Speed");
    /* track with a tick per step, longer at 100% */
    color(150, 150, 150);
    fill(TRACK_X, mid - 1, TRACK_W, 3);
    for (int i = 0; i < SPEED_COUNT; i++) {
        int h = i == SPEED_NORMAL ? 6 * TEXT_SCALE : 3 * TEXT_SCALE;
        fill((float)knob_x(i), mid - h, 1, (float)(2 * h));
    }
    float kw = 5.0f * TEXT_SCALE, kh = TOOL_H - 8.0f;
    if (dragging)
        color(0, 90, 180);
    else
        color(60, 110, 200);
    fill(knob_x(view_speed) - kw / 2, mid - kh / 2, kw, kh);
    char value[16];
    speed_label(value, sizeof value);
    color(0, 0, 0);
    text(VALUE_X, mid - GLYPH_H / 2.0f, value);

}

void menubar_draw(int width, const char *status)
{
    color(240, 240, 240);
    fill(0, 0, (float)width, MENU_H);
    color(200, 200, 200);
    fill(0, MENU_H - 1, (float)width, 1);
    for (int m = 0; m < MENU_COUNT; m++) {
        bool hot = m == open_menu || (open_menu < 0 && title_at(mouse_x, mouse_y) == m);
        if (MENUS[m].button) {
            SDL_FRect box = {(float)title_x(m) + 4, 3, (float)title_w(m) - 8, MENU_H - 7};
            color(hot ? 204 : 225, hot ? 228 : 225, hot ? 247 : 225);
            SDL_RenderFillRect(view_renderer, &box);
            color(170, 170, 170);
            SDL_RenderRect(view_renderer, &box);
        } else if (hot) {
            color(204, 228, 247);
            fill((float)title_x(m), 0, (float)title_w(m), MENU_H - 1);
        }
        color(0, 0, 0);
        text((float)(title_x(m) + PAD), (MENU_H - GLYPH_H) / 2.0f, MENUS[m].title);
    }
    if (view_show_speed)
        draw_toolbar(width);
    char right[400];
    snprintf(right, sizeof right, "%s%s%s", view_paused ? "PAUSED" : "", view_paused && status[0] ? "  " : "",
             status);
    if (right[0])
        draw_status(width, right);
    if (open_menu >= 0)
        draw_dropdown();
}

/*---------------------------------------------------------------------------
 * Actions
 *-------------------------------------------------------------------------*/

/* A file name with the time: <stem>-YYYYMMDD-HHMMSS.bmp */
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

void menubar_poll(void)
{
    if (SDL_GetAtomicInt(&open_ready)) {
        SDL_SetAtomicInt(&open_ready, 0);
        view_pending = CMD_OPEN;
    }
}

const char *menubar_open_path(void)
{
    return open_path;
}

static void message(const char *title, const char *body)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, view_window);
}

static void saved(bool ok, const char *path)
{
    if (ok) {
        char msg[300];
        snprintf(msg, sizeof msg, "Saved %s", path);
        view_flash_status(msg);
    }
}

/* Restarts the add-in; a reset also ends a pause. */
static void request_reset(void)
{
    view_pending = CMD_RESET;
    view_paused = false;
}

static void run_action(Action a)
{
    static const SDL_DialogFileFilter FILTERS[] = {{"Casio fx-CG add-ins", "g3a"}, {"All files", "*"}};
    char path[256];
    switch (a) {
    case A_OPEN:
        SDL_ShowOpenFileDialog(open_done, NULL, view_window, FILTERS, 2, NULL, false);
        break;
    case A_INFO:
        info_window_open();
        break;
    case A_SCREENSHOT:
        timestamped(path, sizeof path, "screenshot");
        saved(platform_save_screenshot(path), path);
        break;
    case A_SAVE_ICON:
        if (!view_have_app || !view_app.has_icon) {
            view_flash_status("This add-in has no icon");
            break;
        }
        snprintf(path, sizeof path, "%s-icon.bmp", view_app.name[0] ? view_app.name : "addin");
        for (char *p = path; *p; p++)
            if (*p == '/' || *p == '\\' || *p == ' ')
                *p = '_';
        saved(platform_save_bmp(path, view_app.icon, G3A_ICON_W, G3A_ICON_H), path);
        break;
    case A_QUIT: view_pending = CMD_QUIT; break;
    case A_PAUSE: view_paused = !view_paused; break;
    case A_RESET: request_reset(); break;
    case A_ZOOM1: case A_ZOOM2: case A_ZOOM3: case A_ZOOM4: view_set_scale((int)(a - A_ZOOM1) + 1); break;
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
    case A_MODEL_AUTO: case A_MODEL_CG20: case A_MODEL_CG50: {
        ModelChoice c = (ModelChoice)(a - A_MODEL_AUTO);
        if (c != view_model_choice) {
            view_model_choice = c;
            if (view_have_app)
                request_reset(); /* run it again on the new model */
        }
        break;
    }
    case A_FASTER: /* the shortcuts show the slider, so the change is visible */
        view_show_speed_row(true);
        if (view_speed < SPEED_COUNT - 1)
            view_speed++;
        break;
    case A_SLOWER:
        view_show_speed_row(true);
        if (view_speed > 0)
            view_speed--;
        break;
    case A_NORMAL_SPEED:
        view_speed = SPEED_NORMAL;
        view_show_speed_row(false);
        break;
    case A_CUSTOM_SPEED: view_show_speed_row(true); break;
    case A_TIMING_REAL: view_realistic = true; break;
    case A_TIMING_FAST: view_realistic = false; break;
    case A_NONE: case A_SEPARATOR: break;
    }
}

/* Ctrl+key shortcuts. */
static bool shortcut(SDL_Keycode k)
{
    static const struct { SDL_Keycode key; Action a; } SHORTCUTS[] = {
        {SDLK_O, A_OPEN}, {SDLK_I, A_INFO}, {SDLK_S, A_SCREENSHOT}, {SDLK_Q, A_QUIT}, {SDLK_P, A_PAUSE},
        {SDLK_R, A_RESET}, {SDLK_EQUALS, A_FASTER}, {SDLK_PLUS, A_FASTER}, {SDLK_MINUS, A_SLOWER},
        {SDLK_0, A_NORMAL_SPEED}, {SDLK_1, A_ZOOM1}, {SDLK_2, A_ZOOM2}, {SDLK_3, A_ZOOM3}, {SDLK_4, A_ZOOM4},
    };
    for (size_t i = 0; i < sizeof SHORTCUTS / sizeof SHORTCUTS[0]; i++) {
        if (SHORTCUTS[i].key == k) {
            run_action(SHORTCUTS[i].a);
            return true;
        }
    }
    return false;
}

/*---------------------------------------------------------------------------
 * Events
 *-------------------------------------------------------------------------*/

static void click(float x, float y)
{
    if (open_menu < 0 && on_slider(x, y)) {
        dragging = true;
        view_speed = speed_at(x);
        view_redraw();
        return;
    }
    int m = title_at(x, y);
    int i = item_at(x, y);
    Action a = A_NONE;
    if (m >= 0 && MENUS[m].button) {
        open_menu = -1;
        a = MENUS[m].button;
    } else if (m >= 0) {
        open_menu = open_menu == m ? -1 : m;
    } else if (i >= 0) {
        a = MENUS[open_menu].items[i].action;
        open_menu = -1;
    } else {
        open_menu = -1;
    }
    hover_item = -1;
    view_redraw();
    run_action(a);
    view_redraw();
}

bool menubar_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x = e->motion.x;
        mouse_y = e->motion.y;
        if (dragging)
            view_speed = speed_at(mouse_x);
        if (open_menu >= 0) {
            int m = title_at(mouse_x, mouse_y);
            if (m >= 0 && !MENUS[m].button)
                open_menu = m;
            hover_item = item_at(mouse_x, mouse_y);
        }
        view_redraw();
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT)
            click(e->button.x, e->button.y);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (dragging) {
            dragging = false;
            view_redraw();
        }
        return true;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat)
            return false;
        if (open_menu >= 0) { /* keys close the menu instead of reaching the calculator */
            open_menu = hover_item = -1;
            view_redraw();
            return true;
        }
        if ((e->key.mod & SDL_KMOD_CTRL) && shortcut(e->key.key)) {
            view_redraw();
            return true;
        }
        return false;
    }
    return false;
}
