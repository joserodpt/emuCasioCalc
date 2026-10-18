/*
 * High-level emulation of the Casio OS: instead of running the OS, we catch
 * jumps to SYSCALL_ENTRY and implement each syscall on the host.
 *
 * Calling convention (SH ABI): r0 = syscall number, args in r4-r7, then on
 * the stack at @r15, @(4,r15)... Return value goes in r0, then jump to PR.
 *
 * Syscall numbers and signatures: WikiPrizm (prizm.cemetech.net) and the
 * syscall stubs in libfxcg (github.com/Jonimoose/libfxcg). This file has
 * the table and the system calls; drawing, keyboard and storage are in
 * syscalls_text.c, syscalls_keys.c, syscalls_bfile.c and syscalls_mcs.c.
 */
#include "syscalls.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "syscalls_impl.h"

typedef uint32_t (*SyscallFn)(Emu *emu, const uint32_t *args);

typedef struct {
    uint32_t number;
    const char *name;
    SyscallFn fn;
} Syscall;

void sc_string(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    mem_read_cstr(&emu->mem, addr, out, cap);
}

/* RTC_GetTicks(): 1/128 s ticks of emulated time. */
static SC(sc_rtc_get_ticks)
{
    (void)a;
    return (uint32_t)(emu->cpu.instructions * 128 / sh7305_iphi_hz(&emu->hw));
}

/* RTC_GetTime(hour*, minute*, second*, millisecond*): host time at startup
 * plus emulated time, like the RTC registers. */
static SC(sc_rtc_get_time)
{
    uint64_t hz = sh7305_iphi_hz(&emu->hw), now = emu->cpu.instructions;
    time_t t = (time_t)(emu->hw.rtc_base + (int64_t)(now / hz));
    struct tm tm;
    localtime_r(&t, &tm);
    uint32_t v[4] = {(uint32_t)tm.tm_hour, (uint32_t)tm.tm_min, (uint32_t)tm.tm_sec,
                     (uint32_t)(now % hz * 1000 / hz)};
    for (int i = 0; i < 4; i++)
        if (a[i])
            mem_write32(&emu->mem, a[i], v[i]);
    return 0;
}

/* OS_InnerWait_ms(ms): time passes without running the add-in. */
static SC(sc_wait_ms)
{
    emu->cpu.instructions += (uint64_t)a[0] * (sh7305_iphi_hz(&emu->hw) / 1000);
    return 0;
}

/* Bump allocator in RAM: good enough to start; free() does nothing yet. */
static SC(sc_malloc)
{
    uint32_t size = (a[0] + 7) & ~7u;
    if (emu->heap_next + size > emu->layout->heap_base + emu->layout->heap_size)
        return 0;
    uint32_t p = emu->heap_next;
    emu->heap_next += size;
    return p;
}

/* Timer_Install(id, handler, ms): OS timers would need their callbacks
 * run like interrupts; report that none is free. (gint's return-to-menu
 * uses one, and simply comes back when it can't get it.) */
static SC(sc_timer_install)
{
    (void)emu; (void)a;
    return (uint32_t)-1;
}

static SC(sc_get_setup_setting)
{
    return emu->setup[a[0] & 0xFF];
}

static SC(sc_set_setup_setting)
{
    emu->setup[a[0] & 0xFF] = (uint8_t)a[1];
    return 0;
}

/* memcpy(dst, src, n) and memset(p, c, n): the OS's own C library. */
static SC(sc_memcpy)
{
    Memory *mem = &emu->mem;
    uint8_t *d = mem_ptr(mem, a[0], a[2]), *s = mem_ptr(mem, a[1], a[2]);
    if (d && s && mem->wr[a[0] >> PAGE_SHIFT]) {
        memmove(d, s, a[2]);
    } else {
        for (uint32_t i = 0; i < a[2]; i++)
            mem_write8(mem, a[0] + i, mem_read8(mem, a[1] + i));
    }
    return a[0];
}

static SC(sc_memset)
{
    Memory *mem = &emu->mem;
    uint8_t *d = mem_ptr(mem, a[0], a[2]);
    if (d && mem->wr[a[0] >> PAGE_SHIFT]) {
        memset(d, (int)(a[1] & 0xFF), a[2]);
    } else {
        for (uint32_t i = 0; i < a[2]; i++)
            mem_write8(mem, a[0] + i, (uint8_t)a[1]);
    }
    return a[0];
}

static SC(sc_strlen)
{
    uint32_t n = 0;
    while (mem_read8(&emu->mem, a[0] + n) && !emu->mem.fault)
        n++;
    return n;
}

/* itoa(value, unsigned char *result): decimal. */
static SC(sc_itoa)
{
    char buf[16];
    int len = snprintf(buf, sizeof buf, "%d", (int32_t)a[0]);
    for (int i = 0; i <= len; i++)
        mem_write8(&emu->mem, a[1] + (uint32_t)i, (uint8_t)buf[i]);
    return 0;
}

/* Calls that only change OS state we don't show (status bar contents,
 * cursor blinking, launch checks) or that have nothing to do here. */
static SC(sc_nothing)
{
    (void)emu; (void)a;
    return 0;
}

