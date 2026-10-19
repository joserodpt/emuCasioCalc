/* Help > About: the emulator's icon, name and version, what it emulates,
 * the projects it builds on, and a link to its repository. Laid out at
 * 100% and drawn at the desktop's scaling, like the Add-in info window. */
#include <stdio.h>
#include <string.h>

#include "app_icon.h"
#include "view.h"

#define ABOUT_W   600
#define ABOUT_H   440
#define MARGIN    28
#define GITHUB    "github.com/joserodpt/emuCasioFxCG"
#define GITHUB_URL "https://" GITHUB

extern const char EMU_BUILD_TIME[], EMU_BUILD_COMMIT[]; /* build_info.c */

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *icon;
static float scale = 1;
static bool link_hover, close_hover, close_down;

/* The pointer: a hand over the link and the button. */
static void set_cursor(bool hand)
{
    static SDL_Cursor *cursors[2];
    if (!cursors[hand])
        cursors[hand] = SDL_CreateSystemCursor(hand ? SDL_SYSTEM_CURSOR_POINTER : SDL_SYSTEM_CURSOR_DEFAULT);
    SDL_SetCursor(cursors[hand]);
}

static const SDL_FRect LINK = {MARGIN, ABOUT_H - 64, 300, 22};
static const SDL_FRect CLOSE = {ABOUT_W - MARGIN - 110, ABOUT_H - 70, 110, 36};

static const struct { const char *what, *who, *licence; } CREDITS[] = {
    {"SDL 3", "the window, input and drawing", "zlib"},
    {"gint and the fxSDK", "Lephenixnoir: gint add-ins, test icons", "MIT"},
    {"WikiPrizm and libfxcg", "the OS syscalls' documentation", ""},
    {"Spleen 2.1.0", "Frederic Cambus: calculator text", "BSD 2-clause"},
    {"GNU Unifont 15.1.05", "Unifoundry: symbols, calculator text", "SIL OFL 1.1"},
    {"Inter 4.1", "Rasmus Andersson: this window, the menus", "SIL OFL 1.1"},
    {"stb_truetype 1.26", "Sean Barrett: font drawing", "public domain"},
};

static void color(Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
}

/* Text with its top at y (size px). */
static void text(float x, float y, const char *s, float px)
{
    view_text_px(ren, x, y + px * 0.45f, s, px);
}

