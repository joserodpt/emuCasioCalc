#include "app_icon.h"

#include <math.h>
#include <stdbool.h>

#define N APP_ICON_SIZE

static uint32_t argb(int r, int g, int b)
{
    return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* Filled rectangle [x0,x1] x [y0,y1] with corners rounded by radius r. */
static void round_rect(uint32_t *px, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int cx = x < x0 + r ? x0 + r : x > x1 - r ? x1 - r : x;
            int cy = y < y0 + r ? y0 + r : y > y1 - r ? y1 - r : y;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r)
                px[y * N + x] = c;
        }
    }
}

void app_icon_pixels(uint32_t *px)
{
    for (int i = 0; i < N * N; i++)
        px[i] = 0; /* transparent */

    /* Body with a darker rim */
    round_rect(px, 10, 1, 53, 62, 7, argb(20, 24, 34));
    round_rect(px, 11, 2, 52, 61, 6, argb(40, 48, 68));

    /* Screen: bezel, then a light-blue display with a graph */
    round_rect(px, 14, 5, 49, 26, 2, argb(10, 12, 18));
    for (int y = 7; y <= 24; y++)
        for (int x = 16; x <= 47; x++)
            px[y * N + x] = argb(205 - (y - 7) * 2, 228 - (y - 7), 250);
    for (int x = 16; x <= 47; x++) {
        int y = 16 + (int)lround(5.0 * sin((x - 16) * 0.35));
        px[y * N + x] = argb(20, 70, 170);
        px[(y + 1) * N + x] = argb(20, 70, 170);
    }

    /* Function keys */
    for (int i = 0; i < 6; i++)
        round_rect(px, 14 + i * 6, 30, 17 + i * 6, 32, 1, argb(90, 150, 220));

    /* Keypad: 5 columns x 4 rows, EXE in orange */
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 5; col++) {
            int x = 14 + col * 7, y = 37 + row * 6;
            bool exe = row == 3 && col == 4;
            round_rect(px, x, y, x + 5, y + 3, 1, exe ? argb(245, 150, 40) : argb(210, 214, 224));
        }
    }
}
