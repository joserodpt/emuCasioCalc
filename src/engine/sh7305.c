#include "sh7305.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Register pages (P2 / P4 addresses). */
#define INTC_PAGE   0xA4080000u
#define TMU_PAGE    0xA4490000u
#define ETMU_PAGE   0xA44D0000u
#define RTC_PAGE    0xA413F000u
#define KEYSC_PAGE  0xA44B0000u
#define PFC_PAGE    0xA4050000u
#define CPG_PAGE    0xA4150000u
#define DMA_PAGE    0xFE008000u
#define CCN_PAGE    0xFF000000u
#define CPUOPM_PAGE 0xFF2F0000u
#define UTLB_ADDR   0xF6000000u
#define UTLB_DATA   0xF7000000u
#define UTLB_SPAN   0x4000u  /* 64 entries, 0x100 apart */

#define OFF(a) ((a) & (PAGE_SIZE - 1))

/* Default PLL setup: FLL x450 (32768 Hz -> 14.7 MHz), PLL x16. FRQCR (the
 * dividers) depends on the model, see emu.c. */
#define PLLCR_DEFAULT  0x00005000u
#define FLLFRQ_DEFAULT 0x00004384u
#define CPG_FRQCR  0x00
#define CPG_PLLCR  0x24
#define CPG_FLLFRQ 0x50

#define PVR_SH7305 0x10300B00u
#define PRR_SH7305 0x00002C00u

#define TMU_TSTR   0x04
#define TMU_BASE   0x08    /* + 12 * channel: TCOR, TCNT, TCR (16-bit) */
#define TCR_UNF    0x0100u
#define TCR_UNIE   0x0020u
#define ETMU_BASE  0x30    /* + 0x20 * channel: TSTR, TCOR, TCNT, TCR (8-bit) */
#define ETCR_UNF   0x02u
#define ETCR_UNIE  0x01u
#define TCLK_HZ    32768u  /* ETMU and RTC clock */

#define RTC_BASE   0xEC0
#define RCR2_PEF   0x80u
#define RCR2_RESET 0x02u

#define PRDR       0x13C   /* port R data; bit 4 is the display's RS line */

#define DMAOR      0x60
#define DMAOR_DME  0x0001u
#define DMAOR_NMIF 0x0002u
#define DMAOR_AE   0x0004u
#define CHCR_DE    0x00000001u
#define CHCR_TE    0x00000002u
#define CHCR_IE    0x00000004u

static uint32_t rd32(const uint8_t *bk, uint32_t off) { return be_read(bk + off, 4); }
static void wr32(uint8_t *bk, uint32_t off, uint32_t v) { be_write(bk + off, v, 4); }

/*---------------------------------------------------------------------------
 * Clocks
 *-------------------------------------------------------------------------*/

static uint64_t pll_output(const Sh7305 *hw)
{
    uint32_t frqcr = rd32(hw->bk_cpg, CPG_FRQCR), pllcr = rd32(hw->bk_cpg, CPG_PLLCR);
    uint32_t fllfrq = rd32(hw->bk_cpg, CPG_FLLFRQ);
    uint64_t f = 32768;
    uint32_t fll = fllfrq & 0x7FF;
    if ((fllfrq >> 14 & 3) == 1)
        fll >>= 1;
    if (pllcr & 0x1000)
        f *= fll;
    if (pllcr & 0x4000)
        f *= (frqcr >> 24 & 0x3F) + 1;
    return f;
}

uint32_t sh7305_iphi_hz(const Sh7305 *hw)
{
    uint32_t ifc = rd32(hw->bk_cpg, CPG_FRQCR) >> 20 & 0xF;
    uint64_t hz = pll_output(hw) >> (ifc + 1);
    return hz ? (uint32_t)hz : 1;
}

/* Pphi = Iphi >> pphi_shift. */
static unsigned pphi_shift(const Sh7305 *hw)
{
    uint32_t frqcr = rd32(hw->bk_cpg, CPG_FRQCR);
    unsigned ifc = frqcr >> 20 & 0xF, p1fc = frqcr & 0xF;
    return p1fc > ifc ? p1fc - ifc : 0;
}

/* Ticks of the 32768 Hz clock since cycle 0. */
static uint64_t tclk(const Sh7305 *hw, uint64_t cycles)
{
    return cycles * TCLK_HZ / sh7305_iphi_hz(hw);
}