static bool inside(SDL_FRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void redraw(void)
{
    color(250, 250, 250);
    SDL_RenderClear(ren);

    /* Header: icon, name, version, one line */
    SDL_FRect icon_box = {MARGIN, MARGIN, 72, 72};
    if (icon)
        SDL_RenderTexture(ren, icon, NULL, &icon_box);
    float x = MARGIN + 72 + 20;
    color(20, 20, 24);
    text(x, MARGIN + 2, "emuCasioFxCG", 30);
    char version[128];
    snprintf(version, sizeof version, "Version %s (%s), built %s", EMU_VERSION, EMU_BUILD_COMMIT, EMU_BUILD_TIME);
    color(110, 110, 116);
    text(x, MARGIN + 42, version, 15);
    color(50, 50, 56);
    text(x, MARGIN + 64, "An emulator for Casio fx-CG graphing calculator add-ins", 15);

    /* Credits */
    float y = MARGIN + 112;
    color(200, 200, 205);
    SDL_FRect line = {MARGIN, y, ABOUT_W - 2 * MARGIN, 1};
    SDL_RenderFillRect(ren, &line);
    y += 16;
    color(20, 20, 24);
    text(MARGIN, y, "Built with", 17);
    y += 28;
    for (size_t i = 0; i < sizeof CREDITS / sizeof CREDITS[0]; i++, y += 22) {
        color(30, 30, 36);
        text(MARGIN + 4, y, CREDITS[i].what, 14);
        color(100, 100, 108);
        text(MARGIN + 190, y, CREDITS[i].who, 14);
        color(130, 130, 138);
        float lw = view_text_w_px(CREDITS[i].licence, 13);
        text(ABOUT_W - MARGIN - lw, y + 1, CREDITS[i].licence, 13);
    }

    /* The repository link and Close */
    int sdl = SDL_GetVersion();
    char sdlv[48];
    snprintf(sdlv, sizeof sdlv, "SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
             SDL_VERSIONNUM_MICRO(sdl));
    color(140, 140, 148);
    text(MARGIN, ABOUT_H - 92, sdlv, 12);
    if (link_hover)
        color(0, 84, 180);
    else
        color(0, 102, 214);
    text(LINK.x, LINK.y, GITHUB, 15);
    if (link_hover) {
        SDL_FRect u = {LINK.x, LINK.y + 19, view_text_w_px(GITHUB, 15), 1};
        SDL_RenderFillRect(ren, &u);
    }
    color(close_down ? 190 : close_hover ? 214 : 232, close_down ? 190 : close_hover ? 214 : 232,
          close_down ? 196 : close_hover ? 222 : 236);
    SDL_RenderFillRect(ren, &CLOSE);
    color(170, 170, 178);
    SDL_RenderRect(ren, &CLOSE);
    color(20, 20, 24);
    float cw = view_text_w_px("Close", 15);
    view_text_px(ren, CLOSE.x + (CLOSE.w - cw) / 2, CLOSE.y + CLOSE.h / 2, "Close", 15);
    SDL_RenderPresent(ren);
}

void about_window_close(void)
{
    if (!win)
        return;
    SDL_DestroyTexture(icon);
    icon = NULL;
    view_text_forget(ren);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    ren = NULL;
    win = NULL;
    link_hover = close_hover = close_down = false;
}

void about_window_open(void)
{
    if (view_headless)
        return;
    if (!win) {
        scale = view_ui_scale;
        if (!SDL_CreateWindowAndRenderer("About emuCasioFxCG", (int)(ABOUT_W * scale), (int)(ABOUT_H * scale), SDL_WINDOW_HIGH_PIXEL_DENSITY, &win,
                                         &ren)) {
            fprintf(stderr, "about window: %s\n", SDL_GetError());
            return;
        }
        float d = SDL_GetWindowPixelDensity(win); /* 2 on a Retina Mac */
        SDL_SetRenderScale(ren, scale * d, scale * d);
        static uint32_t pixels[APP_ICON_SIZE * APP_ICON_SIZE];
        app_icon_pixels(pixels);
        SDL_Surface *s = SDL_CreateSurfaceFrom(APP_ICON_SIZE, APP_ICON_SIZE, SDL_PIXELFORMAT_ARGB8888, pixels,
                                               APP_ICON_SIZE * 4);
        if (s) {
#ifndef __APPLE__ /* there it is the Dock's icon, already set (and sharper) */
            SDL_SetWindowIcon(win, s);
#endif
            icon = SDL_CreateTextureFromSurface(ren, s);
            SDL_SetTextureScaleMode(icon, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(s);
        }
    }
    SDL_RaiseWindow(win);
    redraw();
}

bool about_window_event(const SDL_Event *e)
{
    if (!win)
        return false;
    SDL_WindowID id = SDL_GetWindowID(win);
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e->window.windowID != id)
            return false;
        about_window_close();
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        if (e->window.windowID != id)
            return false;
        redraw();
        return true;
    case SDL_EVENT_MOUSE_MOTION: {
        if (e->motion.windowID != id)
            return false;
        float x = e->motion.x / scale, y = e->motion.y / scale;
        SDL_FRect link = LINK;
        link.w = view_text_w_px(GITHUB, 15);
        bool lh = inside(link, x, y), ch = inside(CLOSE, x, y);
        if (lh != link_hover || ch != close_hover) {
            link_hover = lh;
            close_hover = ch;
            set_cursor(lh || ch);
            redraw();
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.windowID != id)
            return false;
        if (close_hover) {
            close_down = true;
            redraw();
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.windowID != id)
            return false;
        if (close_down && close_hover) {
            about_window_close();
        } else if (link_hover && !SDL_OpenURL(GITHUB_URL)) {
            view_flash_status("Could not open the browser");
        } else {
            close_down = false;
            redraw();
        }
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (e->window.windowID != id)
            return false;
        if (link_hover || close_hover) {
            link_hover = close_hover = close_down = false;
            set_cursor(false);
            redraw();
        }
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.windowID != id)
            return false;
        if (e->type == SDL_EVENT_KEY_DOWN &&
            (e->key.key == SDLK_ESCAPE || e->key.key == SDLK_RETURN || e->key.key == SDLK_KP_ENTER))
            about_window_close();
        return true;
    }
    return false;
}
