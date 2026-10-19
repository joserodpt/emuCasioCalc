#ifndef EMU_T6K11_H
#define EMU_T6K11_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/*
 * Toshiba T6K11 display controller (fx-9860G panel: 128x64, 1 bit per
 * pixel), as gint drives it (src/t6k11/t6k11.c).
 *
 * Two byte ports at physical 0x14000000: address bit 16 is the RS line.
 * RS=0 (0xB4000000) selects a register (and reads the status); RS=1
 * (0xB4010000) writes the selected register or, for the data register,
 * 8 pixels at the current position (bit 7 leftmost). Two versions:
 * - 1, the fx-9860GII: register 4 is the row (0x80 | 0x40 | row) or the
 *   column byte (0..15), register 1 the counter mode, register 7 the data;
 * - 2, the Graph 35+E II / fx-9750GIII: register 8 takes the row (0x80 |
 *   row), register 10 is the data.
 * Their register numbers don't overlap, so both are understood at once.
 *
 * Grey: gint's grey engine flickers between two pictures on a timer (one
 * cycle is about 11.6 ms at its default delays), and the LCD's slow pixels
 * average them. Here a pixel that keeps switching (a full dark-light cycle
 * no longer than GREY_MAX_MS, the last switch at most two cycles ago) shows
 * the share of its last cycle it was dark, as a shade: the same at every
 * render, wherever renders fall in the cycle. Any other pixel shows what
 * it is, so black and white change at once.
 */
#define MONO_WIDTH  128
#define MONO_HEIGHT 64
#define MONO_VRAM_SIZE (MONO_WIDTH * MONO_HEIGHT / 8) /* the OS's VRAM: rows of 16 bytes */
#define MONO_PHYS_SEL 0x14000000u /* RS = 0 */
#define MONO_PHYS_CMD 0x14010000u /* RS = 1 */

typedef struct {
    uint8_t pixels[MONO_HEIGHT][MONO_WIDTH / 8]; /* what the panel shows, 1 = dark */
    uint8_t reg;          /* the selected register */
    int row, col;         /* where the next data byte goes (col in bytes) */
    int counter;          /* v1 counter mode: 0 col-, 1 col+, 2 row-, 3 row+ */
    bool dirty;           /* to render again: changed, or some pixels are grey */
    const uint64_t *now;  /* the emulated clock (CPU cycles) */
    uint64_t grey_max;    /* GREY_MAX_MS, in cycles */
    /* Grey: each pixel's last switches, and its last full cycle. */
    uint64_t rise[MONO_WIDTH * MONO_HEIGHT];    /* when it last turned dark (0: never) */
    uint64_t fall[MONO_WIDTH * MONO_HEIGHT];    /* ...and light */
    uint32_t period[MONO_WIDTH * MONO_HEIGHT];  /* dark to dark, cycles (0: none yet) */
    uint32_t dark_len[MONO_WIDTH * MONO_HEIGHT]; /* the dark part of it */
} T6k11;

#define GREY_MAX_MS 50

/* hz: the CPU clock (cycles per second of *now). */
void t6k11_reset(T6k11 *lcd, const uint64_t *now, uint32_t hz);
/* What the OS's Bdisp_PutDisp_DD does: shows a 1 kB VRAM (rows of 16
 * bytes, bit 7 leftmost, 1 = dark). */
void t6k11_show(T6k11 *lcd, const uint8_t *vram);
/* The inverse (Bdisp_GetDisp_DD): what the panel shows, as a VRAM. */
void t6k11_read(const T6k11 *lcd, uint8_t *vram);
/* Maps the two ports (P2) on mem. */
bool t6k11_map(T6k11 *lcd, Memory *mem);
/* The picture as RGB565 (grey pixels shaded). Returns whether out changed;
 * lcd->dirty stays set while pixels are grey, to notice when they stop. */
bool t6k11_render(T6k11 *lcd, uint16_t *out);

#endif