/* First cycle at which tclk() reaches ticks. */
static uint64_t tclk_cycle(const Sh7305 *hw, uint64_t ticks)
{
    uint64_t hz = sh7305_iphi_hz(hw);
    return (ticks * hz + TCLK_HZ - 1) / TCLK_HZ;
}

/*---------------------------------------------------------------------------
 * Interrupt controller
 *-------------------------------------------------------------------------*/

enum { IPRA, IPRB, IPRC, IPRD, IPRE, IPRF, IPRG, IPRH, IPRI, IPRJ, IPRK, IPRL };
enum { SRC_TMU, SRC_ETMU, SRC_DMA, SRC_RTC };

/* Interrupt sources we can raise: event code, priority field, mask bit
 * (from gint's src/intc/intc.c). */
static const struct {
    uint16_t code;
    uint8_t ipr, shift, imr, bit, kind, index;
} IRQS[] = {
    {0x400, IPRA, 12, 4, 0x10, SRC_TMU, 0},
    {0x420, IPRA, 8, 4, 0x20, SRC_TMU, 1},
    {0x440, IPRA, 4, 4, 0x40, SRC_TMU, 2},
    {0x9E0, IPRJ, 12, 6, 0x08, SRC_ETMU, 0},
    {0xC20, IPRG, 8, 5, 0x02, SRC_ETMU, 1},
    {0xC40, IPRG, 4, 5, 0x04, SRC_ETMU, 2},
    {0x900, IPRE, 4, 2, 0x01, SRC_ETMU, 3},
    {0xD00, IPRI, 12, 6, 0x10, SRC_ETMU, 4},
    {0xFA0, IPRL, 12, 8, 0x02, SRC_ETMU, 5},
    {0x800, IPRE, 12, 1, 0x01, SRC_DMA, 0},
    {0x820, IPRE, 12, 1, 0x02, SRC_DMA, 1},
    {0x840, IPRE, 12, 1, 0x04, SRC_DMA, 2},
    {0x860, IPRE, 12, 1, 0x08, SRC_DMA, 3},
    {0xB80, IPRF, 8, 5, 0x10, SRC_DMA, 4},
    {0xBA0, IPRF, 8, 5, 0x20, SRC_DMA, 5},
    {0xAA0, IPRK, 12, 10, 0x02, SRC_RTC, 0},
};

static uint32_t dma_chan(int i)
{
    return i < 4 ? 0x20u + 0x10u * (uint32_t)i : 0x70u + 0x10u * (uint32_t)(i - 4);
}

static bool asserted(const Sh7305 *hw, int i)
{
    int k = IRQS[i].index;
    uint32_t chcr;
    switch (IRQS[i].kind) {
    case SRC_TMU:
        return (hw->tmu[k].tcr & (TCR_UNF | TCR_UNIE)) == (TCR_UNF | TCR_UNIE);
    case SRC_ETMU:
        return (hw->etmu[k].tcr & (ETCR_UNF | ETCR_UNIE)) == (ETCR_UNF | ETCR_UNIE);
    case SRC_DMA:
        chcr = rd32(hw->bk_dma, dma_chan(k) + 0xC);
        return (chcr & (CHCR_TE | CHCR_IE)) == (CHCR_TE | CHCR_IE);
    case SRC_RTC:
        return (hw->rcr2 & RCR2_PEF) && (hw->rcr2 >> 4 & 7);
    }
    return false;
}

static void intc_update(Sh7305 *hw)
{
    int best = 0;
    uint32_t code = 0;
    for (size_t i = 0; i < sizeof IRQS / sizeof IRQS[0]; i++) {
        if (!asserted(hw, (int)i) || (hw->imr[IRQS[i].imr] & IRQS[i].bit))
            continue;
        int level = hw->ipr[IRQS[i].ipr] >> IRQS[i].shift & 0xF;
        if (level > best) {
            best = level;
            code = IRQS[i].code;
        }
    }
    cpu_set_irq(hw->cpu, best, code);
}

/* IPRA-IPRL: 16-bit at 0x00 + 4i. IMR0-12: 8-bit at 0x80 + 4i (writing 1
 * masks). IMCR0-12: 8-bit at 0xC0 + 4i (writing 1 unmasks). */
