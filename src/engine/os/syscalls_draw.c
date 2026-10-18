/*
 * HLE syscalls: drawing into VRAM and onto the display (the _DD calls).
 *
 * From WikiPrizm: the Bdisp_Fill_VRAM areas, Bdisp_AreaClr's modes and
 * targets, the rectangle calls adding 24 to y (status bar), the
 * display_graph structure of Bdisp_WriteGraph*, DirectDrawRectangle in
 * panel coordinates, FrameColor. Our reading where they are silent:
 * AreaClr and WriteGraph coordinates are VRAM pixels (no status offset);
 * the _WB variants behave like the plain ones; 4-bit sprites use the 8
 * PrintXY colours; "halftone" draws every other pixel.
 */
#include <string.h>

#include "display.h"
#include "syscalls_impl.h"

#define FKEY_TOP 192 /* the F-key labels take the last 24 rows */

/* Area operations of Bdisp_AreaClr: 0 white, 1 colour, 2 shade (every
 * other pixel), 4 invert. Coordinates are clipped and may come in any order. */
static void area_op(uint8_t *vram, int x1, int y1, int x2, int y2, int mode, uint16_t color)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            switch (mode) {
            case 0: display_set_pixel(vram, x, y, 0xFFFF); break;
            case 1: display_set_pixel(vram, x, y, color); break;
            case 2: if ((x + y) & 1) display_set_pixel(vram, x, y, color); break;
            case 4: display_set_pixel(vram, x, y, (uint16_t)~display_get_pixel(vram, x, y)); break;
            }
        }
    }
}

/* Runs fn on what the display shows (as VRAM-sized pixels), then puts the
 * result on the display: the _DD calls skip VRAM. */
static uint8_t dd_buffer[VRAM_SIZE];

static uint8_t *dd_begin(Emu *emu)
{
    r61524_read_vram(&emu->lcd, dd_buffer);
    return dd_buffer;
}

static void dd_end(Emu *emu)
{
    r61524_blit_vram(&emu->lcd, dd_buffer);
}

/* Bdisp_Fill_VRAM(color, mode): 1 under the status bar, 2 between the
 * status bar and the F-key labels, 3 and 4 the whole VRAM. */
SC(sc_fill_vram)
{
    int top = 0, bottom = LCD_HEIGHT - 1;
    switch (a[1]) {
    case 1: top = STATUS_HEIGHT; break;
    case 2: top = STATUS_HEIGHT; bottom = FKEY_TOP - 1; break;
    case 3: case 4: break;
    default: return 0;
    }
    display_rect(emu->vram, 0, top, LCD_WIDTH - 1, bottom, (uint16_t)a[0]);
    return 0;
}

/* Bdisp_AreaClr(struct display_fill *area, target, color): area is x1, y1,
 * x2, y2 (ints) and mode (byte). target bit 0: VRAM, bit 1: display. */
SC(sc_area_clr)
{
    Memory *mem = &emu->mem;
    int x1 = (int32_t)mem_read32(mem, a[0]), y1 = (int32_t)mem_read32(mem, a[0] + 4);
    int x2 = (int32_t)mem_read32(mem, a[0] + 8), y2 = (int32_t)mem_read32(mem, a[0] + 12);
    int mode = mem_read8(mem, a[0] + 16);
    if (a[1] & 1)
        area_op(emu->vram, x1, y1, x2, y2, mode, (uint16_t)a[2]);
    if (a[1] & 2) {
        area_op(dd_begin(emu), x1, y1, x2, y2, mode, (uint16_t)a[2]);
        dd_end(emu);
    }
    return 0;
}

/* Bdisp_Rectangle / FilledRectangle(x1, y1, x2, y2, PrintXY colour index)
 * and FilledRectangleFullColor(..., RGB565): y under the status bar. */
static void rectangle(Emu *emu, const uint32_t *a, uint16_t color, bool filled)
{
    int x1 = (int32_t)a[0], y1 = (int32_t)a[1] + STATUS_HEIGHT;
    int x2 = (int32_t)a[2], y2 = (int32_t)a[3] + STATUS_HEIGHT;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (filled) {
        display_rect(emu->vram, x1, y1, x2, y2, color);
        return;
    }
    display_rect(emu->vram, x1, y1, x2, y1, color);
    display_rect(emu->vram, x1, y2, x2, y2, color);
    display_rect(emu->vram, x1, y1, x1, y2, color);
    display_rect(emu->vram, x2, y1, x2, y2, color);
}

SC(sc_rectangle)
{
    rectangle(emu, a, display_text_color((int)(a[4] & 7)), false);
    return 0;
}

SC(sc_filled_rectangle)
{
    rectangle(emu, a, display_text_color((int)(a[4] & 7)), true);
    return 0;
}

SC(sc_filled_rectangle_full)
{
    rectangle(emu, a, (uint16_t)a[4], true);
    return 0;
}

SC(sc_get_point_vram)
{
    return display_get_pixel(emu->vram, (int32_t)a[0], (int32_t)a[1]);
}

/* Bdisp_SetPoint_DD / GetPoint_DD(x, y[, color]): one pixel of the display,
 * in VRAM coordinates. */
SC(sc_set_point_dd)
{
    int x = (int32_t)a[0], y = (int32_t)a[1];
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return 0;
    emu->lcd.fb[(y + VRAM_ORIGIN_Y) * PANEL_WIDTH + x + VRAM_ORIGIN_X] = (uint16_t)a[2];
    emu->lcd.dirty = true;
    return 0;
}

