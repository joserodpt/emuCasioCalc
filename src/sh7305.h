#ifndef EMU_SH7305_H
#define EMU_SH7305_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "mem.h"
#include "r61524.h"

/*
 * SH7305 on-chip peripherals, as far as gint needs them: interrupt
 * controller, timers (TMU, ETMU), RTC, DMA, key scan interface, clock
 * registers, the MMU/exception registers and the UTLB arrays.
 *
 * References: gint's register definitions (include/gint/mpu/ in the gint sources), which
 * follow the SH7724 hardware manual where the SH7305 matches it.
 *
 * Timers are evaluated lazily from cpu->instructions (one instruction per
 * Iphi cycle); the next timer/RTC interrupt is scheduled with
 * cpu->next_event so that SLEEP can skip ahead to it.
 */

typedef struct {
    uint32_t tcor, tcnt;
    uint16_t tcr;
    uint64_t synced; /* cycle of the last update */
} Tmu;

typedef struct {
    uint8_t tstr, tcr;
    uint32_t tcor, tcnt;
    uint64_t synced;
} Etmu;

typedef struct {
    uint32_t vaddr, paddr, size;
} Mapping;

typedef struct {
    Cpu *cpu;
    Memory *mem;
    R61524 *lcd;
    bool trace;

    uint16_t ipr[12];
    uint8_t imr[13];

    uint8_t tstr;
    Tmu tmu[3];
    Etmu etmu[6];

    uint8_t rcr1, rcr2;
    uint64_t rtc_synced;
    int64_t rtc_base;       /* Unix time at cycle 0 */

    uint64_t dma_end[6];    /* cycle when a running transfer reports TE, 0 = idle */

    uint8_t keys[12];       /* key matrix: keys[row] bit col, as KEYSC reports it */
    bool keysc_used;        /* the add-in scans the keyboard itself (gint) */

    /* U0 mappings, reported through the UTLB address/data arrays. */
    Mapping maps[4];
    int map_count;

    /* Register images of each device page (DMA registers live only here). */
    uint8_t bk_intc[PAGE_SIZE], bk_tmu[PAGE_SIZE], bk_etmu[PAGE_SIZE], bk_rtc[PAGE_SIZE];
    uint8_t bk_keysc[PAGE_SIZE], bk_pfc[PAGE_SIZE], bk_cpg[PAGE_SIZE], bk_dma[PAGE_SIZE];
    uint8_t bk_ccn[PAGE_SIZE], bk_cpuopm[PAGE_SIZE];
} Sh7305;

bool sh7305_init(Sh7305 *hw, Cpu *cpu, Memory *mem, R61524 *lcd, int64_t rtc_base);
void sh7305_add_mapping(Sh7305 *hw, uint32_t vaddr, uint32_t paddr, uint32_t size);

/* CPU clock, from the CPG registers (117.96 MHz by default). */
uint32_t sh7305_iphi_hz(const Sh7305 *hw);

/* Cpu event handler: brings timers and the RTC up to date. */
void sh7305_event(Cpu *cpu, void *user);

#endif
