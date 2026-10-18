/*
 * SH4AL-DSP integer interpreter (the fx-CG50's SH7305 has no FPU, so 0xFxxx
 * opcodes, which are DSP data instructions here, are treated as illegal).
 * Opcode reference: Renesas "SH-4A Software Manual", chapter 10 (instruction
 * descriptions), and the SH4AL-DSP additions (banked/DSP control registers,
 * repeat loops). Privileged mode, exceptions and interrupts follow the SH-4A
 * model: SSR/SPC/SGR are saved, SR gets MD=RB=BL=1 and execution continues
 * at VBR+0x100 (general), VBR+0x400 (TLB miss) or VBR+0x600 (interrupt).
 */
#include "cpu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RN  cpu->r[n]
#define RM  cpu->r[m]
#define R0  cpu->r[0]
#define T   (cpu->sr & SR_T)

static inline void set_t(Cpu *cpu, bool v)
{
    cpu->sr = v ? cpu->sr | SR_T : cpu->sr & ~SR_T;
}

static inline void set_bit(Cpu *cpu, uint32_t bit, bool v)
{
    cpu->sr = v ? cpu->sr | bit : cpu->sr & ~bit;
}

static inline uint32_t sext8(uint32_t v)  { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sext16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
static inline uint32_t sext12(uint32_t v) { return (uint32_t)((int32_t)(v << 20) >> 20); }

static inline bool bank1(uint32_t sr)
{
    return (sr & (SR_MD | SR_RB)) == (SR_MD | SR_RB);
}

void cpu_set_sr(Cpu *cpu, uint32_t sr)
{
    sr &= SR_MASK;
    if (bank1(sr) != bank1(cpu->sr)) {
        for (int i = 0; i < 8; i++) {
            uint32_t t = cpu->r[i];
            cpu->r[i] = cpu->bank[i];
            cpu->bank[i] = t;
        }
    }
    cpu->sr = sr;
    cpu->attention = true;
}

void cpu_set_irq(Cpu *cpu, int level, uint32_t code)
{
    if (level != cpu->irq_level || code != cpu->irq_code)
        cpu->attention = true;
    cpu->irq_level = level;
    cpu->irq_code = code;
}

void cpu_report(Cpu *cpu, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    /* Append, dropping the oldest lines when full. */
    while (strlen(cpu->error) + strlen(line) + 2 > sizeof cpu->error) {
        char *nl = strchr(cpu->error, '\n');
        if (!nl) {
            cpu->error[0] = '\0';
            break;
        }
        memmove(cpu->error, nl + 1, strlen(nl + 1) + 1);
    }
    if (cpu->error[0])
        strcat(cpu->error, "\n");
    strcat(cpu->error, line);
}

static void exec(Cpu *cpu, uint32_t pc);

static void take_exception(Cpu *cpu, uint32_t code, uint32_t offset, uint32_t spc)
{
    cpu->exception = true;
    if (!cpu->vbr || (cpu->sr & SR_BL)) {
        cpu_report(cpu, "cpu: exception 0x%03X at 0x%08X with no handler%s", code, spc,
                cpu->vbr ? " (SR.BL is set)" : " (VBR is 0)");
        cpu->state = CPU_ERROR;
        return;
    }
    if (cpu->trace_exceptions)
        fprintf(stderr, "cpu: exception 0x%03X at 0x%08X\n", code, spc);
    cpu->expevt = code;
    cpu->spc = spc;
    cpu->ssr = cpu->sr;
    cpu->sgr = cpu->r[15];
    cpu_set_sr(cpu, cpu->sr | SR_MD | SR_RB | SR_BL);
    cpu->pc = cpu->vbr + offset;
}

static void accept_interrupt(Cpu *cpu)
{
    cpu->intevt = cpu->irq_code;
    cpu->spc = cpu->pc;
    cpu->ssr = cpu->sr;
    cpu->sgr = cpu->r[15];
    uint32_t sr = cpu->sr | SR_MD | SR_RB | SR_BL;
    if (cpu->intmu)
        sr = (sr & ~SR_IMASK) | (uint32_t)cpu->irq_level << 4;
    cpu_set_sr(cpu, sr);
    cpu->pc = cpu->vbr + 0x600;
    cpu->sleeping = false;
}

static void illegal(Cpu *cpu, uint16_t op, uint32_t pc)
{
    cpu_report(cpu, "cpu: illegal or unimplemented instruction 0x%04X at 0x%08X", op, pc);
    if (cpu->in_delay_slot)
        take_exception(cpu, EXC_ILLEGAL_SLOT, 0x100, pc - 2);
    else
        take_exception(cpu, EXC_ILLEGAL, 0x100, pc);
}

/* Runs the delay-slot instruction at pc+2, then jumps. target is computed by
 * the caller before the slot runs, as on hardware. */
static void delayed_branch(Cpu *cpu, uint32_t pc, uint32_t target)
{
    if (cpu->in_delay_slot) {
        illegal(cpu, mem_read16(cpu->mem, pc), pc);
        return;
    }
    cpu->in_delay_slot = true;
    exec(cpu, pc + 2);
    cpu->in_delay_slot = false;
    if (cpu->exception || cpu->mem->fault || cpu->state != CPU_RUNNING) {
        cpu->op_pc = pc;  /* an exception in the slot reports the branch */
        return;
    }
    cpu->pc = target;
}

/* One step of non-restoring division (manual pseudo-code, condensed). */
static void div1(Cpu *cpu, unsigned n, unsigned m)
{
    bool old_q = cpu->sr & SR_Q, M = cpu->sr & SR_M;
    bool msb = RN >> 31;
    uint32_t before = RN << 1 | T;
    uint32_t after;
    bool carry;
    if (old_q == M) {
        after = before - RM;
        carry = after > before;
    } else {
        after = before + RM;
        carry = after < before;
    }
    RN = after;
    bool q = M ? !(msb ^ carry) : (msb ^ carry);
    set_bit(cpu, SR_Q, q);
    set_t(cpu, q == M);
}

static void mac_l(Cpu *cpu, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    int64_t a = (int32_t)mem_read32(mem, RN);
    RN += 4;
    int64_t b = (int32_t)mem_read32(mem, RM);
    RM += 4;
    int64_t acc = (int64_t)((uint64_t)cpu->mach << 32 | cpu->macl) + a * b;
    if (cpu->sr & SR_S) {
        const int64_t lim = (int64_t)1 << 47;
        if (acc < -lim) acc = -lim;
        if (acc > lim - 1) acc = lim - 1;
    }
    cpu->mach = (uint32_t)((uint64_t)acc >> 32);
    cpu->macl = (uint32_t)acc;
}

static void mac_w(Cpu *cpu, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    int32_t a = (int16_t)mem_read16(mem, RN);
    RN += 2;
    int32_t b = (int16_t)mem_read16(mem, RM);
    RM += 2;
    int64_t prod = (int64_t)a * b;
    if (cpu->sr & SR_S) {
        int64_t r = (int64_t)(int32_t)cpu->macl + prod;
        if (r > INT32_MAX || r < INT32_MIN) {
            cpu->macl = r > 0 ? INT32_MAX : (uint32_t)INT32_MIN;
            cpu->mach |= 1;
        } else {
            cpu->macl = (uint32_t)r;
        }
    } else {
        uint64_t acc = ((uint64_t)cpu->mach << 32 | cpu->macl) + (uint64_t)prod;
        cpu->mach = (uint32_t)(acc >> 32);
        cpu->macl = (uint32_t)acc;
    }
}

static uint32_t shad(uint32_t rn, uint32_t rm)
{
    int32_t s = (int32_t)rm;
    if (s >= 0)
        return rn << (s & 0x1F);
    if ((s & 0x1F) == 0)
        return (int32_t)rn < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)rn >> ((~s & 0x1F) + 1));
}

