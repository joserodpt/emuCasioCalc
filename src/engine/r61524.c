#include "r61524.h"

#include <string.h>

#include "mem.h"

#define REG_DEVICE_CODE 0x000
#define REG_HADDR       0x200
#define REG_VADDR       0x201
#define REG_DATA        0x202
#define REG_HSTART      0x210
#define REG_HEND        0x211
#define REG_VSTART      0x212
#define REG_VEND        0x213

#define X_ORIGIN VRAM_ORIGIN_X
#define Y_ORIGIN VRAM_ORIGIN_Y

/* Window edges in panel coordinates (the horizontal axis is mirrored). */
static int win_left(const R61524 *lcd)   { return PANEL_WIDTH - 1 - lcd->regs[REG_HEND]; }
static int win_right(const R61524 *lcd)  { return PANEL_WIDTH - 1 - lcd->regs[REG_HSTART]; }
static int win_top(const R61524 *lcd)    { return lcd->regs[REG_VSTART]; }
static int win_bottom(const R61524 *lcd) { return lcd->regs[REG_VEND]; }

/* The address counter is relative to the window's origin. */
static void reset_cursor(R61524 *lcd)
{
    lcd->x = win_left(lcd) + lcd->regs[REG_HADDR];
    lcd->y = win_top(lcd) + lcd->regs[REG_VADDR];
}

void r61524_reset(R61524 *lcd)
{
    memset(lcd, 0, sizeof *lcd);
    lcd->regs[REG_DEVICE_CODE] = 0x1524;
    lcd->regs[REG_HEND] = PANEL_WIDTH - 1;
    lcd->regs[REG_VEND] = PANEL_HEIGHT - 1;
    lcd->border = 0xFFFF;
    for (int i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; i++)
        lcd->fb[i] = 0xFFFF;
    reset_cursor(lcd);
    lcd->dirty = true;
}

static void put_pixel(R61524 *lcd, uint16_t color)
{
    if (lcd->x >= 0 && lcd->x < PANEL_WIDTH && lcd->y >= 0 && lcd->y < PANEL_HEIGHT) {
        lcd->fb[lcd->y * PANEL_WIDTH + lcd->x] = color;
        lcd->dirty = true;
    }
    if (++lcd->x > win_right(lcd)) {
        lcd->x = win_left(lcd);
        if (++lcd->y > win_bottom(lcd))
            lcd->y = win_top(lcd);
    }
}

void r61524_write(R61524 *lcd, uint16_t value)
{
    if (!lcd->rs) {
        lcd->index = value & 0x7FF;
        return;
    }
    if (lcd->index == REG_DATA) {
        put_pixel(lcd, value);
        return;
    }
    if (lcd->index != REG_DEVICE_CODE)
        lcd->regs[lcd->index] = value;
    switch (lcd->index) {
    case REG_HADDR: case REG_VADDR:
    case REG_HSTART: case REG_HEND: case REG_VSTART: case REG_VEND:
        reset_cursor(lcd);
    }
}

uint16_t r61524_read(R61524 *lcd)
{
    if (!lcd->rs || lcd->index == REG_DATA)
        return 0;
    return lcd->regs[lcd->index];
}

void r61524_blit_vram_rows(R61524 *lcd, const uint8_t *vram, int y1, int y2)
{
    if (y1 < 0) y1 = 0;
    if (y2 > LCD_HEIGHT - 1) y2 = LCD_HEIGHT - 1;
    for (int y = y1; y <= y2; y++) {
        uint16_t *dst = lcd->fb + (y + Y_ORIGIN) * PANEL_WIDTH + X_ORIGIN;
        const uint8_t *src = vram + y * LCD_WIDTH * 2;
        for (int x = 0; x < LCD_WIDTH; x++)
            dst[x] = (uint16_t)(src[2 * x] << 8 | src[2 * x + 1]);
    }
    lcd->dirty = true;
}

void r61524_blit_vram(R61524 *lcd, const uint8_t *vram)
{
    r61524_blit_vram_rows(lcd, vram, 0, LCD_HEIGHT - 1);
}

void r61524_read_vram(const R61524 *lcd, uint8_t *vram)
{
    for (int y = 0; y < LCD_HEIGHT; y++) {
        const uint16_t *src = lcd->fb + (y + Y_ORIGIN) * PANEL_WIDTH + X_ORIGIN;
        uint8_t *dst = vram + y * LCD_WIDTH * 2;
        for (int x = 0; x < LCD_WIDTH; x++) {
            dst[2 * x] = src[x] >> 8;
            dst[2 * x + 1] = src[x] & 0xFF;
        }
    }
}

void r61524_set_border(R61524 *lcd, uint16_t color)
{
    lcd->border = color;
    for (int y = 0; y < PANEL_HEIGHT; y++)
        for (int x = 0; x < PANEL_WIDTH; x++)
            if (x < X_ORIGIN || x >= X_ORIGIN + LCD_WIDTH || y < Y_ORIGIN || y >= Y_ORIGIN + LCD_HEIGHT)
                lcd->fb[y * PANEL_WIDTH + x] = color;
    lcd->dirty = true;
}
