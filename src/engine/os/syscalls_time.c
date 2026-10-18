/*
 * HLE syscalls: time. OS timers (Timer_*), RTC calls, CMT delays.
 *
 * From WikiPrizm: 10 timers, 1-4 the system's, 5-10 for add-ins
 * (Timer_Install with slot 0 picks a free one); a timer runs its handler
 * every `elapse` ms once started, with about 25 Hz precision; Install
 * returns the slot or -1 if taken, Start / Stop / Deinstall 0 or -2 if
 * not installed. RTC_Elapsed_ms compares against RTC_GetTicks (1/128 s);
 * RTC_SetDateTime takes 7 BCD bytes (year as 2, month, day, hour, minute,
 * second). Our reading: periods are whole 40 ms ticks (nearest, at least
 * one); RTC_Reset(nonzero) sets the clock to 2000-01-01 00:00:00.
 *
 * Handlers run like interrupts (cpu_call): between run slices, while
 * GetKey waits for a key, and during delays, at the emulated time they
 * are due.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "syscalls.h"
#include "syscalls_impl.h"

#define FIRST_USER_TIMER 5
#define TIMER_COUNT      10
#define TICK_MS          40   /* the OS's timers run at about 25 Hz */
#define HANDLER_MAX_S    2    /* a handler running longer than this is stuck */

static uint64_t cycles_per_ms(const Emu *emu)
{
    return sh7305_iphi_hz(&emu->hw) / 1000;
}

uint64_t timers_next_due(const Emu *emu)
{
    uint64_t due = UINT64_MAX;
    for (int i = 1; i <= TIMER_COUNT; i++)
        if (emu->timers[i].running && emu->timers[i].next < due)
            due = emu->timers[i].next;
    return due;
}

void timers_fire_due(Emu *emu)
{
    if (emu->in_timer || emu->cpu.state != CPU_RUNNING)
        return;
    uint64_t now = emu->cpu.instructions;
    for (int i = 1; i <= TIMER_COUNT; i++) {
        if (!emu->timers[i].running || emu->timers[i].next > now)
            continue;
        uint64_t period = (uint64_t)emu->timers[i].period_ms * cycles_per_ms(emu);
        emu->timers[i].next += period;
        if (emu->timers[i].next <= now) /* far behind: don't catch up in a burst */
            emu->timers[i].next = now + period;
        if (emu->trace)
            printf("%9.3f ms  timer %d: handler 0x%08X\n", (double)now * 1000 / sh7305_iphi_hz(&emu->hw), i,
                   emu->timers[i].handler);
        emu->in_timer = true;
        bool ok = cpu_call(&emu->cpu, emu->timers[i].handler,
                           (uint64_t)HANDLER_MAX_S * 1000 * cycles_per_ms(emu));
        emu->in_timer = false;
        if (!ok) {
            if (emu->cpu.state == CPU_ERROR)
                cpu_report(&emu->cpu, "in the handler of OS timer %d (0x%08X)", i, emu->timers[i].handler);
            return;
        }
    }
}

void time_advance(Emu *emu, uint64_t cycles)
{
    uint64_t end = emu->cpu.instructions + cycles;
    while (emu->cpu.state == CPU_RUNNING && !emu->in_timer) {
        uint64_t due = timers_next_due(emu);
        if (due > end)
            break;
        if (due > emu->cpu.instructions)
            emu->cpu.instructions = due;
        timers_fire_due(emu);
    }
    if (emu->cpu.instructions < end)
        emu->cpu.instructions = end;
}