static const Syscall SYSCALLS[] = {
    {0x0029, "GlibAddinAplExecutionCheck", sc_nothing},
    {0x0031, "GetSetupSetting", sc_get_setup_setting},
    {0x0032, "SetSetupSetting", sc_set_setup_setting},
    {0x01E6, "GetVRAMAddress", sc_get_vram_address},
    {0x01E9, "GetMiniGlyphPtr", sc_get_mini_glyph_ptr},
    {0x0239, "PrintCXY", sc_print_cxy},
    {0x023B, "PrintMiniGlyph", sc_print_mini_glyph},
    {0x023C, "PrintMini", sc_print_mini},
    {0x025F, "Bdisp_PutDisp_DD", sc_bdisp_putdisp_dd},
    {0x0263, "Bdisp_SetPoint_VRAM", sc_bdisp_setpoint_vram},
    {0x0272, "Bdisp_AllClr_VRAM", sc_bdisp_allclr_vram},
    {0x02A8, "DrawFrame", sc_draw_frame},
    {0x02B7, "EnableStatusArea", sc_nothing},
    {0x02B8, "DefineStatusAreaFlags", sc_nothing},
    {0x02C0, "RTC_GetTime", sc_rtc_get_time},
    {0x02C1, "RTC_GetTicks", sc_rtc_get_ticks},
    {0x08C8, "Cursor_SetFlashOff", sc_nothing},
    {0x08D9, "Timer_Install", sc_timer_install},
    {0x0921, "Bdisp_EnableColor", sc_nothing},
    {0x0D39, "PRGM_GetKey_OS", sc_prgm_get_key_os},
    {0x0EAB, "GetKey", sc_get_key},
    {0x1170, "itoa", sc_itoa},
    {0x120A, "EditMBStringCtrl2", sc_edit_mbstring_ctrl2},
    {0x1218, "DisplayMBString2", sc_display_mbstring2},
    {0x1224, "EditMBStringChar", sc_edit_mbstring_char},
    {0x12BF, "GetKeyWait_OS", sc_get_key_wait_os},
    {0x12C7, "Keyboard_ClearKeyBuffer", sc_nothing},
    {0x12F3, "GetFKeyPtr", sc_get_fkey_ptr},
    {0x1349, "strlen", sc_strlen},
    {0x1511, "memset", sc_memset},
    {0x154D, "MCSPutVar2", sc_mcs_put_var2},
    {0x1552, "MCSOvwDat2", sc_mcs_ovw_dat2},
    {0x1558, "MCSDelVar2", sc_mcs_del_var2},
    {0x1562, "MCSGetDlen2", sc_mcs_get_dlen2},
    {0x1563, "MCSGetData1", sc_mcs_get_data1},
    {0x17F7, "MsgBoxPush", sc_msgbox_push},
    {0x17F9, "MsgBoxPop", sc_msgbox_pop},
    {0x18EC, "PrintXY_2", sc_print_xy_2},
    {0x18F9, "PrintXY", sc_print_xy},
    {0x1BB4, "OS_InnerWait_ms", sc_wait_ms},
    {0x1D77, "DefineStatusMessage", sc_nothing},
    {0x1DA3, "Bfile_OpenFile_OS", sc_bfile_open},
    {0x1DA4, "Bfile_CloseFile_OS", sc_bfile_close},
    {0x1DAC, "Bfile_ReadFile_OS", sc_bfile_read},
    {0x1DAE, "Bfile_CreateEntry_OS", sc_bfile_create},
    {0x1DAF, "Bfile_WriteFile_OS", sc_bfile_write},
    {0x1DB4, "Bfile_DeleteEntry", sc_bfile_delete},
    {0x1D81, "DisplayStatusArea", sc_nothing},
    {0x1DD0, "memcpy", sc_memcpy},
    {0x1E62, "SaveVRAM_1", sc_save_vram_1},
    {0x1E63, "LoadVRAM_1", sc_load_vram_1},
    {0x1F42, "sys_free", sc_nothing},
    {0x1F44, "sys_malloc", sc_malloc},
};

/* Stack arguments; near the top of the stack they may not exist. */
static uint32_t stack_arg(Emu *emu, int index)
{
    uint32_t addr = emu->cpu.r[15] + 4 * (uint32_t)index;
    return mem_ptr(&emu->mem, addr, 4) ? mem_read32(&emu->mem, addr) : 0;
}

static void dispatch(Cpu *cpu, void *user)
{
    Emu *emu = user;
    uint32_t number = cpu->r[0];
    uint32_t args[SYSCALL_ARGS] = {cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7]};
    for (int i = 4; i < SYSCALL_ARGS; i++)
        args[i] = stack_arg(emu, i - 4);

    for (size_t i = 0; i < sizeof SYSCALLS / sizeof SYSCALLS[0]; i++) {
        if (SYSCALLS[i].number != number)
            continue;
        if (emu->trace)
            printf("%9.3f ms  syscall %04X %-24s (%08X, %08X, %08X, %08X)\n",
                   (double)cpu->instructions * 1000 / sh7305_iphi_hz(&emu->hw), number, SYSCALLS[i].name,
                   args[0], args[1], args[2], args[3]);
        cpu->r[0] = SYSCALLS[i].fn(emu, args);
        cpu->pc = cpu->pr;
        if (emu->mem.fault) {
            fprintf(stderr, "syscall %04X %s: bad address 0x%08X\n", number, SYSCALLS[i].name,
                    emu->mem.fault_addr);
            cpu->state = CPU_ERROR;
        }
        return;
    }
    fprintf(stderr, "unimplemented syscall 0x%04X (r4=%08X r5=%08X r6=%08X r7=%08X), called from 0x%08X\n",
            number, args[0], args[1], args[2], args[3], cpu->pr);
    cpu->state = CPU_ERROR;
}

void syscalls_install(Emu *emu)
{
    emu->cpu.syscall = dispatch;
    emu->cpu.syscall_user = emu;
}
