#include "t6k11.h"

#include <string.h>

/* The panel's colours: a dark pixel, and the background. */
#define DARK_R 0x18
#define DARK_G 0x1C
#define DARK_B 0x20
#define LIGHT_R 0xC6
#define LIGHT_G 0xD0
#define LIGHT_B 0xC0

enum { V1_DISPLAY = 0, V1_COUNTER = 1, V1_ADDRESS = 4, V1_DATA = 7, V2_ADDRESS = 8, V2_DATA = 10 };

void t6k11_reset(T6k11 *lcd, const uint64_t *now, uint32_t hz)
{
    memset(lcd, 0, sizeof *lcd);
    lcd->now = now;
    lcd->grey_max = (uint64_t)hz * GREY_MAX_MS / 1000;
    lcd->counter = 1; /* column up, what gint uses */
    lcd->dirty = true; /* a first, blank render */
}

/* avg moves 1/GREY_WEIGHT of the way to v. */
static uint32_t toward(uint32_t avg, uint32_t v)
{
    return (uint32_t)((int64_t)avg + ((int64_t)v - (int64_t)avg) / GREY_WEIGHT);
}

/* Pixel p turns dark (on) or light now. Turning dark after a dark-light
 * pair closes a cycle: its length and dark part go into the pixel's
 * averages (the first cycle of a grey sets them). */
static void set_pixel(T6k11 *lcd, int p, bool on)
{
    uint64_t now = *lcd->now;
    if (!on) {
        lcd->fall[p] = now;
        return;
    }
    if (lcd->rise[p] && lcd->fall[p] > lcd->rise[p]) {
        uint64_t period = now - lcd->rise[p];
        if (period <= lcd->grey_max) {
            uint32_t dark = (uint32_t)(lcd->fall[p] - lcd->rise[p]);
            bool grey = lcd->period[p] != 0;
            lcd->avg_period[p] = grey ? toward(lcd->avg_period[p], (uint32_t)period) : (uint32_t)period;
            lcd->avg_dark[p] = grey ? toward(lcd->avg_dark[p], dark) : dark;
            lcd->period[p] = (uint32_t)period;
        } else {
            lcd->period[p] = 0; /* a slow blink, not grey */
        }
    }
    lcd->rise[p] = now;
}

/* 8 pixels at a row and column byte. */
static void put_cell(T6k11 *lcd, int row, int col, uint8_t byte)
{
    uint8_t *cell = &lcd->pixels[row][col];
    uint8_t changed = *cell ^ byte;
    if (changed) {
        int p0 = row * MONO_WIDTH + col * 8;
        for (int i = 0; i < 8; i++)
            if (changed & (0x80 >> i))
                set_pixel(lcd, p0 + i, byte & (0x80 >> i));
        *cell = byte;
        lcd->dirty = true;
    }
}

static void write_data(T6k11 *lcd, uint8_t byte)
{
    put_cell(lcd, lcd->row & (MONO_HEIGHT - 1), lcd->col & (MONO_WIDTH / 8 - 1), byte);
    if (lcd->reg == V2_DATA || lcd->counter == 1)
        lcd->col = (lcd->col + 1) & (MONO_WIDTH / 8 - 1);
    else if (lcd->counter == 0)
        lcd->col = (lcd->col - 1) & (MONO_WIDTH / 8 - 1);
    else if (lcd->counter == 3)
        lcd->row = (lcd->row + 1) & (MONO_HEIGHT - 1);
    else
        lcd->row = (lcd->row - 1) & (MONO_HEIGHT - 1);
}

static void write_reg(T6k11 *lcd, uint8_t value)
{
    switch (lcd->reg) {
    case V1_COUNTER:
        lcd->counter = value & 3;
        break;
    case V1_ADDRESS: /* bit 7: the row (bit 6 the display mode), else the column byte */
        if (value & 0x80)
            lcd->row = value & 0x3F;
        else
            lcd->col = value & 0x1F;
        break;
    case V2_ADDRESS: /* 0x80 | row; then 4, which starts the row */
        if (value & 0x80)
            lcd->row = value & 0x3F;
        else
            lcd->col = 0;
        break;
    case V1_DATA:
    case V2_DATA:
        write_data(lcd, value);
        break;
    default: /* display on/off, contrast, test registers: nothing to show */
        break;
    }
}

static uint32_t port_read(void *ctx, uint32_t addr, int size)
{
    T6k11 *lcd = ctx;
    (void)size;
    /* RS=0: the status (never busy; the counter mode in bits 0-1) */
    return addr & 0x10000 ? 0 : (uint32_t)(lcd->counter & 3);
}

static void port_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    T6k11 *lcd = ctx;
    for (int i = size - 1; i >= 0; i--) { /* wider writes: the bytes in order */
        uint8_t b = (uint8_t)(value >> 8 * i);
        if (addr & 0x10000)
            write_reg(lcd, b);
        else
            lcd->reg = b;
    }
}

void t6k11_show(T6k11 *lcd, const uint8_t *vram)
{
    for (int row = 0; row < MONO_HEIGHT; row++)
        for (int col = 0; col < MONO_WIDTH / 8; col++)
            put_cell(lcd, row, col, vram[row * (MONO_WIDTH / 8) + col]);
}

void t6k11_read(const T6k11 *lcd, uint8_t *vram)
{
    memcpy(vram, lcd->pixels, MONO_VRAM_SIZE);
}

bool t6k11_map(T6k11 *lcd, Memory *mem)
{
    return mem_add_mmio(mem, P2(MONO_PHYS_SEL), PAGE_SIZE, port_read, port_write, lcd) &&
           mem_add_mmio(mem, P2(MONO_PHYS_CMD), PAGE_SIZE, port_read, port_write, lcd);
}

bool t6k11_render(T6k11 *lcd, uint16_t *out)
{
    uint64_t now = *lcd->now;
    bool grey = false, changed = false;
    for (int y = 0; y < MONO_HEIGHT; y++)
        for (int x = 0; x < MONO_WIDTH; x++) {
            int p = y * MONO_WIDTH + x;
            bool on = lcd->pixels[y][x >> 3] & (0x80 >> (x & 7));
            uint64_t last = lcd->rise[p] > lcd->fall[p] ? lcd->rise[p] : lcd->fall[p];
            uint32_t period = lcd->period[p];
            unsigned level = on ? 255 : 0; /* 255 = dark */
            if (period && now - last <= 2 * (uint64_t)period) { /* still switching: grey */
                level = (unsigned)((uint64_t)lcd->avg_dark[p] * 255 / lcd->avg_period[p]);
                if (level > 255)
                    level = 255;
                grey = true;
            }
            unsigned r = LIGHT_R + (DARK_R - LIGHT_R) * (int)level / 255;
            unsigned g = LIGHT_G + (DARK_G - LIGHT_G) * (int)level / 255;
            unsigned b = LIGHT_B + (DARK_B - LIGHT_B) * (int)level / 255;
            uint16_t c = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            changed |= out[p] != c;
            out[p] = c;
        }
    /* Grey pixels: render again, to see them stop. */
    lcd->dirty = grey;
    return changed;
}
