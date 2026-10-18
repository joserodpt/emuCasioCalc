#include "app_icon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Drawn at SS x the size, then averaged down: smooth edges. The design is
 * on a 64 x 64 grid, scaled to the size asked for. */
#define SS 4

typedef struct {
    float r, g, b, a;
} Px;

static Px *canvas;
static int N;       /* canvas side: size * SS */
static float unit;  /* canvas pixels per grid unit */

static Px rgb(int r, int g, int b)
{
    return (Px){r / 255.0f, g / 255.0f, b / 255.0f, 1};
}

static Px mix(Px a, Px b, float t)
{
    return (Px){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

/* Paints c over pixel (x, y) with coverage `cover` (0-1). */
static void put(int x, int y, Px c, float cover)
{
    if (x < 0 || y < 0 || x >= N || y >= N)
        return;
    Px *p = &canvas[y * N + x];
    float a = c.a * cover;
    p->r = c.r * a + p->r * (1 - a);
    p->g = c.g * a + p->g * (1 - a);
    p->b = c.b * a + p->b * (1 - a);
    p->a = a + p->a * (1 - a);
}

/* Coordinates below are on the 64 x 64 grid (floats). */

/* Rounded rectangle, filled with a vertical gradient from top to bottom. */
static void rrect(float x0, float y0, float x1, float y1, float r, Px top, Px bottom)
{
    for (int y = (int)(y0 * unit); y < (int)(y1 * unit); y++) {
        for (int x = (int)(x0 * unit); x < (int)(x1 * unit); x++) {
            float fx = (x + 0.5f) / unit, fy = (y + 0.5f) / unit;
            float cx = fx < x0 + r ? x0 + r : fx > x1 - r ? x1 - r : fx;
            float cy = fy < y0 + r ? y0 + r : fy > y1 - r ? y1 - r : fy;
            float dx = fx - cx, dy = fy - cy;
            if (dx * dx + dy * dy <= r * r)
                put(x, y, mix(top, bottom, (fy - y0) / (y1 - y0)), 1);
        }
    }
}

static void rect(float x0, float y0, float x1, float y1, Px c)
{
    rrect(x0, y0, x1, y1, 0.01f, c, c);
}

static float wave(float x) { return 40 + 6 * sinf((x - 10) * 0.28f); }

/* A pixel screen: a white LCD in a grey border, showing "CG" in square
 * pixels over a coloured wave. */
static void draw(void)
{
    rrect(3, 3, 61, 61, 12, rgb(150, 154, 162), rgb(116, 120, 130)); /* grey border */
    rrect(8, 8, 56, 56, 6, rgb(255, 255, 255), rgb(236, 238, 242));  /* white screen */
    /* "CG" in 2.6-unit pixels, 3 x 5 letters */
    static const char *glyphs[2][5] = {{"###", "#..", "#..", "#..", "###"}, {"###", "#..", "#.#", "#.#", "###"}};
    float p = 2.6f, ox = 15, oy = 13;
    for (int l = 0; l < 2; l++)
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 3; x++)
                if (glyphs[l][y][x] == '#')
                    rect(ox + l * 4.4f * p + x * p, oy + y * p, ox + l * 4.4f * p + (x + 1) * p - 0.4f,
                         oy + (y + 1) * p - 0.4f, rgb(30, 44, 30));
    /* the wave, in squares, from blue to red */
    for (int i = 0; i < 16; i++) {
        float x = 11 + i * 2.6f, y = wave(x + 1);
        Px c = mix(rgb(40, 100, 230), rgb(230, 60, 60), i / 15.0f);
        rect(x, y, x + 2.2f, y + 2.2f, c);
    }
}

void app_icon_pixels_size(int size, uint32_t *argb)
{
    N = size * SS;
    unit = (float)N / 64;
    canvas = calloc((size_t)N * (size_t)N, sizeof *canvas);
    if (!canvas) {
        memset(argb, 0, (size_t)size * (size_t)size * 4);
        return;
    }
    draw();
    /* average SS x SS blocks, back to straight alpha */
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    Px q = canvas[(y * SS + sy) * N + x * SS + sx];
                    r += q.r;
                    g += q.g;
                    b += q.b;
                    a += q.a;
                }
            }
            float n = SS * SS;
            r /= n;
            g /= n;
            b /= n;
            a /= n;
            if (a > 0) {
                r /= a;
                g /= a;
                b /= a;
            }
            argb[y * size + x] = (uint32_t)(a * 255 + 0.5f) << 24 | (uint32_t)(fminf(r, 1) * 255 + 0.5f) << 16 |
                                 (uint32_t)(fminf(g, 1) * 255 + 0.5f) << 8 | (uint32_t)(fminf(b, 1) * 255 + 0.5f);
        }
    }
    free(canvas);
    canvas = NULL;
}

void app_icon_pixels(uint32_t *argb)
{
    app_icon_pixels_size(APP_ICON_SIZE, argb);
}
