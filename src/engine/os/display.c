#include "display.h"

#include "mem.h"
#include "platform.h"

/* TEXT_COLOR_BLACK, BLUE, GREEN, CYAN, RED, PURPLE, YELLOW, WHITE */
static const uint16_t TEXT_COLORS[8] = {0x0000, 0x001F, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0xFFE0, 0xFFFF};

uint16_t display_text_color(int index)
{
    return TEXT_COLORS[index & 7];
}

void display_fill(uint8_t *vram, uint16_t color)
{
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        vram[2 * i] = color >> 8;
        vram[2 * i + 1] = color & 0xFF;
    }
}

void display_set_pixel(uint8_t *vram, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    uint8_t *p = vram + 2 * (y * LCD_WIDTH + x);
    p[0] = color >> 8;
    p[1] = color & 0xFF;
}

uint16_t display_get_pixel(const uint8_t *vram, int x, int y)
{
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return 0;
    const uint8_t *p = vram + 2 * (y * LCD_WIDTH + x);
    return (uint16_t)(p[0] << 8 | p[1]);
}

void display_rect(uint8_t *vram, int x0, int y0, int x1, int y1, uint16_t color)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            display_set_pixel(vram, x, y, color);
}

/* One character of the 8x8 font scaled sx x sy in a cw x ch cell, with the
 * glyph offset by (ox, oy) inside the cell. */
static void draw_char(uint8_t *vram, int px, int py, char c, int cw, int ch, int sx, int sy, int ox, int oy,
                      uint16_t fg, uint16_t bg, bool opaque)
{
    const uint8_t *g = platform_glyph(c);
    for (int cy = 0; cy < ch; cy++) {
        for (int cx = 0; cx < cw; cx++) {
            int gx = (cx - ox) / sx, gy = (cy - oy) / sy;
            bool on = cx >= ox && cy >= oy && gx < 8 && gy < 8 && (g[gy] & (0x80 >> gx));
            if (on)
                display_set_pixel(vram, px + cx, py + cy, fg);
            else if (opaque)
                display_set_pixel(vram, px + cx, py + cy, bg);
        }
    }
}

void display_text_big(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg, bool opaque)
{
    for (const char *c = text; *c; c++, x += BIG_CELL_W)
        draw_char(vram, x, y, *c, BIG_CELL_W, BIG_CELL_H, 2, 3, 1, 0, fg, bg, opaque);
}

void display_print_xy(uint8_t *vram, int x, int y, const char *text, int mode, int color)
{
    uint16_t fg = display_text_color(color), bg = 0xFFFF;
    if (mode == TEXT_MODE_INVERT) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    bool opaque = mode == TEXT_MODE_NORMAL || mode == TEXT_MODE_INVERT;
    display_text_big(vram, (x - 1) * BIG_CELL_W, y * BIG_CELL_H, text, fg, bg, opaque);
}

void display_text_small(uint8_t *vram, int x, int y, const char *text, uint16_t fg)
{
    for (const char *c = text; *c; c++, x += SMALL_CELL_W)
        draw_char(vram, x, y, *c, SMALL_CELL_W, SMALL_CELL_H, 1, 1, 0, 1, fg, 0, false);
}

int display_text_mini(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg,
                      bool opaque, int xlimit, bool draw)
{
    for (const char *c = text; *c; c++) {
        if (x + MINI_CELL_W > xlimit)
            break;
        if (draw)
            draw_char(vram, x, y, *c, MINI_CELL_W, MINI_CELL_H, 1, 2, 1, 1, fg, bg, opaque);
        x += MINI_CELL_W;
    }
    return x;
}
