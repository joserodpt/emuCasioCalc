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
 * Grey: gint's grey engine flickers between two pictures many times a
 * second, and the LCD's slow pixels average them. Here each pixel's dark
 * time is counted between two renders, and shown as a shade.
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
    bool dirty;           /* changed since the last render */
    bool still;           /* nothing drawn yet: time starts counting at the first change */
    const uint64_t *now;  /* the emulated clock (CPU cycles) */
    /* Grey: since `since`, each pixel's dark time up to dark_from[]. */
    uint64_t since;
    uint64_t dark[MONO_WIDTH * MONO_HEIGHT];      /* cycles dark, counted */
    uint64_t dark_from[MONO_WIDTH * MONO_HEIGHT]; /* dark since then (if dark) */
} T6k11;

void t6k11_reset(T6k11 *lcd, const uint64_t *now);
/* What the OS's Bdisp_PutDisp_DD does: shows a 1 kB VRAM (rows of 16
 * bytes, bit 7 leftmost, 1 = dark). */
void t6k11_show(T6k11 *lcd, const uint8_t *vram);
/* The inverse (Bdisp_GetDisp_DD): what the panel shows, as a VRAM. */
void t6k11_read(const T6k11 *lcd, uint8_t *vram);
/* Maps the two ports (P2) on mem. */
bool t6k11_map(T6k11 *lcd, Memory *mem);
/* The picture since the last render, as RGB565: each pixel the shade of
 * the share of the time it was dark. */
void t6k11_render(T6k11 *lcd, uint16_t *out);

#endif
