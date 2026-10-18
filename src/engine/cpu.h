#ifndef EMU_CPU_H
#define EMU_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/* SR bits (SH4AL-DSP). */
#define SR_T      0x00000001u
#define SR_S      0x00000002u
#define SR_IMASK  0x000000F0u
#define SR_Q      0x00000100u
#define SR_M      0x00000200u
#define SR_DSP    0x00001000u
#define SR_RC     0x0FFF0000u  /* DSP repeat counter */
#define SR_BL     0x10000000u
#define SR_RB     0x20000000u
#define SR_MD     0x40000000u
#define SR_MASK   0x7FFF1FF3u  /* writable bits */

/* Exception event codes (EXPEVT) and VBR offsets. */
#define EXC_TLB_MISS_READ   0x040
#define EXC_TLB_MISS_WRITE  0x060
#define EXC_TLB_PROT_WRITE  0x0C0
#define EXC_ADDR_ERR_READ   0x0E0
#define EXC_ADDR_ERR_WRITE  0x100
#define EXC_TRAPA           0x160
#define EXC_ILLEGAL         0x180
#define EXC_ILLEGAL_SLOT    0x1A0

typedef enum {
    CPU_RUNNING,
    CPU_EXITED,       /* add-in returned from its entry point */
    CPU_ERROR,        /* illegal instruction, memory fault, unimplemented syscall... */
} CpuState;

typedef struct Cpu Cpu;

/*
 * Called when execution reaches SYSCALL_ENTRY. The handler reads r0 (syscall
 * number) and r4-r7 / stack args, writes r0, and must set cpu->pc (normally
 * to cpu->pr) or set cpu->state.
 */
typedef void (*SyscallHandler)(Cpu *cpu, void *user);

/* Called once cpu->instructions reaches cpu->next_event; must move
 * next_event forward (UINT64_MAX when nothing is scheduled). */
typedef void (*EventHandler)(Cpu *cpu, void *user);

struct Cpu {
    uint32_t r[16];
    uint32_t bank[8];   /* the register bank that is not currently selected */
    uint32_t pc, pr, sr, gbr, vbr, mach, macl;
    uint32_t ssr, spc, sgr, dbr;
    uint32_t rs, re, mod;  /* DSP repeat start/end, modulo */
    uint32_t dsp[6];       /* DSR, A0, X0, X1, Y0, Y1 (stored, no DSP arithmetic) */
    uint32_t expevt, intevt, tea, tra;
    bool intmu;            /* CPUOPM.INTMU: interrupts raise IMASK to their level */

    uint32_t op_pc;   /* address of the instruction being executed */
    CpuState state;
    bool in_delay_slot;
    bool exception;   /* an exception was taken during the current instruction */
    bool sleeping;
    /* Time base: one instruction is one CPU clock (Iphi) cycle. */
    uint64_t instructions;

    /* Interrupt request from the interrupt controller: the highest pending
     * priority (0 = none) and its INTEVT code. Set with cpu_set_irq(). */
    int irq_level;
    uint32_t irq_code;
    /* Something may have made an interrupt acceptable (SR or the request
     * changed): the run loop leaves its fast path to check. */
    bool attention;

    uint64_t next_event;
    EventHandler event;
    void *event_user;

    Memory *mem;
    SyscallHandler syscall;
    void *syscall_user;
    bool trace_exceptions;
    /* What went wrong, for the user: the last error lines (cpu_report). */
    char error[256];
};

void cpu_reset(Cpu *cpu, Memory *mem, uint32_t entry, uint32_t stack_top);
/* Runs for up to max_instructions cycles (sleeping counts) or until the
 * state leaves CPU_RUNNING. */
void cpu_run(Cpu *cpu, uint64_t max_instructions);
void cpu_dump(const Cpu *cpu);

/* Writes SR, switching register banks if MD/RB change. */
void cpu_set_sr(Cpu *cpu, uint32_t sr);
/* Prints an error line to stderr and keeps it in cpu->error (the last few
 * lines are kept, newline-separated). */
void cpu_report(Cpu *cpu, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Updates the pending interrupt request. */
void cpu_set_irq(Cpu *cpu, int level, uint32_t code);

#endif