static uint32_t shld(uint32_t rn, uint32_t rm)
{
    int32_t s = (int32_t)rm;
    if (s >= 0)
        return rn << (s & 0x1F);
    if ((s & 0x1F) == 0)
        return 0;
    return rn >> ((~s & 0x1F) + 1);
}

/* Control registers selected by bits 7-4 in STC/LDC (0x0nX2, 0x4nX3,
 * 0x4mX7, 0x4mXE). SR (0) needs cpu_set_sr and is handled by the callers;
 * 8-15 are the banked R0-R7. */
static uint32_t *ctrl_reg(Cpu *cpu, unsigned sel)
{
    switch (sel) {
    case 0x1: return &cpu->gbr;
    case 0x2: return &cpu->vbr;
    case 0x3: return &cpu->ssr;
    case 0x4: return &cpu->spc;
    case 0x5: return &cpu->mod;
    case 0x6: return &cpu->rs;
    case 0x7: return &cpu->re;
    }
    return sel >= 8 ? &cpu->bank[sel & 7] : NULL;
}

/* System registers selected by bits 7-4 in STS/LDS (0x0nXA, 0x4nX2, 0x4mX6,
 * 0x4mXA), plus SGR and DBR which share the encoding space. */
static uint32_t *sys_reg(Cpu *cpu, unsigned sel, bool load)
{
    switch (sel) {
    case 0x0: return &cpu->mach;
    case 0x1: return &cpu->macl;
    case 0x2: return &cpu->pr;
    case 0x3: return load ? NULL : &cpu->sgr;
    case 0x6: case 0x7: case 0x8: case 0x9: case 0xA: case 0xB: return &cpu->dsp[sel - 6];
    case 0xF: return &cpu->dbr;
    }
    return NULL;
}

