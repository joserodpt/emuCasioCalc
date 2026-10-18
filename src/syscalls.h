#ifndef EMU_SYSCALLS_H
#define EMU_SYSCALLS_H

#include "emu.h"

/* Hooks the syscall handler (calls to SYSCALL_ENTRY). */
void syscalls_install(Emu *emu);

#endif
