#ifndef EMU_R61524_H
#define EMU_R61524_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Renesas R61524 display controller (fx-CG50 panel: 396x224, RGB565).
 *
 * The CPU talks to it through one 16-bit port at physical 0x14000000; bit 4
 * of port R (PRDR) is the RS line: RS=0 selects a register, RS=1 reads or
 * writes it. Register 0x202 is the pixel data port, written inside the
 * window set by registers 0x210-0x213. The panel is mounted mirrored, so
 * gint sets the horizontal window as 395-xmax .. 395-xmin.
 */
#define PANEL_WIDTH  396
#define PANEL_HEIGHT 224

typedef struct {
    uint16_t fb[PANEL_WIDTH * PANEL_HEIGHT]; /* host-endian RGB565 */
    uint16_t regs[0x800];
    uint16_t index;
    bool rs;
    int x, y;    /* next pixel, in panel coordinates */
    bool dirty;  /* fb changed since the frontend last presented it */
    uint16_t border; /* colour around the OS's VRAM area (DrawFrame) */
} R61524;

void r61524_reset(R61524 *lcd);
void r61524_write(R61524 *lcd, uint16_t value);
uint16_t r61524_read(R61524 *lcd);

/* What Bdisp_PutDisp_DD does: shows the OS's 384x216 VRAM (big-endian
 * RGB565), centred on the panel. */
void r61524_blit_vram(R61524 *lcd, const uint8_t *vram);
/* DrawFrame: repaints the border around the VRAM area. */
void r61524_set_border(R61524 *lcd, uint16_t color);

#endif