static uint32_t read_ctrl(Cpu *cpu, unsigned sel)
{
    if (sel == 0)
        return cpu->sr;
    uint32_t *r = ctrl_reg(cpu, sel);
    return r ? *r : 0;
}

static bool write_ctrl(Cpu *cpu, unsigned sel, uint32_t v)
{
    if (sel == 0) {
        cpu_set_sr(cpu, v);
        return true;
    }
    uint32_t *r = ctrl_reg(cpu, sel);
    if (r)
        *r = v;
    return r != NULL;
}

static void exec_0(Cpu *cpu, uint16_t op, uint32_t pc, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    uint32_t *r;
    switch (op & 0xF) {
    case 0x2: /* STC SR/GBR/VBR/SSR/SPC/MOD/RS/RE/Rm_BANK,Rn */
        if (m == 0 || ctrl_reg(cpu, m)) {
            RN = read_ctrl(cpu, m);
            return;
        }
        break;
    case 0x3:
        switch (m) {
        case 0x0: cpu->pr = pc + 4; delayed_branch(cpu, pc, pc + 4 + RN); return; /* BSRF */
        case 0x2: delayed_branch(cpu, pc, pc + 4 + RN); return;                   /* BRAF */
        case 0x6: R0 = mem_read32(mem, RN); return;                               /* MOVLI.L @Rm,R0 */
        case 0x7: mem_write32(mem, RN, R0); set_t(cpu, true); return;             /* MOVCO.L R0,@Rn */
        case 0xC: mem_write32(mem, RN, R0); return;                               /* MOVCA.L R0,@Rn */
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xD: case 0xE: return;       /* cache ops: no-op */
        }
        break;
    case 0x4: mem_write8(mem, R0 + RN, (uint8_t)RM); return;
    case 0x5: mem_write16(mem, R0 + RN, (uint16_t)RM); return;
    case 0x6: mem_write32(mem, R0 + RN, RM); return;
    case 0x7: cpu->macl = RN * RM; return; /* MUL.L */
    case 0x8:
        switch (op) {
        case 0x0008: set_t(cpu, false); return;                 /* CLRT */
        case 0x0018: set_t(cpu, true); return;                  /* SETT */
        case 0x0028: cpu->mach = cpu->macl = 0; return;         /* CLRMAC */
        case 0x0038: return;                                    /* LDTLB: we have no TLB */
        case 0x0048: set_bit(cpu, SR_S, false); return;         /* CLRS */
        case 0x0058: set_bit(cpu, SR_S, true); return;          /* SETS */
        }
        break;
    case 0x9:
        if (op == 0x0009) return;                                       /* NOP */
        if (op == 0x0019) { cpu->sr &= ~(SR_M | SR_Q | SR_T); return; } /* DIV0U */
        if (m == 2) { RN = T; return; }                                 /* MOVT */
        break;
    case 0xA: /* STS MACH/MACL/PR/DSP regs, STC SGR/DBR */
        if ((r = sys_reg(cpu, m, false))) {
            RN = *r;
            return;
        }
        break;
    case 0xB:
        if (op == 0x000B) { delayed_branch(cpu, pc, cpu->pr); return; } /* RTS */
        if (op == 0x001B) { cpu->sleeping = cpu->attention = true; return; } /* SLEEP */
        if (op == 0x002B) {                                             /* RTE */
            uint32_t target = cpu->spc;
            cpu_set_sr(cpu, cpu->ssr);
            delayed_branch(cpu, pc, target);
            return;
        }
        if (op == 0x00AB) return;                                       /* SYNCO */
        break;
    case 0xC: RN = sext8(mem_read8(mem, R0 + RM)); return;
    case 0xD: RN = sext16(mem_read16(mem, R0 + RM)); return;
    case 0xE: RN = mem_read32(mem, R0 + RM); return;
    case 0xF: mac_l(cpu, n, m); return;
    }
    illegal(cpu, op, pc);
}