static uint8_t intc_byte(Sh7305 *hw, uint32_t off)
{
    if (off < 0x30 && (off & 3) < 2)
        return (off & 3) ? hw->ipr[off / 4] & 0xFF : hw->ipr[off / 4] >> 8;
    if (off >= 0x80 && off < 0x80 + 13 * 4 && !(off & 3))
        return hw->imr[(off - 0x80) / 4];
    if (off >= 0xC0 && off < 0xC0 + 13 * 4 && !(off & 3))
        return 0;
    return hw->bk_intc[off];
}

static uint32_t intc_read(void *ctx, uint32_t addr, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v = v << 8 | intc_byte(ctx, OFF(addr) + (uint32_t)i);
    return v;
}

static void intc_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    for (int i = 0; i < size; i++) {
        uint32_t off = OFF(addr) + (uint32_t)i;
        uint8_t b = value >> 8 * (size - 1 - i) & 0xFF;
        if (off < 0x30 && (off & 3) < 2) {
            uint16_t *ipr = &hw->ipr[off / 4];
            *ipr = (off & 3) ? (*ipr & 0xFF00) | b : (uint16_t)((*ipr & 0x00FF) | b << 8);
        } else if (off >= 0x80 && off < 0x80 + 13 * 4 && !(off & 3)) {
            hw->imr[(off - 0x80) / 4] |= b;
        } else if (off >= 0xC0 && off < 0xC0 + 13 * 4 && !(off & 3)) {
            hw->imr[(off - 0xC0) / 4] &= (uint8_t)~b;
        } else {
            hw->bk_intc[off] = b;
        }
    }
    intc_update(hw);
}

/*---------------------------------------------------------------------------
 * Timers and RTC
 *-------------------------------------------------------------------------*/

/* Counts down ticks; on underflow reloads from tcor. Returns true if it
 * underflowed at least once. */
static bool count_down(uint32_t *tcnt, uint32_t tcor, uint64_t ticks)
{
    if (ticks <= *tcnt) {
        *tcnt -= (uint32_t)ticks;
        return false;
    }
    ticks -= (uint64_t)*tcnt + 1;
    *tcnt = tcor - (uint32_t)(ticks % ((uint64_t)tcor + 1));
    return true;
}

/* TMU channels count Pphi/4 .. Pphi/1024 (TPSC 0-4). Returns the shift from
 * CPU cycles to timer ticks. */
static unsigned tmu_shift(const Sh7305 *hw, const Tmu *t)
{
    unsigned tpsc = t->tcr & 7;
    return pphi_shift(hw) + (tpsc <= 4 ? 2 * (tpsc + 1) : 2);
}

static void timers_sync(Sh7305 *hw)
{
    uint64_t now = hw->cpu->instructions;
    for (int i = 0; i < 3; i++) {
        Tmu *t = &hw->tmu[i];
        if (hw->tstr & (1 << i)) {
            unsigned sh = tmu_shift(hw, t);
            uint64_t ticks = (now >> sh) - (t->synced >> sh);
            if (ticks && count_down(&t->tcnt, t->tcor, ticks))
                t->tcr |= TCR_UNF;
        }
        t->synced = now;
    }
    for (int i = 0; i < 6; i++) {
        Etmu *t = &hw->etmu[i];
        if (t->tstr & 1) {
            uint64_t ticks = tclk(hw, now) - tclk(hw, t->synced);
            if (ticks && count_down(&t->tcnt, t->tcor, ticks))
                t->tcr |= ETCR_UNF;
        }
        t->synced = now;
    }
}

/* Periodic interrupt intervals for RCR2.PES = 1..7, in 32768 Hz ticks. */
static const uint32_t RTC_PERIOD[8] = {0, 128, 512, 2048, 8192, 16384, 32768, 65536};

static void rtc_sync(Sh7305 *hw)
{
    uint64_t now = hw->cpu->instructions;
    uint32_t period = RTC_PERIOD[hw->rcr2 >> 4 & 7];
    if (period && tclk(hw, now) / period != tclk(hw, hw->rtc_synced) / period)
        hw->rcr2 |= RCR2_PEF;
    hw->rtc_synced = now;
}

static bool dma_sync(Sh7305 *hw);

/* Schedules the CPU event for the next timer underflow, RTC period or end of
 * DMA transfer. */
