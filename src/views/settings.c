/* What the window remembers between runs, in settings.ini in the
 * emulator's data folder (beside the kept OS dump): zoom, position,
 * keyboard, font, language, model, timing, speed, battery, reload, and the
 * folder of the last add-in. Read before the command line's options and
 * written when the window closes. An option only lasts for its run: a
 * value it changed, and the user didn't change again, is saved as it was. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "view.h"

#define SETTINGS_FILE "settings.ini"

/* The zoom of the colour screen (what the window opens with): with the
 * monochrome one shown, the colour zoom that fills the same width. */
static int get_zoom(void)
{
    int z = view_panel_w == PANEL_WIDTH ? view_zoom() : 0;
    if (!z) /* in between: the zoom below */
        z = (int)(view_screen.w / PANEL_WIDTH);
    return z < 1 ? 1 : z > 6 ? 6 : z;
}

static int get_x(void)
{
    int x, y;
    SDL_GetWindowPosition(view_window, &x, &y);
    return x;
}

static int get_y(void)
{
    int x, y;
    SDL_GetWindowPosition(view_window, &x, &y);
    return y;
}

static int get_keyboard(void) { return view_show_keypad; }
static void set_keyboard(int v) { platform_set_keyboard(v); }
static int get_font(void) { return view_font; }
static void set_font(int v) { view_font = v; }
static int get_language(void) { return view_language; }
static void set_language(int v) { view_language = v; }
static int get_model(void) { return (int)view_model_choice; }
static void set_model(int v) { view_model_choice = (ModelChoice)v; }
static int get_realistic(void) { return view_realistic; }
static void set_realistic(int v) { view_realistic = v; }
static int get_speed(void) { return (int)(menubar_speed() * 100 + 0.5); }
static void set_speed(int v) { menubar_set_speed(v / 100.0); }
static int get_speed_row(void) { return view_show_speed; }
static void set_speed_row(int v) { view_show_speed = v; }
static int get_battery(void) { return view_battery_cv; }
static void set_battery(int v) { view_battery_cv = v; }
static int get_sound(void) { return view_sound; }
static void set_sound(int v) { view_sound = v; }
static int get_reload(void) { return view_auto_reload; }
static void set_reload(int v) { view_auto_reload = v; }

static const struct {
    const char *key;
    int (*get)(void);
    void (*set)(int v); /* NULL: the window reads it (settings_int) */
    int min, max;
} SETTINGS[] = {
    {"zoom", get_zoom, NULL, 1, 6},
    {"x", get_x, NULL, INT_MIN, INT_MAX},
    {"y", get_y, NULL, INT_MIN, INT_MAX},
    {"keyboard", get_keyboard, set_keyboard, 0, 1},
    {"font", get_font, set_font, 0, FONT_STYLE_COUNT - 1},
    {"language", get_language, set_language, 0, LANG_COUNT - 1},
    {"model", get_model, set_model, CHOICE_AUTO, CHOICE_FX9750G3},
    {"realistic_timing", get_realistic, set_realistic, 0, 1},
    {"speed", get_speed, set_speed, 0, 400}, /* percent, 0 = as fast as possible */
    {"speed_row", get_speed_row, set_speed_row, 0, 1}, /* after speed, which sets it too */
    {"battery", get_battery, set_battery, 300, 600},
    {"reload", get_reload, set_reload, 0, 1},
    {"sound", get_sound, set_sound, 0, 1},
};
#define SETTING_COUNT ((int)(sizeof SETTINGS / sizeof SETTINGS[0]))

static int loaded[SETTING_COUNT];
static bool has[SETTING_COUNT];
static int start[SETTING_COUNT];
static char folder[1024];

/* The emulator's per-user folder (SDL's "pref" folder): settings and the
 * kept OS dump. It was named after emuCasioFxCG, the emulator's old name:
 * the first time, that folder's files move here, so nothing is lost. */
char *view_pref_dir(void)
{
    static bool checked;
    char *dir = SDL_GetPrefPath("", "emuCasioCalc");
    if (!dir || checked)
        return dir;
    checked = true;

    /* The old folder is next to the new one: same path, old name */
    static const char NEW[] = "emuCasioCalc", OLD[] = "emuCasioFxCG";
    size_t n = strlen(dir), k = sizeof NEW - 1;
    if (n < k + 1 || strncmp(dir + n - k - 1, NEW, k))
        return dir;
    char old[1024], from[1100], to[1100];
    snprintf(old, sizeof old, "%.*s%s%s", (int)(n - k - 1), dir, OLD, dir + n - 1);
    if (!SDL_GetPathInfo(old, NULL))
        return dir;
    static const char *const files[] = {SETTINGS_FILE, "os-dump.bin"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(from, sizeof from, "%s%s", old, files[i]);
        snprintf(to, sizeof to, "%s%s", dir, files[i]);
        if (SDL_GetPathInfo(from, NULL) && !SDL_GetPathInfo(to, NULL))
            SDL_RenamePath(from, to);
    }
    SDL_RemovePath(old); /* only goes if it is now empty */
    return dir;
}

static bool settings_path(char *path, size_t cap)
{
    char *dir = view_pref_dir();
    if (!dir)
        return false;
    snprintf(path, cap, "%s%s", dir, SETTINGS_FILE);
    SDL_free(dir);
    return true;
}

bool platform_load_settings(void)
{
    char path[1024], line[1200];
    if (!settings_path(path, sizeof path))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = '\0';
        const char *value = eq + 1;
        if (!strcmp(line, "folder")) {
            snprintf(folder, sizeof folder, "%s", value);
            continue;
        }
        for (int i = 0; i < SETTING_COUNT; i++) {
            if (strcmp(line, SETTINGS[i].key))
                continue;
            char *end;
            long v = strtol(value, &end, 10);
            if (end == value || *end || v < SETTINGS[i].min || v > SETTINGS[i].max)
                break; /* not a value it can have: left at the default */
            loaded[i] = (int)v;
            has[i] = true;
            if (SETTINGS[i].set)
                SETTINGS[i].set((int)v);
        }
    }
    fclose(f);
    return true;
}

int settings_int(const char *key, int fallback)
{
    for (int i = 0; i < SETTING_COUNT; i++)
        if (!strcmp(key, SETTINGS[i].key))
            return has[i] ? loaded[i] : fallback;
    return fallback;
}

const char *settings_folder(void)
{
    return folder;
}

void settings_started(void)
{
    for (int i = 0; i < SETTING_COUNT; i++)
        start[i] = SETTINGS[i].get();
}

void settings_save(void)
{
    char path[1024], tmp[1100];
    if (!settings_path(path, sizeof path))
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# emuCasioCalc's settings, written when its window closes\n");
    for (int i = 0; i < SETTING_COUNT; i++) {
        int v = SETTINGS[i].get();
        if (has[i] && v == start[i] && start[i] != loaded[i])
            v = loaded[i]; /* set by an option for this run only */
        fprintf(f, "%s=%d\n", SETTINGS[i].key, v);
    }
    /* the folder of the add-in last run, as an absolute path */
    char real[PATH_MAX];
    if (view_app_path[0] && realpath(view_app_path, real)) {
        char *slash = strrchr(real, '/');
        if (slash) {
            *slash = '\0';
            snprintf(folder, sizeof folder, "%s", real[0] ? real : "/");
        }
    }
    if (folder[0])
        fprintf(f, "folder=%s\n", folder);
    bool ok = !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    if (ok)
        rename(tmp, path);
    else
        remove(tmp);
}