/* Timer_Install(slot, handler, elapse_ms): slot 0 takes a free user slot. */
SC(sc_timer_install)
{
    int slot = (int32_t)a[0];
    if (slot == 0) {
        for (int i = FIRST_USER_TIMER; i <= TIMER_COUNT && !slot; i++)
            if (!emu->timers[i].installed)
                slot = i;
        if (!slot)
            return (uint32_t)-1;
    }
    if (slot < 1 || slot > TIMER_COUNT || emu->timers[slot].installed)
        return (uint32_t)-1;
    int32_t elapse = (int32_t)a[2];
    int ticks = elapse <= 0 ? 1 : (elapse + TICK_MS / 2) / TICK_MS;
    emu->timers[slot].installed = true;
    emu->timers[slot].running = false;
    emu->timers[slot].handler = a[1];
    emu->timers[slot].period_ms = (uint32_t)(ticks < 1 ? 1 : ticks) * TICK_MS;
    return (uint32_t)slot;
}

static bool installed(const Emu *emu, uint32_t slot)
{
    return slot >= 1 && slot <= TIMER_COUNT && emu->timers[slot].installed;
}

SC(sc_timer_start)
{
    if (!installed(emu, a[0]))
        return (uint32_t)-2;
    if (!emu->timers[a[0]].running) {
        emu->timers[a[0]].running = true;
        emu->timers[a[0]].next = emu->cpu.instructions + (uint64_t)emu->timers[a[0]].period_ms * cycles_per_ms(emu);
    }
    return 0;
}

SC(sc_timer_stop)
{
    if (!installed(emu, a[0]))
        return (uint32_t)-2;
    emu->timers[a[0]].running = false;
    return 0;
}

SC(sc_timer_deinstall)
{
    if (!installed(emu, a[0]))
        return (uint32_t)-2;
    memset(&emu->timers[a[0]], 0, sizeof emu->timers[a[0]]);
    return 0;
}

static uint32_t ticks_now(Emu *emu)
{
    return (uint32_t)(emu->cpu.instructions * 128 / sh7305_iphi_hz(&emu->hw));
}

/* RTC_Elapsed_ms(start_ticks, ms): 1 once ms have passed since start. */
SC(sc_rtc_elapsed_ms)
{
    uint32_t elapsed = ticks_now(emu) - a[0];
    return (uint64_t)elapsed * 1000 / 128 >= (uint64_t)(int64_t)(int32_t)a[1] ? 1 : 0;
}

/* Sets the clock so that it reads t now. */
static void set_clock(Emu *emu, time_t t)
{
    emu->hw.rtc_base = (int64_t)t - (int64_t)(emu->cpu.instructions / sh7305_iphi_hz(&emu->hw));
}

/* RTC_Reset(mode): with mode != 0 the registers are cleared. */
SC(sc_rtc_reset)
{
    if (a[0]) {
        struct tm tm = {.tm_year = 100, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1};
        set_clock(emu, mktime(&tm));
    }
    return 0;
}

static int bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 15);
}

/* RTC_SetDateTime(unsigned char time[7]): BCD year (2 bytes), month, day,
 * hour, minute, second. */
SC(sc_rtc_set_date_time)
{
    uint8_t b[7];
    for (int i = 0; i < 7; i++)
        b[i] = mem_read8(&emu->mem, a[0] + (uint32_t)i);
    struct tm tm = {
        .tm_year = bcd(b[0]) * 100 + bcd(b[1]) - 1900,
        .tm_mon = bcd(b[2]) - 1,
        .tm_mday = bcd(b[3]),
        .tm_hour = bcd(b[4]),
        .tm_min = bcd(b[5]),
        .tm_sec = bcd(b[6]),
        .tm_isdst = -1,
    };
    time_t t = mktime(&tm);
    if (t != (time_t)-1)
        set_clock(emu, t);
    return 0;
}

/* CMT_Delay_micros(us), CMT_Delay_100micros(n): busy waits. Time passes
 * in both timing modes; due timers run meanwhile. */
SC(sc_delay_micros)
{
    int32_t us = (int32_t)a[0];
    if (us > 0)
        time_advance(emu, (uint64_t)us * sh7305_iphi_hz(&emu->hw) / 1000000);
    return 0;
}

SC(sc_delay_100micros)
{
    int32_t n = (int32_t)a[0];
    if (n > 0)
        time_advance(emu, (uint64_t)n * sh7305_iphi_hz(&emu->hw) / 10000);
    return 0;
}