static void schedule(Sh7305 *hw)
{
    uint64_t now = hw->cpu->instructions, next = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        const Tmu *t = &hw->tmu[i];
        if (!(hw->tstr & (1 << i)) || !(t->tcr & TCR_UNIE))
            continue;
        /* From when tcnt was counted: callers that didn't sync first (RTC
         * and DMA writes) would otherwise push the underflow later. */
        unsigned sh = tmu_shift(hw, t);
        uint64_t c = ((t->synced >> sh) + (uint64_t)t->tcnt + 1) << sh;
        if (c < next)
            next = c;
    }
    for (int i = 0; i < 6; i++) {
        const Etmu *t = &hw->etmu[i];
        if (!(t->tstr & 1) || !(t->tcr & ETCR_UNIE))
            continue;
        uint64_t c = tclk_cycle(hw, tclk(hw, t->synced) + t->tcnt + 1);
        if (c < next)
            next = c;
    }
    uint32_t period = RTC_PERIOD[hw->rcr2 >> 4 & 7];
    if (period) {
        uint64_t c = tclk_cycle(hw, (tclk(hw, now) / period + 1) * period);
        if (c < next)
            next = c;
    }
    for (int i = 0; i < 6; i++)
        if (hw->dma_end[i] && hw->dma_end[i] < next)
            next = hw->dma_end[i];
    hw->cpu->next_event = next > now ? next : now + 1;
}

void sh7305_event(Cpu *cpu, void *user)
{
    Sh7305 *hw = user;
    (void)cpu;
    timers_sync(hw);
    rtc_sync(hw);
    dma_sync(hw);
    intc_update(hw);
    schedule(hw);
}

/* TMU: TSTR at 0x04, then per channel TCOR, TCNT (32-bit), TCR (16-bit).
 * Registers are kept in bk_tmu so that any access size works. */
static void tmu_store(Sh7305 *hw)
{
    hw->bk_tmu[TMU_TSTR] = hw->tstr;
    for (int i = 0; i < 3; i++) {
        uint32_t b = TMU_BASE + 12u * (uint32_t)i;
        wr32(hw->bk_tmu, b, hw->tmu[i].tcor);
        wr32(hw->bk_tmu, b + 4, hw->tmu[i].tcnt);
        be_write(hw->bk_tmu + b + 8, hw->tmu[i].tcr, 2);
    }
}

static uint32_t tmu_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    timers_sync(hw);
    intc_update(hw);
    tmu_store(hw);
    return be_read(hw->bk_tmu + OFF(addr), size);
}

static void tmu_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    timers_sync(hw);
    tmu_store(hw);
    be_write(hw->bk_tmu + OFF(addr), value, size);
    hw->tstr = hw->bk_tmu[TMU_TSTR] & 7;
    for (int i = 0; i < 3; i++) {
        Tmu *t = &hw->tmu[i];
        uint32_t b = TMU_BASE + 12u * (uint32_t)i;
        uint16_t tcr = (uint16_t)be_read(hw->bk_tmu + b + 8, 2);
        t->tcor = rd32(hw->bk_tmu, b);
        t->tcnt = rd32(hw->bk_tmu, b + 4);
        /* UNF can only be cleared (write 0 after reading 1). */
        t->tcr = (uint16_t)((tcr & ~TCR_UNF) | (t->tcr & tcr & TCR_UNF));
    }
    intc_update(hw);
    schedule(hw);
}

/* ETMU channel i at 0x30 + 0x20i: TSTR (8), TCOR, TCNT (32), TCR (8). */
static void etmu_store(Sh7305 *hw)
{
    for (int i = 0; i < 6; i++) {
        uint32_t b = ETMU_BASE + 0x20u * (uint32_t)i;
        hw->bk_etmu[b] = hw->etmu[i].tstr;
        wr32(hw->bk_etmu, b + 4, hw->etmu[i].tcor);
        wr32(hw->bk_etmu, b + 8, hw->etmu[i].tcnt);
        hw->bk_etmu[b + 0xC] = hw->etmu[i].tcr;
    }
}

static uint32_t etmu_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    timers_sync(hw);
    intc_update(hw);
    etmu_store(hw);
    return be_read(hw->bk_etmu + OFF(addr), size);
}

static void etmu_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    timers_sync(hw);
    etmu_store(hw);
    be_write(hw->bk_etmu + OFF(addr), value, size);
    for (int i = 0; i < 6; i++) {
        Etmu *t = &hw->etmu[i];
        uint32_t b = ETMU_BASE + 0x20u * (uint32_t)i;
        uint8_t tcr = hw->bk_etmu[b + 0xC];
        t->tstr = hw->bk_etmu[b] & 1;
        t->tcor = rd32(hw->bk_etmu, b + 4);
        t->tcnt = rd32(hw->bk_etmu, b + 8);
        t->tcr = (uint8_t)((tcr & ~ETCR_UNF) | (t->tcr & tcr & ETCR_UNF));
    }
    intc_update(hw);
    schedule(hw);
}

