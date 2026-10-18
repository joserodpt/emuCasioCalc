/* The "Add-in information" window: the add-in's name, header fields and menu
 * icon, which switches to its selected version under the mouse. */
#include <stdio.h>
#include <string.h>

#include "view.h"

#define INFO_W      640
#define INFO_H      260
#define ICON_X      20
#define ICON_Y      64
#define ICON_SCALE  2   /* icons shown at 2x: 184 x 128 */
#define TEXT        2   /* debug font at 2x: 16 px characters */
#define ROW         26

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *icons[2]; /* unselected, selected */
static bool hover;            /* mouse over the icon: show the selected one */

static SDL_Texture *icon_texture(const uint16_t *pixels)
{
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, G3A_ICON_W, G3A_ICON_H);
    if (t) {
        SDL_UpdateTexture(t, NULL, pixels, G3A_ICON_W * 2);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    }
    return t;
}

static bool over_icon(float x, float y)
{
    return x >= ICON_X && x < ICON_X + G3A_ICON_W * ICON_SCALE && y >= ICON_Y && y < ICON_Y + G3A_ICON_H * ICON_SCALE;
}

static void field(float x, float y, const char *label, const char *value)
{
    SDL_SetRenderDrawColor(ren, 100, 100, 100, 255);
    view_text(ren, x, y, label, TEXT);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    view_text(ren, x + 9 * 8 * TEXT, y, value[0] ? value : "-", TEXT);
}

static void redraw(void)
{
    const G3aInfo *app = &view_app;
    bool have = view_have_app;
    SDL_SetRenderDrawColor(ren, 250, 250, 250, 255);
    SDL_RenderClear(ren);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    view_text(ren, ICON_X, 18, have && app->name[0] ? app->name : "(no name)", 3);

    /* Icon, selected version while hovered */
    SDL_FRect box = {ICON_X - 1, ICON_Y - 1, G3A_ICON_W * ICON_SCALE + 2, G3A_ICON_H * ICON_SCALE + 2};
    SDL_SetRenderDrawColor(ren, 180, 180, 180, 255);
    SDL_RenderRect(ren, &box);
    SDL_FRect dst = {ICON_X, ICON_Y, G3A_ICON_W * ICON_SCALE, G3A_ICON_H * ICON_SCALE};
    if (have && app->has_icon) {
        SDL_RenderTexture(ren, icons[hover ? 1 : 0], NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(ren, 120, 120, 120, 255);
        view_text(ren, ICON_X + 36, ICON_Y + 56, "no icon", TEXT);
    }
    SDL_SetRenderDrawColor(ren, 120, 120, 120, 255);
    view_text(ren, ICON_X, ICON_Y + 6 * ROW + 12, /* under the icon and the fields */
              hover ? "Selected icon" : "Unselected icon (hover it to see the selected one)", 1);

    /* Header fields */
    char size[32];
    snprintf(size, sizeof size, "%u bytes", have ? app->size : 0);
    const char *base = strrchr(view_app_path, '/');
    const char *file = base ? base + 1 : view_app_path;
    float x = ICON_X + G3A_ICON_W * ICON_SCALE + 20, y = ICON_Y;
    field(x, y, "Internal", have ? app->internal : "");
    field(x, y += ROW, "Version", have ? app->version : "");
    field(x, y += ROW, "Date", have ? app->date : "");
    field(x, y += ROW, "Size", have ? size : "");
    field(x, y += ROW, "Header", have ? app->filename : "");
    field(x, y += ROW, "File", file);
    SDL_RenderPresent(ren);
}

void info_window_close(void)
{
    for (int i = 0; i < 2; i++) {
        SDL_DestroyTexture(icons[i]);
        icons[i] = NULL;
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    ren = NULL;
    win = NULL;
    hover = false;
}

void info_window_open(void)
{
    if (view_headless)
        return;
    if (!win) {
        if (!SDL_CreateWindowAndRenderer("Add-in information", INFO_W, INFO_H, 0, &win, &ren)) {
            fprintf(stderr, "info window: %s\n", SDL_GetError());
            return;
        }
        icons[0] = icon_texture(view_app.icon);
        icons[1] = icon_texture(view_app.icon_sel);
        if (view_have_app && view_app.has_icon) {
            SDL_Surface *s = SDL_CreateSurfaceFrom(G3A_ICON_W, G3A_ICON_H, SDL_PIXELFORMAT_RGB565, view_app.icon,
                                                   G3A_ICON_W * 2);
            SDL_SetWindowIcon(win, s);
            SDL_DestroySurface(s);
        }
    }
    SDL_RaiseWindow(win);
    redraw();
}

void info_window_refresh(void)
{
    if (win) {
        info_window_close();
        info_window_open();
    }
}

bool info_window_event(const SDL_Event *e)
{
    if (!win)
        return false;
    SDL_WindowID id = SDL_GetWindowID(win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID != id)
            return false;
        info_window_close();
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        if (e->window.windowID != id)
            return false;
        redraw();
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (e->window.windowID != id)
            return false;
        if (hover) {
            hover = false;
            redraw();
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.windowID != id)
            return false;
        if (over_icon(e->motion.x, e->motion.y) != hover) {
            hover = !hover;
            redraw();
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return e->button.windowID == id;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.windowID != id)
            return false;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE)
            info_window_close();
        return true;
    }
    return false;
}
