/*
 * The SH3 keyboard of the first fx-9860G models, for add-ins built with
 * Casio's original SDK. Its IsKeyDown (in fxlib, linked into the add-in)
 * scans the keyboard through the SH7705's ports, not through the OS:
 * - it drives one matrix row low: rows 0..7 on port B (PBDR, 0xA4000122),
 *   rows 8.. on port M (PMDR, 0xA4000138), after setting them as outputs
 *   (PBCR 0xA4000102, PMCR 0xA4000118);
 * - waits a little with the watchdog timer (WTCNT 0xFFFFFF84, WTCSR
 *   0xFFFFFF86: it polls the interval overflow flag, bit 3);
 * - reads port A (PADR, 0xA4000120): a 0 bit is a key down in that column.
 * The SH4 models have none of these (such add-ins hang on them unless
 * patched); the monochrome models get them here so those add-ins run.
 * Matrix positions are Keyboard_IsSpecialKeyDown's: columns 0..6, rows
 * 0..11, AC/on (0, 0).
 */
#include <string.h>

#include "keys.h"
#include "syscalls_impl.h"

#define SH3_PFC_PAGE 0xA4000000u
#define SH3_WDT_PAGE 0xFFFFF000u
#define PADR 0x120
#define PBDR 0x122
#define PMDR 0x138
#define WTCSR 0xF86
#define WTCSR_IOVF 0x08
#define SCAN_CYCLES 300 /* a row's settle delay: the watchdog's count, about 10 us */

/* Port A as the keyboard drives it: the columns of the keys held in the
 * selected rows, active low. */
static uint8_t port_a(Emu *emu, const uint8_t *regs)
{
    uint8_t pb = regs[PBDR], pm = regs[PMDR];
    uint8_t cols = 0;
    emu->polls_keys = true;
    for (int k = 0; k < KEY_COUNT; k++) {
        if (!emu->key_down[k])
            continue;
        int col, row;
        sc_os_matrix(k, &col, &row);
        col--, row--;
        bool selected = row < 8 ? !(pb & (1 << row)) : !(pm & (1 << (row - 8)));
        if (selected && col >= 0 && col < 8)
            cols |= (uint8_t)(1 << col);
    }
    return (uint8_t)~cols;
}

static uint8_t pfc[PAGE_SIZE], wdt[PAGE_SIZE];

static uint32_t pfc_read(void *ctx, uint32_t addr, int size)
{
    uint32_t off = addr & (PAGE_SIZE - 1);
    if (off == PADR && size == 1)
        return port_a(ctx, pfc);
    uint32_t v = be_read(pfc + off, size);
    if (off <= PADR && off + (uint32_t)size > PADR) /* a wider read over PADR */
        v = (v & ~(0xFFu << 8 * (off + size - 1 - PADR))) | (uint32_t)port_a(ctx, pfc) << 8 * (off + size - 1 - PADR);
    return v;
}

static void pfc_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    (void)ctx;
    be_write(pfc + (addr & (PAGE_SIZE - 1)), value, size);
}

/* The watchdog: its interval timer overflows at once (the time it would
 * take is spent instead). Writes carry a password in their high byte:
 * 0x5A for WTCNT, 0xA5 for WTCSR. */
static uint32_t wdt_read(void *ctx, uint32_t addr, int size)
{
    Emu *emu = ctx;
    uint32_t off = addr & (PAGE_SIZE - 1);
    if (off == WTCSR && size == 1) {
        uint8_t v = wdt[WTCSR];
        if (v & 0x80) { /* running */
            sc_spend(emu, SCAN_CYCLES);
            v |= WTCSR_IOVF;
        }
        return v;
    }
    return be_read(wdt + off, size);
}

static void wdt_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    (void)ctx;
    uint32_t off = addr & (PAGE_SIZE - 1);
    if ((off == WTCSR || off == WTCSR - 2) && size == 2) { /* password + value */
        wdt[off] = (uint8_t)value;
        return;
    }
    be_write(wdt + off, value, size);
}

bool sh3_keys_map(Emu *emu)
{
    memset(pfc, 0xFF, sizeof pfc);
    memset(wdt, 0, sizeof wdt);
    return mem_add_mmio(&emu->mem, SH3_PFC_PAGE, PAGE_SIZE, pfc_read, pfc_write, emu) &&
           mem_add_mmio(&emu->mem, SH3_WDT_PAGE, PAGE_SIZE, wdt_read, wdt_write, emu);
}
