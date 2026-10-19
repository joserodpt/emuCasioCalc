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

void t6k11_reset(T6k11 *lcd, const uint64_t *now)
{
    memset(lcd, 0, sizeof *lcd);
    lcd->now = now;
    lcd->counter = 1; /* column up, what gint uses */
    lcd->since = *now;
    lcd->dirty = true; /* a first, blank render */
    lcd->still = true;
}

/* Pixel p turns dark (on) or light at the current time. */
static void set_pixel(T6k11 *lcd, int p, bool on)
{
    uint64_t now = *lcd->now;
    if (on)
        lcd->dark_from[p] = now;
    else
        lcd->dark[p] += now - lcd->dark_from[p];
}

static void write_data(T6k11 *lcd, uint8_t byte)
{
    uint8_t *cell = &lcd->pixels[lcd->row & (MONO_HEIGHT - 1)][lcd->col & (MONO_WIDTH / 8 - 1)];
    uint8_t changed = *cell ^ byte;
    if (changed) {
        if (!lcd->dirty || lcd->still) { /* the picture was still: time starts counting now */
            lcd->still = false;
            uint64_t now = *lcd->now;
            lcd->since = now;
            for (int p = 0; p < MONO_WIDTH * MONO_HEIGHT; p++) {
                lcd->dark[p] = 0;
                lcd->dark_from[p] = now;
            }
        }
        int p0 = (lcd->row & (MONO_HEIGHT - 1)) * MONO_WIDTH + (lcd->col & (MONO_WIDTH / 8 - 1)) * 8;
        for (int i = 0; i < 8; i++)
            if (changed & (0x80 >> i))
                set_pixel(lcd, p0 + i, byte & (0x80 >> i));
        *cell = byte;
        lcd->dirty = true;
    }
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

bool t6k11_map(T6k11 *lcd, Memory *mem)
{
    return mem_add_mmio(mem, P2(MONO_PHYS_SEL), PAGE_SIZE, port_read, port_write, lcd) &&
           mem_add_mmio(mem, P2(MONO_PHYS_CMD), PAGE_SIZE, port_read, port_write, lcd);
}

void t6k11_render(T6k11 *lcd, uint16_t *out)
{
    uint64_t now = *lcd->now, span = now - lcd->since;
    bool grey = false;
    for (int y = 0; y < MONO_HEIGHT; y++)
        for (int x = 0; x < MONO_WIDTH; x++) {
            int p = y * MONO_WIDTH + x;
            bool on = lcd->pixels[y][x >> 3] & (0x80 >> (x & 7));
            uint64_t dark = lcd->dark[p] + (on ? now - lcd->dark_from[p] : 0);
            unsigned level = span ? (unsigned)(dark * 255 / span) : on ? 255 : 0; /* 255 = dark */
            if (level > 255)
                level = 255;
            if (level && level < 255)
                grey = true;
            unsigned r = LIGHT_R + (DARK_R - LIGHT_R) * (int)level / 255;
            unsigned g = LIGHT_G + (DARK_G - LIGHT_G) * (int)level / 255;
            unsigned b = LIGHT_B + (DARK_B - LIGHT_B) * (int)level / 255;
            out[p] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            lcd->dark[p] = 0;
            lcd->dark_from[p] = now;
        }
    lcd->since = now;
    /* Shades come from flicker: render again until it stops. */
    lcd->dirty = grey;
}
