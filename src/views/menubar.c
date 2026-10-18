/* The menu bar at the top of the main window: drop-down menus, buttons,
 * Ctrl shortcuts and the actions behind them. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "view.h"

typedef enum {
    A_NONE, A_SEPARATOR, A_OPEN, A_INFO, A_SCREENSHOT, A_SAVE_ICON, A_QUIT, A_PAUSE, A_RESET,
    A_SEND, A_AUTO_RELOAD, A_KEYPAD,
    A_FONT_CLASSIC, A_FONT_SPLEEN, A_FONT_UNIFONT, /* consecutive, as FONT_STYLE_* */
    A_ZOOM1, A_ZOOM2, A_ZOOM3, A_ZOOM4, A_ZOOM5, A_ZOOM6, /* consecutive */
    A_KEYS, A_ABOUT,
    A_MODEL_AUTO, A_MODEL_CG20, A_MODEL_CG50, A_TIMING_REAL, A_TIMING_FAST,
    A_FASTER, A_SLOWER, A_NORMAL_SPEED, A_CUSTOM_SPEED,
    A_BATTERY_FULL, A_BATTERY_HALF, A_BATTERY_LOW, A_BATTERY_EMPTY, /* consecutive, as BATTERY_LEVEL_CV */
    A_LANG_EN, A_LANG_ES, A_LANG_DE, A_LANG_FR, A_LANG_PT, A_LANG_ZH, /* consecutive, as LANG_* */
} Action;