static void exec_2(Cpu *cpu, uint16_t op, uint32_t pc, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    uint32_t t;
    switch (op & 0xF) {
    case 0x0: mem_write8(mem, RN, (uint8_t)RM); return;
    case 0x1: mem_write16(mem, RN, (uint16_t)RM); return;
    case 0x2: mem_write32(mem, RN, RM); return;
    case 0x4: t = RM; mem_write8(mem, RN - 1, (uint8_t)t); RN -= 1; return;
    case 0x5: t = RM; mem_write16(mem, RN - 2, (uint16_t)t); RN -= 2; return;
    case 0x6: t = RM; mem_write32(mem, RN - 4, t); RN -= 4; return;
    case 0x7: /* DIV0S */
        set_bit(cpu, SR_Q, RN >> 31);
        set_bit(cpu, SR_M, RM >> 31);
        set_t(cpu, (RN >> 31) != (RM >> 31));
        return;
    case 0x8: set_t(cpu, (RN & RM) == 0); return;
    case 0x9: RN &= RM; return;
    case 0xA: RN ^= RM; return;
    case 0xB: RN |= RM; return;
    case 0xC: /* CMP/STR: T if any byte is equal */
        t = RN ^ RM;
        set_t(cpu, !(t & 0xFF000000) || !(t & 0xFF0000) || !(t & 0xFF00) || !(t & 0xFF));
        return;
    case 0xD: RN = RM << 16 | RN >> 16; return;                                     /* XTRCT */
    case 0xE: cpu->macl = (uint32_t)(uint16_t)RN * (uint16_t)RM; return;            /* MULU.W */
    case 0xF: cpu->macl = (uint32_t)((int32_t)(int16_t)RN * (int16_t)RM); return;   /* MULS.W */
    }
    illegal(cpu, op, pc);
}

static void exec_3(Cpu *cpu, uint16_t op, uint32_t pc, unsigned n, unsigned m)
{
    uint64_t u;
    int64_t s;
    switch (op & 0xF) {
    case 0x0: set_t(cpu, RN == RM); return;                         /* CMP/EQ */
    case 0x2: set_t(cpu, RN >= RM); return;                         /* CMP/HS */
    case 0x3: set_t(cpu, (int32_t)RN >= (int32_t)RM); return;       /* CMP/GE */
    case 0x4: div1(cpu, n, m); return;
    case 0x5: u = (uint64_t)RN * RM; cpu->mach = (uint32_t)(u >> 32); cpu->macl = (uint32_t)u; return;
    case 0x6: set_t(cpu, RN > RM); return;                          /* CMP/HI */
    case 0x7: set_t(cpu, (int32_t)RN > (int32_t)RM); return;        /* CMP/GT */
    case 0x8: RN -= RM; return;
    case 0xA: u = (uint64_t)RN - RM - T; RN = (uint32_t)u; set_t(cpu, u >> 32 & 1); return; /* SUBC */
    case 0xB: s = (int64_t)(int32_t)RN - (int32_t)RM; RN = (uint32_t)s; set_t(cpu, s != (int32_t)s); return;
    case 0xC: RN += RM; return;
    case 0xD:
        s = (int64_t)(int32_t)RN * (int32_t)RM;
        cpu->mach = (uint32_t)((uint64_t)s >> 32);
        cpu->macl = (uint32_t)s;
        return;
    case 0xE: u = (uint64_t)RN + RM + T; RN = (uint32_t)u; set_t(cpu, u >> 32); return;   /* ADDC */
    case 0xF: s = (int64_t)(int32_t)RN + (int32_t)RM; RN = (uint32_t)s; set_t(cpu, s != (int32_t)s); return;
    }
    illegal(cpu, op, pc);
}

static void set_rc(Cpu *cpu, uint32_t rc)
{
    cpu->sr = (cpu->sr & ~SR_RC) | (rc & 0xFFF) << 16;
}

/* MOVUA.L: 32-bit load from any alignment. */
static uint32_t read_unaligned32(Memory *mem, uint32_t a)
{
    return (uint32_t)mem_read8(mem, a) << 24 | (uint32_t)mem_read8(mem, a + 1) << 16 |
           (uint32_t)mem_read8(mem, a + 2) << 8 | mem_read8(mem, a + 3);
}

