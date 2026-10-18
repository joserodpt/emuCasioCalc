/* The 8x8 font the OS text syscalls draw with (platform_glyph): SDL's
 * built-in debug font, read back once from a software renderer. */
#include <SDL3/SDL.h>

#include "view.h"

#define FIRST_GLYPH 0x20
#define GLYPH_COUNT 95

static uint8_t glyphs[GLYPH_COUNT][8];

bool font_init(void)
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

const uint8_t *platform_glyph(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < FIRST_GLYPH || u >= FIRST_GLYPH + GLYPH_COUNT)
        u = '?';
    return glyphs[u - FIRST_GLYPH];
}