static uint8_t bcd(int v)
{
    return (uint8_t)((v / 10 % 10) << 4 | v % 10);
}

/* RTC at 0xA413FEC0: R64CNT, then BCD second/minute/hour, weekday, day,
 * month (each a byte followed by a pad byte), year (16-bit BCD), alarms,
 * RCR1 (+0x1C), RCR2 (+0x1E). The time is the host time at startup plus
 * emulated time, so it is deterministic relative to the run. */
static void rtc_store(Sh7305 *hw)
{
    uint8_t *r = hw->bk_rtc + RTC_BASE;
    uint64_t now = hw->cpu->instructions, hz = sh7305_iphi_hz(hw);
    time_t t = (time_t)(hw->rtc_base + (int64_t)(now / hz));
    struct tm tm;
    localtime_r(&t, &tm);
    r[0x00] = (uint8_t)(now * 128 / hz & 0x7F);
    r[0x02] = bcd(tm.tm_sec);
    r[0x04] = bcd(tm.tm_min);
    r[0x06] = bcd(tm.tm_hour);
    r[0x08] = (uint8_t)tm.tm_wday;
    r[0x0A] = bcd(tm.tm_mday);
    r[0x0C] = bcd(tm.tm_mon + 1);
    int year = tm.tm_year + 1900;
    r[0x0E] = bcd(year / 100);
    r[0x0F] = bcd(year % 100);
    r[0x1C] = hw->rcr1;
    r[0x1E] = hw->rcr2;
}

static uint32_t rtc_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    rtc_sync(hw);
    intc_update(hw);
    rtc_store(hw);
    return be_read(hw->bk_rtc + OFF(addr), size);
}

static void rtc_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    rtc_sync(hw);
    rtc_store(hw);
    be_write(hw->bk_rtc + OFF(addr), value, size);
    /* Writes to the time registers are ignored: the clock follows the host. */
    hw->rcr1 = hw->bk_rtc[RTC_BASE + 0x1C] & 0x7F; /* CF stays clear */
    uint8_t rcr2 = hw->bk_rtc[RTC_BASE + 0x1E] & ~RCR2_RESET;
    hw->rcr2 = (uint8_t)((rcr2 & ~RCR2_PEF) | (hw->rcr2 & rcr2 & RCR2_PEF));
    intc_update(hw);
    schedule(hw);
}

/*---------------------------------------------------------------------------
 * Key scan interface, ports, clocks, CPU core registers
 *-------------------------------------------------------------------------*/

/* KEYSC data: six 16-bit registers, register i holds rows 2i (low byte)
 * and 2i+1 (high byte); one bit per column. */
static uint32_t keysc_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    uint32_t off = OFF(addr);
    if (off < 12) {
        hw->keysc_used = hw->keysc_read = true;
        for (int i = 0; i < 6; i++) {
            hw->bk_keysc[2 * i] = hw->keys[2 * i + 1];
            hw->bk_keysc[2 * i + 1] = hw->keys[2 * i];
        }
    }
    return be_read(hw->bk_keysc + off, size);
}

static void keysc_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    if (OFF(addr) >= 12)
        be_write(hw->bk_keysc + OFF(addr), value, size);
}

static uint32_t pfc_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    return be_read(hw->bk_pfc + OFF(addr), size);
}

static void pfc_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    be_write(hw->bk_pfc + OFF(addr), value, size);
    if (hw->lcd)
        hw->lcd->rs = hw->bk_pfc[PRDR] >> 4 & 1;
}

static uint32_t cpg_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    return be_read(hw->bk_cpg + OFF(addr), size);
}

static void cpg_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    timers_sync(hw); /* timers ran at the old speed until now */
    rtc_sync(hw);
    be_write(hw->bk_cpg + OFF(addr), value, size);
    uint32_t frqcr = rd32(hw->bk_cpg, CPG_FRQCR);
    wr32(hw->bk_cpg, CPG_FRQCR, frqcr & 0x7FFFFFFFu); /* KICK completes at once */
    schedule(hw);
}