#define GITHUB_URL "https://github.com/joserodpt/emuCasioFxCG"

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
    {"Copy add-in to calculator...", "", A_SEND},
    {"Reload when the file changes", "", A_AUTO_RELOAD},
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
    {"", "", A_SEPARATOR},
    {"Battery: full (5.20 V)", "", A_BATTERY_FULL},
    {"Battery: half (4.80 V)", "", A_BATTERY_HALF},
    {"Battery: low (4.40 V)", "", A_BATTERY_LOW},
    {"Battery: empty (4.00 V)", "", A_BATTERY_EMPTY},
};
static const Item VIEW_ITEMS[] = {
    {"Keyboard", "Ctrl+K", A_KEYPAD},
    {"", "", A_SEPARATOR},
    {"Font: Classic", "", A_FONT_CLASSIC},
    {"Font: Spleen", "", A_FONT_SPLEEN},
    {"Font: Unifont", "", A_FONT_UNIFONT},
    {"", "", A_SEPARATOR},
    {"Zoom 1x", "Ctrl+1", A_ZOOM1},
    {"Zoom 2x", "Ctrl+2", A_ZOOM2},
    {"Zoom 3x", "Ctrl+3", A_ZOOM3},
    {"Zoom 4x", "Ctrl+4", A_ZOOM4},
    {"Zoom 5x", "Ctrl+5", A_ZOOM5},
    {"Zoom 6x", "Ctrl+6", A_ZOOM6},
};
/* The calculator's message language (System > Language on the calculator) */
static const Item LANGUAGE_ITEMS[] = {
    {"English", "", A_LANG_EN},
    {"Espanol (Spanish)", "", A_LANG_ES},
    {"Deutsch (German)", "", A_LANG_DE},
    {"Francais (French)", "", A_LANG_FR},
    {"Portugues (Portuguese)", "", A_LANG_PT},
    {"Chinese", "", A_LANG_ZH},
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
    MENU("Language", LANGUAGE_ITEMS),
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
    if (a >= A_ZOOM1 && a <= A_ZOOM6)
        return view_zoom() == (int)(a - A_ZOOM1) + 1;
    if (a >= A_MODEL_AUTO && a <= A_MODEL_CG50)
        return view_model_choice == (ModelChoice)(a - A_MODEL_AUTO);
    if (a == A_NORMAL_SPEED || a == A_CUSTOM_SPEED)
        return view_show_speed == (a == A_CUSTOM_SPEED);
    if (a == A_TIMING_REAL || a == A_TIMING_FAST)
        return view_realistic == (a == A_TIMING_REAL);
    if (a == A_AUTO_RELOAD)
        return view_auto_reload;
    if (a == A_KEYPAD)
        return view_show_keypad;
    if (a >= A_FONT_CLASSIC && a <= A_FONT_UNIFONT)
        return view_font == (int)(a - A_FONT_CLASSIC);
    if (a >= A_LANG_EN && a <= A_LANG_ZH)
        return view_language == (int)(a - A_LANG_EN);
    if (a >= A_BATTERY_FULL && a <= A_BATTERY_EMPTY)
        return view_battery_cv == BATTERY_LEVEL_CV[a - A_BATTERY_FULL];
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

/*
 * The status row under the bar, like the calculator's status bar: SHIFT and
 * ALPHA (yellow and red as on the keys, dim when off), the battery
 * (click it for the next level, Emulation > Battery to pick one) and the
 * add-in's status message.
 */
#define GAP (4 * TEXT_SCALE)

static int badge_w(const char *s)
{
    return (int)strlen(s) * CHAR_W + 6 * TEXT_SCALE;
}

static void badge(float x, const char *s, bool on, int r, int g, int b, bool light_text)
{
    float h = GLYPH_H + 6, y = STATUS_Y + (STATUS_H - h) / 2;
    SDL_FRect box = {x, y, (float)badge_w(s), h};
    if (on) {
        color(r, g, b);
        SDL_RenderFillRect(view_renderer, &box);
        color(light_text ? 255 : 0, light_text ? 255 : 0, light_text ? 255 : 0);
    } else {
        color(205, 205, 205);
        SDL_RenderRect(view_renderer, &box);
        color(175, 175, 175);
    }
    text(x + 3 * TEXT_SCALE, STATUS_Y + (STATUS_H - GLYPH_H) / 2.0f, s);
}

static const char *const LEVEL_NAMES[BATTERY_LEVELS] = {"full", "half", "low", "empty"};

/* Index of the battery level nearest to the current voltage. */
static int battery_level(void)
{
    int best = 0;
    for (int i = 1; i < BATTERY_LEVELS; i++) {
        int d = BATTERY_LEVEL_CV[i] - view_battery_cv, b = BATTERY_LEVEL_CV[best] - view_battery_cv;
        if (d * d < b * b)
            best = i;
    }
    return best;
}

static float battery_x(void)
{
    return (float)(PAD + badge_w("SHIFT") + GAP + badge_w("ALPHA LOCK") + 3 * GAP);
}

static int battery_w(void)
{
    return 15 * TEXT_SCALE + GAP + 12 * CHAR_W; /* icon, "5.20V (full)" */
}

static bool on_battery(float x, float y)
{
    return y >= STATUS_Y && y < BAR_H && x >= battery_x() - GAP && x < battery_x() + battery_w();
}

static void draw_status_row(int width)
{
    color(246, 246, 246);
    fill(0, STATUS_Y, (float)width, STATUS_H);
    color(200, 200, 200);
    fill(0, BAR_H - 1, (float)width, 1);

    float x = PAD;
    badge(x, "SHIFT", view_key_shift, 240, 190, 0, false);
    x += badge_w("SHIFT") + GAP;
    const char *alpha = view_key_alpha == 2 ? "ALPHA LOCK" : "ALPHA";
    badge(x, alpha, view_key_alpha != 0, 200, 30, 30, true);

    /* Battery: body, terminal, 3 bars, as on the status bar */
    int k = TEXT_SCALE;
    x = battery_x();
    float bh = 8.0f * k, by = STATUS_Y + (STATUS_H - bh) / 2;
    bool hot = open_menu < 0 && on_battery(mouse_x, mouse_y);
    color(hot ? 0 : 60, hot ? 90 : 60, hot ? 180 : 60);
    SDL_FRect body = {x, by, 13.0f * k, bh};
    SDL_RenderRect(view_renderer, &body);
    fill(x + 13.0f * k, by + 2.0f * k, 2.0f * k, bh - 4.0f * k);
    int bars = battery_bars(view_battery_cv);
    if (bars <= 1)
        color(200, 30, 30);
    else
        color(40, 160, 40);
    for (int i = 0; i < bars; i++)
        fill(x + (1.0f + 4.0f * i) * k, by + 1.0f * k, 3.0f * k, bh - 2.0f * k);
    x += 15 * k + GAP;
    char volts[32];
    snprintf(volts, sizeof volts, "%d.%02dV (%s)", view_battery_cv / 100, view_battery_cv % 100,
             LEVEL_NAMES[battery_level()]);
    color(0, 0, 0);
    text(x, STATUS_Y + (STATUS_H - GLYPH_H) / 2.0f, volts);

    /* The message language, then the add-in's status message, cut to the
     * room left */
    x = battery_x() + battery_w() + 2 * GAP;
    char lang[8];
    snprintf(lang, sizeof lang, "%s", LANG_CODES[view_language]);
    lang[0] = (char)(lang[0] - 'a' + 'A');
    lang[1] = (char)(lang[1] - 'a' + 'A');
    color(60, 60, 60);
    text(x, STATUS_Y + (STATUS_H - GLYPH_H) / 2.0f, lang);
    x += 2 * CHAR_W + 3 * GAP;
    int fit = (int)((float)width - PAD - x) / CHAR_W;
    if (view_os_status[0] && fit > 3) {
        char msg[80];
        snprintf(msg, sizeof msg, "\"%.*s\"", fit - 2, view_os_status);
        color(70, 70, 70);
        text(x, STATUS_Y + (STATUS_H - GLYPH_H) / 2.0f, msg);
    }
}

/* Status on the right: in the speed row next to the slider when it is
 * shown, else in the menu row after the titles. Smaller text, then cut, if
 * it doesn't fit. */
static void draw_status(int width, const char *right)
{
    int right_edge = width;
    int room = view_show_speed ? width - VALUE_X - 5 * CHAR_W - 2 * PAD
                               : right_edge - title_x(MENU_COUNT - 1) - title_w(MENU_COUNT - 1) - 2 * PAD;
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
    view_text(view_renderer, (float)(right_edge - PAD - (int)strlen(shown) * 8 * k), row + (row_h - 8.0f * k) / 2,
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
    return view_show_speed && y >= MENU_H && y < MENU_H + TOOL_H && x >= TRACK_X - 2 * PAD &&
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
    fill(0, MENU_H + TOOL_H - 1, (float)width, 1);

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
    draw_status_row(width);
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
    /* NULL: no dialog available; no entry or an empty one (some dialog
     * backends): cancelled */
    if (!files || !files[0] || !files[0][0])
        return;
    snprintf(open_path, sizeof open_path, "%s", files[0]);
    SDL_SetAtomicInt(&open_ready, 1);
}

void menubar_poll(void)
{
    send_addin_poll();
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
    case A_SEND:
        send_addin_start();
        break;
    case A_AUTO_RELOAD:
        view_auto_reload = !view_auto_reload;
        break;
    case A_KEYPAD:
        view_show_keypad_panel(!view_show_keypad);
        break;
    case A_FONT_CLASSIC: case A_FONT_SPLEEN: case A_FONT_UNIFONT:
        view_font = (int)(a - A_FONT_CLASSIC);
        view_flash_status("Font changed: the add-in's next text uses it");
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
    case A_ZOOM1: case A_ZOOM2: case A_ZOOM3: case A_ZOOM4: case A_ZOOM5: case A_ZOOM6:
        view_set_scale((int)(a - A_ZOOM1) + 1);
        break;
    case A_KEYS:
        message("Keyboard controls",
                "Arrows: arrows\nEnter: EXE\nEsc: EXIT\nHome: MENU\nTab: SHIFT\nLeft Alt: ALPHA\n"
                "O: OPTN\nV: VARS\nBackspace: DEL\nDelete: AC/ON\nF1-F6: F1-F6\n"
                "0-9 . + - * / ( ) ,: the same keys\nE: x10^x\nU: (-)\n"
                "X: X,theta,T\nL: log\nN: ln\nS C T: sin cos tan\n"
                "F: a b/c\nD: F<>D\nA: ->\nQ: x^2\nP: ^\n\n"
                "In OS add-ins, SHIFT and ALPHA work as on the calculator\n"
                "(SHIFT then ALPHA locks ALPHA; the menu bar shows the mode).\n"
                "With ALPHA on, PC letters and space type those characters.");
        break;
    case A_ABOUT: {
        static const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open on GitHub"},
        };
        const SDL_MessageBoxData box = {
            SDL_MESSAGEBOX_INFORMATION, view_window, "About emuCasioFxCG",
            "emuCasioFxCG\n\nAn emulator for Casio fx-CG50 add-ins (.g3a):\n"
            "OS syscalls are emulated on the host, and gint add-ins\n"
            "run on an emulated SH7305 and R61524 display.\n\n" GITHUB_URL,
            2, buttons, NULL};
        int pressed = 0;
        if (SDL_ShowMessageBox(&box, &pressed) && pressed == 1 && !SDL_OpenURL(GITHUB_URL))
            view_flash_status("Could not open the browser");
        break;
    }
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
    case A_LANG_EN: case A_LANG_ES: case A_LANG_DE: case A_LANG_FR: case A_LANG_PT: case A_LANG_ZH:
        view_language = (int)(a - A_LANG_EN);
        break;
    case A_BATTERY_FULL: case A_BATTERY_HALF: case A_BATTERY_LOW: case A_BATTERY_EMPTY:
        view_battery_cv = BATTERY_LEVEL_CV[a - A_BATTERY_FULL];
        break;
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
        {SDLK_5, A_ZOOM5}, {SDLK_6, A_ZOOM6}, {SDLK_K, A_KEYPAD},
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
    if (open_menu < 0 && on_battery(x, y)) {
        view_battery_cv = BATTERY_LEVEL_CV[(battery_level() + 1) % BATTERY_LEVELS];
        view_redraw();
        return;
    }
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