static void exec_4(Cpu *cpu, uint16_t op, uint32_t pc, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    unsigned sel = m; /* bits 7-4 select the register in the families below */
    uint32_t t, *r;
    switch (op & 0xF) {
    case 0xC: RN = shad(RN, RM); return;
    case 0xD: RN = shld(RN, RM); return;
    case 0xF: mac_w(cpu, n, m); return;
    case 0x2: /* STS.L sysreg,@-Rn / STC.L SGR/DBR,@-Rn */
        if ((r = sys_reg(cpu, sel, false))) {
            mem_write32(mem, RN - 4, *r);
            RN -= 4;
            return;
        }
        break;
    case 0x3: /* STC.L ctrl,@-Rn */
        if (sel == 0 || ctrl_reg(cpu, sel)) {
            mem_write32(mem, RN - 4, read_ctrl(cpu, sel));
            RN -= 4;
            return;
        }
        break;
    case 0x6: /* LDS.L @Rm+,sysreg / LDC.L @Rm+,DBR (register field is in the n position) */
        if ((r = sys_reg(cpu, sel, true))) {
            t = mem_read32(mem, RN);
            RN += 4;
            *r = t;
            return;
        }
        break;
    case 0x7: /* LDC.L @Rm+,ctrl */
        t = mem_read32(mem, RN);
        if (sel == 0 || ctrl_reg(cpu, sel)) {
            RN += 4;
            write_ctrl(cpu, sel, t);
            return;
        }
        break;
    case 0xA: /* LDS Rm,sysreg / LDC Rm,DBR */
        if ((r = sys_reg(cpu, sel, true))) {
            *r = RN;
            return;
        }
        break;
    case 0xE: /* LDC Rm,ctrl */
        if (write_ctrl(cpu, sel, RN))
            return;
        break;
    }
    switch (op & 0xFF) {
    case 0x00: case 0x20: set_t(cpu, RN >> 31); RN <<= 1; return;           /* SHLL / SHAL */
    case 0x01: set_t(cpu, RN & 1); RN >>= 1; return;                         /* SHLR */
    case 0x21: set_t(cpu, RN & 1); RN = (uint32_t)((int32_t)RN >> 1); return; /* SHAR */
    case 0x04: t = RN >> 31; RN = RN << 1 | t; set_t(cpu, t); return;        /* ROTL */
    case 0x05: t = RN & 1; RN = RN >> 1 | t << 31; set_t(cpu, t); return;    /* ROTR */
    case 0x24: t = RN >> 31; RN = RN << 1 | T; set_t(cpu, t); return;        /* ROTCL */
    case 0x25: t = RN & 1; RN = RN >> 1 | (uint32_t)T << 31; set_t(cpu, t); return; /* ROTCR */
    case 0x08: RN <<= 2; return;
    case 0x18: RN <<= 8; return;
    case 0x28: RN <<= 16; return;
    case 0x09: RN >>= 2; return;
    case 0x19: RN >>= 8; return;
    case 0x29: RN >>= 16; return;
    case 0x10: RN -= 1; set_t(cpu, RN == 0); return;                         /* DT */
    case 0x11: set_t(cpu, (int32_t)RN >= 0); return;                         /* CMP/PZ */
    case 0x15: set_t(cpu, (int32_t)RN > 0); return;                          /* CMP/PL */
    case 0x1B: t = mem_read8(mem, RN); set_t(cpu, t == 0); mem_write8(mem, RN, (uint8_t)(t | 0x80)); return; /* TAS.B */
    case 0x14: set_rc(cpu, RN); return;                                      /* SETRC Rm */
    case 0x34: set_rc(cpu, RN); return;                                      /* LDRC Rm */
    case 0xA9: R0 = read_unaligned32(mem, RN); return;                       /* MOVUA.L @Rm,R0 */
    case 0xE9: t = read_unaligned32(mem, RN); RN += 4; R0 = t; return;       /* MOVUA.L @Rm+,R0 */
    case 0x0B: t = RN; cpu->pr = pc + 4; delayed_branch(cpu, pc, t); return; /* JSR */
    case 0x2B: delayed_branch(cpu, pc, RN); return;                           /* JMP */
    }
    illegal(cpu, op, pc);
}