SC(sc_get_point_dd)
{
    int x = (int32_t)a[0], y = (int32_t)a[1];
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return 0;
    return emu->lcd.fb[(y + VRAM_ORIGIN_Y) * PANEL_WIDTH + x + VRAM_ORIGIN_X];
}

/* DirectDrawRectangle(x1, y1, x2, y2, color): filled, on the display in
 * panel coordinates (0-395, 0-223), frame included. */
SC(sc_direct_draw_rectangle)
{
    int x1 = (int32_t)a[0], y1 = (int32_t)a[1], x2 = (int32_t)a[2], y2 = (int32_t)a[3];
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    for (int y = y1 < 0 ? 0 : y1; y <= y2 && y < PANEL_HEIGHT; y++)
        for (int x = x1 < 0 ? 0 : x1; x <= x2 && x < PANEL_WIDTH; x++)
            emu->lcd.fb[y * PANEL_WIDTH + x] = (uint16_t)a[4];
    emu->lcd.dirty = true;
    return 0;
}

/* FrameColor(mode, color): mode 1 sets the screen border to color, 0 to
 * white; returns the border colour. */
SC(sc_frame_color)
{
    if (a[0] == 1)
        r61524_set_border(&emu->lcd, (uint16_t)a[1]);
    else if (a[0] == 0)
        r61524_set_border(&emu->lcd, 0xFFFF);
    return emu->lcd.border;
}

/* Bdisp_PutDisp_DD_stripe(y1, y2): VRAM rows y1..y2 to the display. */
SC(sc_putdisp_stripe)
{
    int y1 = (int32_t)a[0], y2 = (int32_t)a[1];
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    r61524_blit_vram_rows(&emu->lcd, emu->vram, y1, y2);
    int rows = y2 - y1 + 1;
    if (rows > 0)
        sc_spend(emu, rows * LCD_WIDTH * 2 * DMA_LCD_CYCLES_PER_BYTE);
    return 0;
}

/*
 * Bdisp_WriteGraph*(struct display_graph *): x, y, xofs, yofs, width,
 * height (ints, 0-20); colormode (24: 1 = 4 bpp, 2 = RGB565, 3 = 1 bpp);
 * bitmap (28); color_idx1/2/3 (32-34: 1 bpp set, clear, transparent);
 * writemodify (36: 1 normal, 2 reverse, 3 halftone); writekind (37:
 * 1 overwrite, 2 OR, 3 AND, 4 XOR); transparency (40, 0-100).
 */
static uint16_t blend(uint16_t src, uint16_t dst, int t)
{
    int r = ((src >> 11) * (100 - t) + (dst >> 11) * t) / 100;
    int g = ((src >> 5 & 63) * (100 - t) + (dst >> 5 & 63) * t) / 100;
    int b = ((src & 31) * (100 - t) + (dst & 31) * t) / 100;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void write_graph(Emu *emu, uint8_t *vram, uint32_t gd)
{
    Memory *mem = &emu->mem;
    int x = (int32_t)mem_read32(mem, gd), y = (int32_t)mem_read32(mem, gd + 4);
    int xofs = (int32_t)mem_read32(mem, gd + 8), yofs = (int32_t)mem_read32(mem, gd + 12);
    int w = (int32_t)mem_read32(mem, gd + 16), h = (int32_t)mem_read32(mem, gd + 20);
    int colormode = mem_read8(mem, gd + 24);
    uint32_t bitmap = mem_read32(mem, gd + 28);
    int idx1 = mem_read8(mem, gd + 32), idx2 = mem_read8(mem, gd + 33), idx3 = mem_read8(mem, gd + 34);
    int modify = mem_read8(mem, gd + 36), kind = mem_read8(mem, gd + 37);
    int t = (int32_t)mem_read32(mem, gd + 40);
    if (t < 0) t = 0;
    if (t > 100) t = 100;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;

    for (int sy = yofs < 0 ? 0 : yofs; sy < h; sy++) {
        for (int sx = xofs < 0 ? 0 : xofs; sx < w; sx++) {
            int dx = x + sx - xofs, dy = y + sy - yofs;
            if (dx < 0 || dy < 0 || dx >= LCD_WIDTH || dy >= LCD_HEIGHT)
                continue;
            uint16_t c;
            if (colormode == 2) {
                c = mem_read16(mem, bitmap + (uint32_t)(sy * w + sx) * 2);
            } else if (colormode == 3) {
                uint8_t byte = mem_read8(mem, bitmap + (uint32_t)(sy * ((w + 7) / 8) + sx / 8));
                int idx = byte >> (7 - sx % 8) & 1 ? idx1 : idx2;
                if (idx == idx3)
                    continue; /* transparent */
                c = display_text_color(idx);
            } else if (colormode == 1) {
                uint8_t byte = mem_read8(mem, bitmap + (uint32_t)(sy * ((w + 1) / 2) + sx / 2));
                c = display_text_color(sx & 1 ? byte & 15 : byte >> 4);
            } else {
                return;
            }
            if (modify == 2)
                c = (uint16_t)~c;
            else if (modify == 3 && ((dx + dy) & 1))
                continue;
            uint16_t d = display_get_pixel(vram, dx, dy);
            switch (kind) {
            case 2: c |= d; break;
            case 3: c &= d; break;
            case 4: c ^= d; break;
            }
            if (t)
                c = blend(c, d, t);
            display_set_pixel(vram, dx, dy, c);
        }
    }
}

SC(sc_write_graph_vram)
{
    write_graph(emu, emu->vram, a[0]);
    return 0;
}

SC(sc_write_graph_dd)
{
    write_graph(emu, dd_begin(emu), a[0]);
    dd_end(emu);
    return 0;
}