/* 0xFF000000: PTEH, PTEL, TTB, TEA (0x0C), MMUCR, TRA (0x20), EXPEVT (0x24),
 * INTEVT (0x28), PVR (0x30), PRR (0x44), PASCR, IRMCR... */
static uint32_t ccn_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    Cpu *cpu = hw->cpu;
    wr32(hw->bk_ccn, 0x0C, cpu->tea);
    wr32(hw->bk_ccn, 0x20, cpu->tra);
    wr32(hw->bk_ccn, 0x24, cpu->expevt);
    wr32(hw->bk_ccn, 0x28, cpu->intevt);
    return be_read(hw->bk_ccn + OFF(addr), size);
}

static void ccn_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    Cpu *cpu = hw->cpu;
    ccn_read(ctx, addr, size);
    be_write(hw->bk_ccn + OFF(addr), value, size);
    cpu->tea = rd32(hw->bk_ccn, 0x0C);
    cpu->tra = rd32(hw->bk_ccn, 0x20);
    cpu->expevt = rd32(hw->bk_ccn, 0x24);
    cpu->intevt = rd32(hw->bk_ccn, 0x28);
    wr32(hw->bk_ccn, 0x30, PVR_SH7305);
    wr32(hw->bk_ccn, 0x44, PRR_SH7305);
}

static uint32_t cpuopm_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    return be_read(hw->bk_cpuopm + OFF(addr), size);
}

static void cpuopm_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    be_write(hw->bk_cpuopm + OFF(addr), value, size);
    hw->cpu->intmu = rd32(hw->bk_cpuopm, 0) & 0x8;
}

/*---------------------------------------------------------------------------
 * UTLB arrays (read-only view of our fixed mappings)
 *-------------------------------------------------------------------------*/

/* Entry e of the address array (VPN | V) or data array (PPN | V | size | PR=3
 * | C | D). Mappings are reported in 1 MB, 64 kB or 4 kB pages. */
static uint32_t utlb_entry(const Sh7305 *hw, int e, bool data)
{
    for (int i = 0; i < hw->map_count; i++) {
        const Mapping *m = &hw->maps[i];
        uint32_t page, bits;
        if (!(m->vaddr & 0xFFFFF) && !(m->size & 0xFFFFF))
            page = 0x100000, bits = 0x90;
        else if (!(m->vaddr & 0xFFFF) && !(m->size & 0xFFFF))
            page = 0x10000, bits = 0x80;
        else
            page = 0x1000, bits = 0x10;
        int count = (int)(m->size / page);
        if (e >= count) {
            e -= count;
            continue;
        }
        uint32_t off = (uint32_t)e * page;
        if (data)
            return ((m->paddr + off) & 0x1FFFFC00u) | 0x100 | bits | 0x60 | 0x08 | 0x04;
        return (m->vaddr + off) | 0x100;
    }
    return 0;
}

static uint32_t utlb_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    uint32_t off = addr & 0xFF;
    if (off >= 4)
        return 0;
    uint8_t word[4];
    be_write(word, utlb_entry(hw, (int)(addr >> 8 & 63), addr >= UTLB_DATA), 4);
    return off + (uint32_t)size <= 4 ? be_read(word + off, size) : 0;
}

/*---------------------------------------------------------------------------
 * DMA controller
 *-------------------------------------------------------------------------*/

/* Physical address -> an address we can access: P1 for memory (the DMA
 * doesn't stall the CPU like uncached accesses do), P2 for the peripheral
 * areas (1: on-chip registers, 5: the display), which only exist there. */
static uint32_t dma_vaddr(uint32_t a)
{
    if (a >= 0x20000000u)
        return a;
    uint32_t area = a >> 26;
    return area == 1 || area == 5 ? P2(a) : P1(a);
}

