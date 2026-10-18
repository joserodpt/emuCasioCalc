/* CPU tests: small hand-assembled SH programs, checked against C. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Loads prog at ADDIN_BASE, sets r0-r3, runs until it returns (rts to EXIT_ADDR). */
static Cpu run(const uint16_t *prog, size_t count, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
    static Memory mem;
    mem_free(&mem);
    mem_init(&mem);
    uint8_t *rom = mem_add_ram(&mem, ADDIN_BASE, 0x1000, false);
    mem_add_ram(&mem, URAM_BASE, 0x1000, true);
    for (size_t i = 0; i < count; i++) {
        rom[2 * i] = prog[i] >> 8;
        rom[2 * i + 1] = prog[i] & 0xFF;
    }
    Cpu cpu = {0};
    cpu_reset(&cpu, &mem, ADDIN_BASE, URAM_BASE + 0x1000);
    cpu.r[0] = r0, cpu.r[1] = r1, cpu.r[2] = r2, cpu.r[3] = r3;
    cpu_run(&cpu, 100000);
    if (cpu.state != CPU_EXITED) {
        failures++;
        printf("FAIL: program did not exit cleanly\n");
        cpu_dump(&cpu);
    }
    return cpu;
}

#define RUN(prog, ...) run(prog, sizeof prog / sizeof prog[0], __VA_ARGS__)

static void test_unsigned_division(void)
{
    /* r2 = (r1:r2) / r0 with DIV0U + 32 x (ROTCL r2; DIV1 r0,r1) + ROTCL r2 */
    uint16_t prog[1 + 64 + 1 + 2];
    size_t k = 0;
    prog[k++] = 0x0019;
    for (int i = 0; i < 32; i++) {
        prog[k++] = 0x4224;
        prog[k++] = 0x3104;
    }
    prog[k++] = 0x4224;
    prog[k++] = 0x000B;
    prog[k++] = 0x0009;

    srand(1);
    for (int i = 0; i < 2000; i++) {
        uint32_t a = (uint32_t)rand() << 1 ^ (uint32_t)rand();
        uint32_t b = i < 1000 ? (uint32_t)rand() % 1000 + 1 : (uint32_t)rand() << 1 ^ (uint32_t)rand();
        if (!b) b = 1;
        Cpu c = RUN(prog, b, 0, a, 0);
        CHECK(c.r[2] == a / b, "%u / %u = %u, got %u", a, b, a / b, c.r[2]);
    }
}

static void test_addc_64bit(void)
{
    static const uint16_t prog[] = {0x0008, 0x302E, 0x313E, 0x000B, 0x0009}; /* clrt; addc r2,r0; addc r3,r1 */
    Cpu c = RUN(prog, 0xFFFFFFFFu, 1, 1, 2);
    CHECK(c.r[0] == 0 && c.r[1] == 4, "got r1:r0 = %08X:%08X", c.r[1], c.r[0]);
}

static void test_call_delay_slot_and_literal(void)
{
    static const uint16_t prog[] = {
        0x4F22,         /* 00 sts.l pr,@-r15 */
        0xB003,         /* 02 bsr 0x0C */
        0xE405,         /* 04 mov #5,r4       (delay slot) */
        0x4F26,         /* 06 lds.l @r15+,pr */
        0x000B,         /* 08 rts */
        0x0009,         /* 0A nop */
        0xD001,         /* 0C mov.l @(0x14),r0 */
        0x000B,         /* 0E rts */
        0x304C,         /* 10 add r4,r0       (delay slot) */
        0x0009,         /* 12 */
        0x1234, 0x5678, /* 14 .long 0x12345678 */
    };
    Cpu c = RUN(prog, 0, 0, 0, 0);
    CHECK(c.r[0] == 0x1234567D, "got %08X", c.r[0]);
}

static void test_dt_loop(void)
{
    static const uint16_t prog[] = {0xE10A, 0xE000, 0x301C, 0x4110, 0x8BFC, 0x000B, 0x0009};
    Cpu c = RUN(prog, 0, 0, 0, 0);
    CHECK(c.r[0] == 55, "sum 1..10 = %u", c.r[0]);
}

static void test_shifts(void)
{
    /* mov r0,r2; shad r1,r0; shld r1,r2 */
    static const uint16_t prog[] = {0x6203, 0x401C, 0x421D, 0x000B, 0x0009};
    Cpu c = RUN(prog, 0x80000000u, (uint32_t)-4, 0, 0);
    CHECK(c.r[0] == 0xF8000000u, "shad got %08X", c.r[0]);
    CHECK(c.r[2] == 0x08000000u, "shld got %08X", c.r[2]);
}

static void test_dmuls(void)
{
    static const uint16_t prog[] = {0x301D, 0x000B, 0x0009}; /* dmuls.l r1,r0 */
    Cpu c = RUN(prog, (uint32_t)-3, 7, 0, 0);
    CHECK(c.mach == 0xFFFFFFFFu && c.macl == 0xFFFFFFEBu, "got %08X:%08X", c.mach, c.macl);
}

static void test_dsp_repeat_loop(void)
{
    /* ldrs 1f; ldre 2f; ldrc r1; nop; 1: add #1,r0; add #2,r2; 2: add #3,r3
     * (the loop runs r1 times, both labels included) */
    static const uint16_t prog[] = {0x8C02, 0x8E03, 0x4134, 0x0009, 0x7001, 0x7202, 0x7303, 0x000B, 0x0009};
    Cpu c = RUN(prog, 0, 5, 0, 0);
    CHECK(c.r[0] == 5 && c.r[2] == 10 && c.r[3] == 15, "got %u %u %u", c.r[0], c.r[2], c.r[3]);
    CHECK((c.sr & SR_RC) == 0, "RC left at %u", (c.sr & SR_RC) >> 16);
}

static void test_trapa_rte_banks(void)
{
    /* VBR = base+0x200; r1 = 7; trapa #0x21; add #1,r1. The handler (bank 1)
     * adds 10 to the user bank's r1 through r1_bank and clobbers its own r1. */
    static const uint16_t prog[0x186] = {
        0xD003, 0x402E, 0xE107, 0xC321, 0x7101, 0x000B, 0x0009, 0x0009, 0x0030, 0x0200,
        [0x180] = 0x0092, 0x700A, 0x409E, 0xE163, 0x002B, 0x0009,
    };
    Cpu c = RUN(prog, 0, 0, 0, 0);
    CHECK(c.r[1] == 18, "r1 = %u", c.r[1]);
    CHECK(c.expevt == EXC_TRAPA && c.tra == 0x84, "EXPEVT %03X TRA %X", c.expevt, c.tra);
    CHECK(c.sr == SR_MD, "SR %08X after RTE", c.sr);
}

static void test_movua(void)
{
    /* store 11223344 55667788 at @r1, then movua.l @(r1+1)+,r0 */
    static const uint16_t prog[] = {0xD203, 0x2122, 0xD203, 0x1121, 0x7101, 0x41E9, 0x000B, 0x0009,
                                    0x1122, 0x3344, 0x5566, 0x7788};
    Cpu c = RUN(prog, 0, URAM_BASE, 0, 0);
    CHECK(c.r[0] == 0x22334455u && c.r[1] == URAM_BASE + 5, "got %08X, r1 %08X", c.r[0], c.r[1]);
}

int main(void)
{
    test_unsigned_division();
    test_addc_64bit();
    test_call_delay_slot_and_literal();
    test_dt_loop();
    test_shifts();
    test_dmuls();
    test_dsp_repeat_loop();
    test_trapa_rte_banks();
    test_movua();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all CPU tests passed\n");
    return 0;
}
