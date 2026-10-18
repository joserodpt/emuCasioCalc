#ifndef EMU_SYSCALLS_H
#define EMU_SYSCALLS_H

#include "emu.h"

/* Hooks the syscall handler (calls to SYSCALL_ENTRY). */
void syscalls_install(Emu *emu);

/* OS timers (syscalls_time.c): the cycle the next one is due
 * (UINT64_MAX if none runs); runs the handlers that are due; lets cycles
 * of time pass, running handlers when they are due. */
uint64_t timers_next_due(const Emu *emu);
void timers_fire_due(Emu *emu);
void time_advance(Emu *emu, uint64_t cycles);

#endif