static void dma_transfer(Sh7305 *hw, int ch)
{
    static const uint32_t UNIT[] = {1, 2, 4, 16, 32};
    Memory *mem = hw->mem;
    uint8_t *bk = hw->bk_dma;
    uint32_t base = dma_chan(ch);
    uint32_t chcr = rd32(bk, base + 0xC);
    unsigned ts = (chcr >> 20 & 3) << 2 | (chcr >> 3 & 3);
    unsigned sm = chcr >> 12 & 3, dm = chcr >> 14 & 3, rs = chcr >> 8 & 0xF;

    if (rs != 4 || ts >= sizeof UNIT / sizeof UNIT[0]) {
        fprintf(stderr, "dma%d: unsupported setup CHCR=0x%08X (only auto-request, 1-32 byte units)\n", ch, chcr);
        wr32(bk, base + 0xC, chcr & ~CHCR_DE);
        return;
    }
    uint32_t unit = UNIT[ts], chunk = unit < 4 ? unit : 4;
    uint32_t sar = rd32(bk, base), dar = rd32(bk, base + 4);
    uint32_t count = rd32(bk, base + 8) & 0xFFFFFF;
    if (!count)
        count = 0x1000000;
    if (hw->trace)
        fprintf(stderr, "dma%d: %u x %u bytes 0x%08X -> 0x%08X\n", ch, count, unit, sar, dar);

    for (uint32_t k = 0; k < count; k++) {
        for (uint32_t b = 0; b < unit; b += chunk) {
            uint32_t s = dma_vaddr(sar + (sm ? b : 0)), d = dma_vaddr(dar + (dm ? b : 0));
            if (chunk == 4) mem_write32(mem, d, mem_read32(mem, s));
            else if (chunk == 2) mem_write16(mem, d, mem_read16(mem, s));
            else mem_write8(mem, d, mem_read8(mem, s));
        }
        sar += sm == 1 ? unit : sm == 2 ? -unit : 0;
        dar += dm == 1 ? unit : dm == 2 ? -unit : 0;
        if (mem->fault) {
            fprintf(stderr, "dma%d: address error at 0x%08X\n", ch, mem->fault_addr);
            mem->fault = FAULT_NONE;
            be_write(bk + DMAOR, be_read(bk + DMAOR, 2) | DMAOR_AE, 2);
            break;
        }
    }
    wr32(bk, base, sar);
    wr32(bk, base + 4, dar);
    wr32(bk, base + 8, 0);

    /* The data has moved, but TE (and the interrupt) come when real
     * hardware would be done. */
    double per_byte = (rd32(bk, base + 4) & 0x1C000000u) == LCD_PHYS ? DMA_LCD_CYCLES_PER_BYTE
                                                                       : DMA_RAM_CYCLES_PER_BYTE;
    hw->dma_end[ch] = hw->cpu->instructions + 1 + (hw->dma_instant ? 0 : (uint64_t)(per_byte * count * unit));
}

/* Sets TE on transfers whose time is up; forgets disabled ones. Returns
 * true if a transfer ended. */
static bool dma_sync(Sh7305 *hw)
{
    bool ended = false;
    for (int i = 0; i < 6; i++) {
        if (!hw->dma_end[i])
            continue;
        uint32_t o = dma_chan(i) + 0xC, chcr = rd32(hw->bk_dma, o);
        if (!(chcr & CHCR_DE)) {
            hw->dma_end[i] = 0;
        } else if (hw->cpu->instructions >= hw->dma_end[i]) {
            wr32(hw->bk_dma, o, chcr | CHCR_TE);
            hw->dma_end[i] = 0;
            ended = true;
        }
    }
    return ended;
}

/* Starts transfers on enabled channels. */
static void dma_check(Sh7305 *hw)
{
    uint16_t dmaor = (uint16_t)be_read(hw->bk_dma + DMAOR, 2);
    if (!(dmaor & DMAOR_DME) || (dmaor & (DMAOR_AE | DMAOR_NMIF)))
        return;
    for (int i = 0; i < 6; i++) {
        uint32_t chcr = rd32(hw->bk_dma, dma_chan(i) + 0xC);
        if ((chcr & CHCR_DE) && !(chcr & CHCR_TE) && !hw->dma_end[i])
            dma_transfer(hw, i);
    }
}

static uint32_t dma_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    if (dma_sync(hw)) /* gint polls CHCR.TE in tight loops: keep this cheap */
        intc_update(hw);
    return be_read(hw->bk_dma + OFF(addr), size);
}

static void dma_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    uint8_t old[PAGE_SIZE / 16];
    dma_sync(hw);
    memcpy(old, hw->bk_dma, sizeof old);
    be_write(hw->bk_dma + OFF(addr), value, size);
    /* TE, AE and NMIF can only be cleared. */
    for (int i = 0; i < 6; i++) {
        uint32_t o = dma_chan(i) + 0xC;
        uint32_t chcr = rd32(hw->bk_dma, o);
        wr32(hw->bk_dma, o, (chcr & ~CHCR_TE) | (rd32(old, o) & chcr & CHCR_TE));
    }
    uint16_t dmaor = (uint16_t)be_read(hw->bk_dma + DMAOR, 2), was = (uint16_t)be_read(old + DMAOR, 2);
    uint16_t sticky = DMAOR_AE | DMAOR_NMIF;
    be_write(hw->bk_dma + DMAOR, (dmaor & ~sticky) | (was & dmaor & sticky), 2);
    dma_sync(hw); /* a channel disabled mid-transfer is aborted */
    dma_check(hw);
    intc_update(hw);
    schedule(hw);
}

