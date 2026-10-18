#include "platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "keys.h"
#include "r61524.h"

#define FIRST_GLYPH 0x20
#define GLYPH_COUNT 95

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen;
static uint16_t frame[PANEL_WIDTH * PANEL_HEIGHT]; /* last presented frame, host RGB565 */
static uint8_t glyphs[GLYPH_COUNT][8];
static bool headless;
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
};

/* KEYS index for an SDL key, or -1. */
static int key_for(SDL_Keycode k)
{
    for (size_t i = 0; i < sizeof KEYMAP / sizeof KEYMAP[0]; i++)
        if (KEYMAP[i].sdl == k)
            return key_find(KEYMAP[i].name);
    return -1;
}

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
    char text[GLYPH_COUNT + 1];
    for (int i = 0; i < GLYPH_COUNT; i++)
        text[i] = (char)(FIRST_GLYPH + i);
    text[GLYPH_COUNT] = '\0';
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDebugText(r, 0, 0, text);
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

bool platform_init(int scale, bool no_window)
{
    headless = no_window;
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
    if (!SDL_CreateWindowAndRenderer("emuCasioFxCG", PANEL_WIDTH * scale, PANEL_HEIGHT * scale, 0, &window, &renderer)) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderLogicalPresentation(renderer, PANEL_WIDTH, PANEL_HEIGHT, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, PANEL_WIDTH, PANEL_HEIGHT);
    SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_NEAREST);
    return screen != NULL;
}

void platform_shutdown(void)
{
    SDL_DestroyTexture(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void platform_present(const uint16_t *fb)
{
    memcpy(frame, fb, sizeof frame);
    if (headless)
        return;
    SDL_UpdateTexture(screen, NULL, frame, PANEL_WIDTH * 2);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, screen, NULL, NULL);
    SDL_RenderPresent(renderer);
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

void platform_set_app(const char *name, const uint16_t *icon, int w, int h)
{
    if (headless)
        return;
    char title[64];
    snprintf(title, sizeof title, "%s - emuCasioFxCG", name);
    SDL_SetWindowTitle(window, title);
    if (!icon)
        return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB565, (void *)icon, w * 2);
    if (s) {
        SDL_SetWindowIcon(window, s);
        SDL_DestroySurface(s);
    }
}

/* Updates held[] from a key event; returns the KEYS index pressed, or -1. */
static int handle_key(const SDL_Event *e)
{
    if (e->type == SDL_EVENT_KEY_UP) {
        held[e->key.scancode] = 0;
    } else if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        int k = key_for(e->key.key);
        held[e->key.scancode] = (uint8_t)(k + 1);
        return k;
    }
    return -1;
}

bool platform_poll(bool *down)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT)
            return false;
        handle_key(&e);
    }
    for (int i = 0; i < KEY_COUNT; i++)
        down[i] = false;
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (held[sc])
            down[held[sc] - 1] = true;
    return true;
}

int platform_wait_key(void)
{
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e) || e.type == SDL_EVENT_QUIT)
            return -1;
        int k = handle_key(&e);
        if (k >= 0)
            return k;
    }
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