static void exec_6(Cpu *cpu, uint16_t op, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    uint32_t t;
    uint64_t u;
    switch (op & 0xF) {
    case 0x0: RN = sext8(mem_read8(mem, RM)); return;
    case 0x1: RN = sext16(mem_read16(mem, RM)); return;
    case 0x2: RN = mem_read32(mem, RM); return;
    case 0x3: RN = RM; return;
    case 0x4: t = sext8(mem_read8(mem, RM)); if (n != m) RM += 1; RN = t; return;
    case 0x5: t = sext16(mem_read16(mem, RM)); if (n != m) RM += 2; RN = t; return;
    case 0x6: t = mem_read32(mem, RM); if (n != m) RM += 4; RN = t; return;
    case 0x7: RN = ~RM; return;
    case 0x8: RN = (RM & 0xFFFF0000u) | (RM & 0xFF) << 8 | (RM >> 8 & 0xFF); return; /* SWAP.B */
    case 0x9: RN = RM << 16 | RM >> 16; return;                                      /* SWAP.W */
    case 0xA: u = 0 - (uint64_t)RM - T; RN = (uint32_t)u; set_t(cpu, u >> 32 & 1); return; /* NEGC */
    case 0xB: RN = 0 - RM; return;
    case 0xC: RN = RM & 0xFF; return;
    case 0xD: RN = RM & 0xFFFF; return;
    case 0xE: RN = sext8(RM); return;
    case 0xF: RN = sext16(RM); return;
    }
}

static void exec_8(Cpu *cpu, uint16_t op, uint32_t pc, unsigned n, unsigned m)
{
    Memory *mem = cpu->mem;
    uint32_t d4 = op & 0xF;
    uint32_t target = pc + 4 + sext8(op & 0xFF) * 2;
    switch (n) {
    case 0x0: mem_write8(mem, RM + d4, (uint8_t)R0); return;
    case 0x1: mem_write16(mem, RM + d4 * 2, (uint16_t)R0); return;
    case 0x2: set_rc(cpu, op & 0xFF); return;               /* SETRC #imm */
    case 0x4: R0 = sext8(mem_read8(mem, RM + d4)); return;
    case 0x5: R0 = sext16(mem_read16(mem, RM + d4 * 2)); return;
    case 0x8: set_t(cpu, R0 == sext8(op & 0xFF)); return;  /* CMP/EQ #imm,R0 */
    case 0x9: if (T) cpu->pc = target; return;              /* BT */
    case 0xA: set_rc(cpu, op & 0xFF); return;               /* LDRC #imm */
    case 0xB: if (!T) cpu->pc = target; return;             /* BF */
    case 0xC: cpu->rs = target; return;                     /* LDRS @(disp,PC) */
    case 0xD: if (T) delayed_branch(cpu, pc, target); return;  /* BT/S */
    case 0xE: cpu->re = target; return;                     /* LDRE @(disp,PC) */
    case 0xF: if (!T) delayed_branch(cpu, pc, target); return; /* BF/S */
    }
    illegal(cpu, op, pc);
}

static void exec_c(Cpu *cpu, uint16_t op, uint32_t pc, unsigned n)
{
    Memory *mem = cpu->mem;
    uint32_t i8 = op & 0xFF;
    uint32_t addr = cpu->gbr + R0;
    switch (n) {
    case 0x0: mem_write8(mem, cpu->gbr + i8, (uint8_t)R0); return;
    case 0x1: mem_write16(mem, cpu->gbr + i8 * 2, (uint16_t)R0); return;
    case 0x2: mem_write32(mem, cpu->gbr + i8 * 4, R0); return;
    case 0x3: /* TRAPA #imm */
        if (cpu->in_delay_slot)
            break;
        cpu->tra = i8 << 2;
        take_exception(cpu, EXC_TRAPA, 0x100, pc + 2);
        return;
    case 0x4: R0 = sext8(mem_read8(mem, cpu->gbr + i8)); return;
    case 0x5: R0 = sext16(mem_read16(mem, cpu->gbr + i8 * 2)); return;
    case 0x6: R0 = mem_read32(mem, cpu->gbr + i8 * 4); return;
    case 0x7: R0 = (pc & ~3u) + 4 + i8 * 4; return;          /* MOVA */
    case 0x8: set_t(cpu, (R0 & i8) == 0); return;
    case 0x9: R0 &= i8; return;
    case 0xA: R0 ^= i8; return;
    case 0xB: R0 |= i8; return;
    case 0xC: set_t(cpu, (mem_read8(mem, addr) & i8) == 0); return;
    case 0xD: mem_write8(mem, addr, (uint8_t)(mem_read8(mem, addr) & i8)); return;
    case 0xE: mem_write8(mem, addr, (uint8_t)(mem_read8(mem, addr) ^ i8)); return;
    case 0xF: mem_write8(mem, addr, (uint8_t)(mem_read8(mem, addr) | i8)); return;
    }
    illegal(cpu, op, pc);
}

