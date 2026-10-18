/* The emulator's own text (menus, status row, keyboard, info window): Inter
 * Medium (assets/fonts, SIL OFL 1.1), embedded at build time, drawn with
 * stb_truetype (third_party, public domain) from a glyph atlas baked once
 * per window and size.
 *
 * view_text keeps the old interface: text at scale k fills a box 8k pixels
 * high (what the 8x8 debug font took), its capitals centred in it, in the
 * renderer's draw colour. Widths are measured with view_text_w. */
#include <math.h>
#include <stdio.h>
#include <string.h>

/* third-party code: not held to this project's warnings */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb_truetype.h"
#pragma GCC diagnostic pop

#include "view.h"

extern const unsigned char UI_FONT_TTF[];
extern const size_t UI_FONT_TTF_SIZE;

#define FIRST_CHAR 32
#define CHAR_COUNT 224 /* ASCII and Latin-1 */
#define PX_PER_K 11.5f /* font pixel height per unit of k */
#define ATLAS 1024

static stbtt_fontinfo font;
static bool font_ok;
static float cap_per_px; /* height of 'H' per pixel of font size */

typedef struct {
    SDL_Renderer *r;
    int px;              /* font size, whole pixels */
    unsigned last_use;
    SDL_Texture *tex;
    stbtt_packedchar chars[CHAR_COUNT];
} Atlas;

#define ATLAS_MAX 16
static Atlas atlases[ATLAS_MAX];
static int atlas_count;
static unsigned use_clock;

static bool ui_font_init(void)
{
    static bool tried;
    if (tried)
        return font_ok;
    tried = true;
    if (!stbtt_InitFont(&font, UI_FONT_TTF, stbtt_GetFontOffsetForIndex(UI_FONT_TTF, 0)))
        return false;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1);
    cap_per_px = (float)y1 * stbtt_ScaleForPixelHeight(&font, 1.0f);
    font_ok = true;
    return true;
}

/* The atlas of renderer r at size px, baked the first time; with the cache
 * full, the least recently used one goes. */
static Atlas *atlas_for(SDL_Renderer *r, int px)
{
    use_clock++;
    for (int i = 0; i < atlas_count; i++) {
        if (atlases[i].r == r && atlases[i].px == px) {
            atlases[i].last_use = use_clock;
            return &atlases[i];
        }
    }
    if (!ui_font_init() || px < 4 || px > 120)
        return NULL;
    Atlas *a;
    if (atlas_count < ATLAS_MAX) {
        a = &atlases[atlas_count++];
    } else {
        a = &atlases[0];
        for (int i = 1; i < ATLAS_MAX; i++)
            if (atlases[i].last_use < a->last_use)
                a = &atlases[i];
        SDL_DestroyTexture(a->tex);
    }
    memset(a, 0, sizeof *a);
    static unsigned char pixels[ATLAS * ATLAS];
    memset(pixels, 0, sizeof pixels);
    stbtt_pack_context pc;
    bool ok = stbtt_PackBegin(&pc, pixels, ATLAS, ATLAS, 0, 1, NULL);
    if (ok) {
        stbtt_PackSetOversampling(&pc, 2, 1); /* sharper at fractional positions */
        ok = stbtt_PackFontRange(&pc, UI_FONT_TTF, 0, (float)px, FIRST_CHAR, CHAR_COUNT, a->chars);
        stbtt_PackEnd(&pc);
    }
    SDL_Texture *t = ok ? SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS) : NULL;
    if (!t) {
        *a = atlases[--atlas_count];
        return NULL;
    }
    static uint8_t rgba[ATLAS * ATLAS * 4];
    for (int i = 0; i < ATLAS * ATLAS; i++) {
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 255;
        rgba[4 * i + 3] = pixels[i];
    }
    SDL_UpdateTexture(t, NULL, rgba, ATLAS * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    a->r = r;
    a->px = px;
    a->tex = t;
    a->last_use = use_clock;
    return a;
}

static int glyph_index(unsigned char c)
{
    return c >= FIRST_CHAR ? c - FIRST_CHAR : '?' - FIRST_CHAR;
}

static int size_px(float px)
{
    return (int)(px + 0.5f);
}

float view_text_w_px(const char *s, float px)
{
    if (!ui_font_init())
        return (float)strlen(s) * px * 0.6f;
    float scale = stbtt_ScaleForPixelHeight(&font, (float)size_px(px)), w = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, *c >= FIRST_CHAR ? *c : '?', &adv, &lsb);
        w += (float)adv * scale;
        if (c[1])
            w += (float)stbtt_GetCodepointKernAdvance(&font, c[0], c[1]) * scale;
    }
    return w;
}

void view_text_px(SDL_Renderer *r, float x, float cy, const char *s, float size)
{
    int px = size_px(size);
    Atlas *a = atlas_for(r, px);
    if (!a) { /* fallback: SDL's 8x8 debug font */
        SDL_RenderDebugText(r, x, cy - 4, s);
        return;
    }
    Uint8 cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_SetTextureColorMod(a->tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(a->tex, ca);
    float scale = stbtt_ScaleForPixelHeight(&font, (float)px);
    float bx = roundf(x), by = roundf(cy + cap_per_px * (float)px / 2); /* baseline */
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(a->chars, ATLAS, ATLAS, glyph_index(*c), &bx, &by, &q, 0);
        SDL_FRect src = {q.s0 * ATLAS, q.t0 * ATLAS, (q.s1 - q.s0) * ATLAS, (q.t1 - q.t0) * ATLAS};
        SDL_FRect dst = {q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        SDL_RenderTexture(r, a->tex, &src, &dst);
        if (c[1])
            bx += (float)stbtt_GetCodepointKernAdvance(&font, c[0], c[1]) * scale;
    }
}

float view_text_w(const char *s, int k)
{
    return view_text_w_px(s, PX_PER_K * (float)k);
}

void view_text(SDL_Renderer *r, float x, float y, const char *s, int k)
{
    view_text_px(r, x, y + 4.0f * k, s, PX_PER_K * (float)k);
}

/* A window is going away: drop its atlases. */
void view_text_forget(SDL_Renderer *r)
{
    for (int i = 0; i < atlas_count; i++) {
        if (atlases[i].r == r) {
            SDL_DestroyTexture(atlases[i].tex);
            atlases[i] = atlases[--atlas_count];
            i--;
        }
    }
}