/*---------------------------------------------------------------------------
 * Display port
 *-------------------------------------------------------------------------*/

static uint32_t lcd_read(void *ctx, uint32_t addr, int size)
{
    Sh7305 *hw = ctx;
    (void)addr;
    uint32_t v = r61524_read(hw->lcd);
    return size == 4 ? v << 16 | r61524_read(hw->lcd) : v;
}

static void lcd_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    Sh7305 *hw = ctx;
    (void)addr;
    if (size == 4) {
        r61524_write(hw->lcd, (uint16_t)(value >> 16));
        r61524_write(hw->lcd, (uint16_t)value);
    } else {
        r61524_write(hw->lcd, (uint16_t)value);
    }
}

/*---------------------------------------------------------------------------
 * Setup
 *-------------------------------------------------------------------------*/

bool sh7305_init(Sh7305 *hw, Cpu *cpu, Memory *mem, R61524 *lcd, int64_t rtc_base, uint32_t frqcr)
{
    memset(hw, 0, sizeof *hw);
    hw->cpu = cpu;
    hw->mem = mem;
    hw->lcd = lcd;
    hw->rtc_base = rtc_base;
    memset(hw->imr, 0xFF, sizeof hw->imr);
    for (int i = 0; i < 3; i++)
        hw->tmu[i].tcor = hw->tmu[i].tcnt = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++)
        hw->etmu[i].tcor = hw->etmu[i].tcnt = 0xFFFFFFFFu;
    wr32(hw->bk_cpg, CPG_FRQCR, frqcr);
    wr32(hw->bk_cpg, CPG_PLLCR, PLLCR_DEFAULT);
    wr32(hw->bk_cpg, CPG_FLLFRQ, FLLFRQ_DEFAULT);
    wr32(hw->bk_ccn, 0x30, PVR_SH7305);
    wr32(hw->bk_ccn, 0x44, PRR_SH7305);
    hw->bk_pfc[PRDR] = 0x10;
    if (lcd)
        lcd->rs = true;

    static const struct {
        uint32_t base, size;
        MmioRead read;
        MmioWrite write;
    } DEVICES[] = {
        {INTC_PAGE, PAGE_SIZE, intc_read, intc_write},
        {TMU_PAGE, PAGE_SIZE, tmu_read, tmu_write},
        {ETMU_PAGE, PAGE_SIZE, etmu_read, etmu_write},
        {RTC_PAGE, PAGE_SIZE, rtc_read, rtc_write},
        {KEYSC_PAGE, PAGE_SIZE, keysc_read, keysc_write},
        {PFC_PAGE, PAGE_SIZE, pfc_read, pfc_write},
        {CPG_PAGE, PAGE_SIZE, cpg_read, cpg_write},
        {DMA_PAGE, PAGE_SIZE, dma_read, dma_write},
        {CCN_PAGE, PAGE_SIZE, ccn_read, ccn_write},
        {CPUOPM_PAGE, PAGE_SIZE, cpuopm_read, cpuopm_write},
        {UTLB_ADDR, UTLB_SPAN, utlb_read, NULL},
        {UTLB_DATA, UTLB_SPAN, utlb_read, NULL},
    };
    for (size_t i = 0; i < sizeof DEVICES / sizeof DEVICES[0]; i++)
        if (!mem_add_mmio(mem, DEVICES[i].base, DEVICES[i].size, DEVICES[i].read, DEVICES[i].write, hw))
            return false;
    /* The colour display; monochrome models have their own (t6k11.c) */
    return !lcd || mem_add_mmio(mem, P2(LCD_PHYS), PAGE_SIZE, lcd_read, lcd_write, hw);
}

void sh7305_add_mapping(Sh7305 *hw, uint32_t vaddr, uint32_t paddr, uint32_t size)
{
    if (hw->map_count < (int)(sizeof hw->maps / sizeof hw->maps[0]))
        hw->maps[hw->map_count++] = (Mapping){vaddr, paddr, size};
}