static void exec(Cpu *cpu, uint32_t pc)
{
    Memory *mem = cpu->mem;
    cpu->op_pc = pc;
    uint16_t op = mem_read16(mem, pc);
    if (mem->fault)
        return;
    unsigned n = op >> 8 & 0xF, m = op >> 4 & 0xF;
    uint32_t i8 = op & 0xFF;
    cpu->pc = pc + 2;
    cpu->instructions++;

    switch (op >> 12) {
    case 0x0: exec_0(cpu, op, pc, n, m); return;
    case 0x1: mem_write32(mem, RN + (op & 0xF) * 4, RM); return;
    case 0x2: exec_2(cpu, op, pc, n, m); return;
    case 0x3: exec_3(cpu, op, pc, n, m); return;
    case 0x4: exec_4(cpu, op, pc, n, m); return;
    case 0x5: RN = mem_read32(mem, RM + (op & 0xF) * 4); return;
    case 0x6: exec_6(cpu, op, n, m); return;
    case 0x7: RN += sext8(i8); return;
    case 0x8: exec_8(cpu, op, pc, n, m); return;
    case 0x9: RN = sext16(mem_read16(mem, pc + 4 + i8 * 2)); return;
    case 0xA: delayed_branch(cpu, pc, pc + 4 + sext12(op & 0xFFF) * 2); return;              /* BRA */
    case 0xB: cpu->pr = pc + 4; delayed_branch(cpu, pc, pc + 4 + sext12(op & 0xFFF) * 2); return; /* BSR */
    case 0xC: exec_c(cpu, op, pc, n); return;
    case 0xD: RN = mem_read32(mem, (pc & ~3u) + 4 + i8 * 4); return;
    case 0xE: RN = sext8(i8); return;
    case 0xF: illegal(cpu, op, pc); return; /* DSP data instructions / FPU: not emulated */
    }
}

static const char *fault_name(FaultKind k)
{
    switch (k) {
    case FAULT_MISALIGNED: return "misaligned";
    case FAULT_READONLY: return "read-only";
    default: return "unmapped";
    }
}

/* Turns a memory fault from the last instruction into an exception (a TLB
 * miss for unmapped U0/P3 addresses, an address error otherwise). */
static void memory_exception(Cpu *cpu)
{
    Memory *mem = cpu->mem;
    uint32_t addr = mem->fault_addr;
    bool write = mem->fault_write;
    FaultKind kind = mem->fault;
    mem->fault = FAULT_NONE;

    cpu_report(cpu, "cpu: %s %s at 0x%08X by instruction at 0x%08X", fault_name(kind),
            write ? "write" : "read", addr, cpu->op_pc);
    cpu->tea = addr;
    bool translated = addr < 0x80000000u || (addr >= 0xC0000000u && addr < 0xE0000000u);
    if (kind == FAULT_UNMAPPED && translated)
        take_exception(cpu, write ? EXC_TLB_MISS_WRITE : EXC_TLB_MISS_READ, 0x400, cpu->op_pc);
    else if (kind == FAULT_READONLY)
        take_exception(cpu, EXC_TLB_PROT_WRITE, 0x100, cpu->op_pc);
    else
        take_exception(cpu, write ? EXC_ADDR_ERR_WRITE : EXC_ADDR_ERR_READ, 0x100, cpu->op_pc);
}

void cpu_reset(Cpu *cpu, Memory *mem, uint32_t entry, uint32_t stack_top)
{
    SyscallHandler handler = cpu->syscall;
    void *user = cpu->syscall_user;
    EventHandler event = cpu->event;
    void *event_user = cpu->event_user;
    bool trace = cpu->trace_exceptions;
    *cpu = (Cpu){0};
    cpu->mem = mem;
    cpu->syscall = handler;
    cpu->syscall_user = user;
    cpu->event = event;
    cpu->event_user = event_user;
    cpu->trace_exceptions = trace;
    cpu->next_event = UINT64_MAX;
    cpu->pc = entry;
    cpu->pr = EXIT_ADDR;
    cpu->sr = SR_MD; /* add-ins run in privileged mode with bank 0 */
    cpu->r[15] = stack_top;
    cpu->state = CPU_RUNNING;
}

/* Executes the instruction at PC (or the syscall/exit it stands for). */
static inline void step_one(Cpu *cpu)
{
    if (cpu->pc == SYSCALL_ENTRY) {
        if (cpu->syscall) {
            cpu->syscall(cpu, cpu->syscall_user);
        } else {
            cpu_report(cpu, "cpu: syscall 0x%04X with no handler", cpu->r[0]);
            cpu->state = CPU_ERROR;
        }
        cpu->attention = true;
        return;
    }
    if (cpu->pc == EXIT_ADDR) {
        cpu->state = CPU_EXITED;
        cpu->attention = true;
        return;
    }
    if (cpu->pc == CALL_RETURN) {
        cpu->state = CPU_RETURNED;
        cpu->attention = true;
        return;
    }

    cpu->exception = false;
    exec(cpu, cpu->pc);
    if (cpu->mem->stall_cycles) { /* uncached memory accesses */
        cpu->instructions += cpu->mem->stall_cycles;
        cpu->mem->stall_cycles = 0;
    }
    if (cpu->mem->fault) {
        memory_exception(cpu);
        return;
    }

    /* DSP repeat loop: after the last instruction, go back to RS until the
     * counter runs out (RS and RE both point at instructions of the loop). */
    if (cpu->sr & SR_RC) {
        uint32_t rc = (cpu->sr & SR_RC) >> 16;
        if (!cpu->exception && cpu->op_pc == cpu->re && cpu->pc == cpu->op_pc + 2) {
            set_rc(cpu, --rc);
            if (rc)
                cpu->pc = cpu->rs;
        }
    }
}

/* Runs until cycle end. The inner loop only executes instructions; events,
 * interrupts and sleep are handled between its runs. */
static void run_until(Cpu *cpu, uint64_t end)
{
    while (cpu->state == CPU_RUNNING && cpu->instructions < end) {
        cpu->attention = false;
        if (cpu->instructions >= cpu->next_event && cpu->event)
            cpu->event(cpu, cpu->event_user);

        if (cpu->irq_level > (int)((cpu->sr & SR_IMASK) >> 4) && !(cpu->sr & SR_BL) && cpu->vbr)
            accept_interrupt(cpu);

        if (cpu->sleeping) {
            /* Time passes until the next event (which may raise an interrupt). */
            uint64_t t = cpu->next_event < end ? cpu->next_event : end;
            if (t > cpu->instructions)
                cpu->instructions = t;
            continue;
        }

        while (!cpu->attention && cpu->state == CPU_RUNNING && cpu->instructions < end &&
               cpu->instructions < cpu->next_event)
            step_one(cpu);
    }
}

void cpu_run(Cpu *cpu, uint64_t max_instructions)
{
    run_until(cpu, cpu->instructions + max_instructions);
}

bool cpu_call(Cpu *cpu, uint32_t fn, uint64_t max_cycles)
{
    if (cpu->state != CPU_RUNNING)
        return false;
    Cpu saved = *cpu;
    cpu->pc = fn;
    cpu->pr = CALL_RETURN;
    cpu->r[15] = (saved.r[15] - 64) & ~3u; /* below what the interrupted code uses */
    cpu->sleeping = false;
    cpu->in_delay_slot = false;
    run_until(cpu, cpu->instructions + max_cycles);
    CpuState state = cpu->state;
    if (state == CPU_RUNNING || state == CPU_ERROR || state == CPU_EXITED) {
        if (state == CPU_RUNNING)
            cpu_report(cpu, "function at 0x%08X called by the OS did not return", fn);
        cpu->state = state == CPU_EXITED ? CPU_EXITED : CPU_ERROR;
        return false;
    }
    /* Returned: back to the interrupted code, keeping what moved on. */
    uint64_t instructions = cpu->instructions, next_event = cpu->next_event;
    int irq_level = cpu->irq_level;
    uint32_t irq_code = cpu->irq_code;
    *cpu = saved;
    cpu->instructions = instructions;
    cpu->next_event = next_event;
    cpu->irq_level = irq_level;
    cpu->irq_code = irq_code;
    cpu->attention = true;
    return true;
}

void cpu_dump(const Cpu *cpu)
{
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "r%-2d=%08X%s", i, cpu->r[i], i % 4 == 3 ? "\n" : "  ");
    fprintf(stderr, "pc =%08X  pr =%08X  sr =%08X  gbr=%08X  vbr=%08X\n",
            cpu->pc, cpu->pr, cpu->sr, cpu->gbr, cpu->vbr);
    fprintf(stderr, "spc=%08X  ssr=%08X  mach=%08X macl=%08X\n", cpu->spc, cpu->ssr, cpu->mach, cpu->macl);
    fprintf(stderr, "last op at %08X  %llu instructions\n", cpu->op_pc,
            (unsigned long long)cpu->instructions);
}
