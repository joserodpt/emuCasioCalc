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
 * syscalls_draw.c, syscalls_status.c, syscalls_time.c, syscalls_text.c, syscalls_keys.c, syscalls_bfile.c and syscalls_mcs.c.
 */
#include "syscalls.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "syscalls_impl.h"

typedef uint32_t (*SyscallFn)(Emu *emu, const uint32_t *args);

/* Emulated time a syscall takes: the real OS runs code for each one (a
 * few microseconds for simple calls); slow calls add their own cost with
 * sc_spend(). Estimates, there are no published timings. */
#define SYSCALL_CYCLES 300

typedef struct {
    uint32_t number;
    const char *name;
    SyscallFn fn;
} Syscall;

void sc_spend(Emu *emu, double cycles)
{
    if (emu->realistic)
        emu->cpu.instructions += (uint64_t)cycles;
}

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
    time_advance(emu, (uint64_t)a[0] * (sh7305_iphi_hz(&emu->hw) / 1000));
    return 0;
}

/*
 * The OS heap (sys_malloc, sys_free, sys_realloc): first fit over blocks
 * laid out back to back in the model's heap area. Each block starts with
 * an 8-byte header in emulated memory: its total size, and 1 if in use.
 * Free neighbours are merged when the heap is walked.
 */
#define BLOCK_HEADER 8
#define BLOCK_MIN    16

static uint32_t heap_end(Emu *emu)
{
    return emu->layout->heap_base + emu->layout->heap_size;
}

static void heap_init(Emu *emu)
{
    if (emu->heap_ready)
        return;
    mem_write32(&emu->mem, emu->layout->heap_base, emu->layout->heap_size);
    mem_write32(&emu->mem, emu->layout->heap_base + 4, 0);
    emu->heap_ready = true;
}

static uint32_t heap_alloc(Emu *emu, uint32_t request)
{
    Memory *mem = &emu->mem;
    heap_init(emu);
    uint32_t need = ((request + 7) & ~7u) + BLOCK_HEADER;
    for (uint32_t b = emu->layout->heap_base; b < heap_end(emu);) {
        uint32_t size = mem_read32(mem, b), used = mem_read32(mem, b + 4);
        if (size < BLOCK_HEADER || b + size > heap_end(emu))
            return 0; /* the add-in overwrote a header */
        if (!used) {
            /* merge the free blocks that follow */
            uint32_t next = b + size;
            while (next < heap_end(emu) && !mem_read32(mem, next + 4)) {
                size += mem_read32(mem, next);
                next = b + size;
            }
            mem_write32(mem, b, size);
            if (size >= need) {
                if (size - need >= BLOCK_MIN) { /* split */
                    mem_write32(mem, b + need, size - need);
                    mem_write32(mem, b + need + 4, 0);
                    size = need;
                }
                mem_write32(mem, b, size);
                mem_write32(mem, b + 4, 1);
                return b + BLOCK_HEADER;
            }
        }
        b += size;
    }
    return 0;
}

/* The block of a pointer we returned, or 0. */
static uint32_t heap_block(Emu *emu, uint32_t p)
{
    if (p < emu->layout->heap_base + BLOCK_HEADER || p >= heap_end(emu))
        return 0;
    return p - BLOCK_HEADER;
}

static SC(sc_malloc)
{
    return heap_alloc(emu, a[0]);
}

static SC(sc_free)
{
    uint32_t b = heap_block(emu, a[0]);
    if (b)
        mem_write32(&emu->mem, b + 4, 0);
    return 0;
}

/* sys_realloc(p, size): a new block with the old contents. */
static SC(sc_realloc)
{
    uint32_t old = heap_block(emu, a[0]);
    if (!a[0])
        return heap_alloc(emu, a[1]);
    if (!old)
        return 0;
    uint32_t p = heap_alloc(emu, a[1]);
    if (!p)
        return 0; /* the old block stays valid */
    uint32_t old_size = mem_read32(&emu->mem, old) - BLOCK_HEADER;
    uint32_t n = old_size < a[1] ? old_size : a[1];
    uint8_t *d = mem_ptr(&emu->mem, p, n), *s = mem_ptr(&emu->mem, a[0], n);
    if (d && s)
        memmove(d, s, n);
    mem_write32(&emu->mem, old + 4, 0);
    return p;
}

/* Timer_Install(id, handler, ms): OS timers would need their callbacks
 * run like interrupts; report that none is free. (gint's return-to-menu
 * uses one, and simply comes back when it can't get it.) */
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

/* memcmp(p1, p2, n): like C's, the difference of the first differing bytes. */
static SC(sc_memcmp)
{
    for (uint32_t i = 0; i < a[2]; i++) {
        int d = mem_read8(&emu->mem, a[0] + i) - mem_read8(&emu->mem, a[1] + i);
        if (d)
            return (uint32_t)d;
    }
    return 0;
}

/* Hex conversions. The *ToHex calls write uppercase digits without a NUL;
 * HexTo* accept either case and return 0, or 1 on a non-hex character
 * (then *result is left alone). */
static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool hex_read(Emu *emu, uint32_t addr, int digits, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < digits; i++) {
        int n = hex_value(mem_read8(&emu->mem, addr + (uint32_t)i));
        if (n < 0)
            return false;
        v = v << 4 | (uint32_t)n;
    }
    *out = v;
    return true;
}

static void hex_write(Emu *emu, uint32_t addr, uint32_t value, int digits)
{
    for (int i = 0; i < digits; i++)
        mem_write8(&emu->mem, addr + (uint32_t)i,
                   (uint8_t)"0123456789ABCDEF"[value >> 4 * (digits - 1 - i) & 15]);
}

/* HexToNibble(char, unsigned char *result) */
static SC(sc_hex_to_nibble)
{
    int n = hex_value((uint8_t)a[0]);
    if (n < 0)
        return 1;
    mem_write8(&emu->mem, a[1], (uint8_t)n);
    return 0;
}

/* HexToByte(const char *two_digits, unsigned char *result) */
static SC(sc_hex_to_byte)
{
    uint32_t v;
    if (!hex_read(emu, a[0], 2, &v))
        return 1;
    mem_write8(&emu->mem, a[1], (uint8_t)v);
    return 0;
}

/* HexToWord(const char *four_digits, unsigned short *result) */
static SC(sc_hex_to_word)
{
    uint32_t v;
    if (!hex_read(emu, a[0], 4, &v))
        return 1;
    mem_write16(&emu->mem, a[1], (uint16_t)v);
    return 0;
}

/* NibbleToHex / ByteToHex / WordToHex(value, char *result) */
static SC(sc_nibble_to_hex)
{
    hex_write(emu, a[1], a[0], 1);
    return 0;
}

static SC(sc_byte_to_hex)
{
    hex_write(emu, a[1], a[0], 2);
    return 0;
}

static SC(sc_word_to_hex)
{
    hex_write(emu, a[1], a[0], 4);
    return 0;
}

/* LongToAscHex(value, char *result, digits): the low digits of value. */
static SC(sc_long_to_asc_hex)
{
    int digits = (int32_t)a[2];
    if (digits > 8) digits = 8;
    if (digits > 0)
        hex_write(emu, a[1], a[0], digits);
    return 0;
}

/* itoa(value, unsigned char *result): decimal; returns result. */
static SC(sc_itoa)
{
    char buf[16];
    int len = snprintf(buf, sizeof buf, "%d", (int32_t)a[0]);
    for (int i = 0; i <= len; i++)
        mem_write8(&emu->mem, a[1] + (uint32_t)i, (uint8_t)buf[i]);
    return a[1]; /* add-ins use the result as the string */
}

/* The serial port (3-pin link cable): we behave like a port with nothing
 * plugged in. Opening works, nothing ever arrives, what is sent is lost. */
static SC(sc_serial_open)
{
    (void)a;
    emu->serial_open = true;
    return 0;
}

static SC(sc_serial_is_open)
{
    (void)a;
    return emu->serial_open ? 1 : 3; /* 3: not open (any nonzero code) */
}

static SC(sc_serial_poll_rx)
{
    (void)emu; (void)a;
    return 0; /* bytes waiting */
}

static SC(sc_serial_poll_tx)
{
    (void)emu; (void)a;
    return 256; /* free space in the send buffer */
}

/* Serial_Read(out, size, short *count): nothing to read. */
static SC(sc_serial_read)
{
    if (a[2])
        mem_write16(&emu->mem, a[2], 0);
    return 1; /* "no data" */
}

static SC(sc_serial_write)
{
    (void)emu; (void)a;
    return 0;
}

/*
 * System information. The OS version is the one each model shipped late in
 * its life (fx-CG50 03.60, fx-CG10/20 02.02); how its parts map to the four
 * outputs isn't documented, this is our reading. The battery voltage is a
 * plausible fixed value, in hundredths of a volt (an assumption).
 */
static SC(sc_os_version)
{
    Memory *mem = &emu->mem;
    bool cg50 = emu->model == MODEL_CG50;
    if (a[0]) mem_write8(mem, a[0], cg50 ? 3 : 2);
    if (a[1]) mem_write8(mem, a[1], cg50 ? 60 : 2);
    if (a[2]) mem_write16(mem, a[2], cg50 ? 2 : 2);
    if (a[3]) mem_write16(mem, a[3], 0);
    return 0;
}

/* GetAppName(name): the add-in's internal name; returns name. */
static SC(sc_get_app_name)
{
    for (size_t i = 0; i <= strlen(emu->app_name); i++)
        mem_write8(&emu->mem, a[0] + (uint32_t)i, (uint8_t)emu->app_name[i]);
    return a[0];
}

static SC(sc_battery_voltage)
{
    (void)a;
    return (uint32_t)emu->battery_cv;
}

static SC(sc_get_battery_type) { (void)a; return (uint32_t)emu->battery_type; }
static SC(sc_set_battery_type) { emu->battery_type = (int)a[0]; return 0; }
static SC(sc_get_backlight) { (void)a; return (uint32_t)emu->backlight_halfmin; }
static SC(sc_set_backlight) { emu->backlight_halfmin = (int)(a[0] & 0xFF); return 0; }
static SC(sc_get_power_off) { (void)a; return (uint32_t)emu->power_off_min; }
static SC(sc_set_power_off) { emu->power_off_min = (int)a[0]; return 0; }

static SC(sc_get_stack_ptr)
{
    (void)a;
    return emu->cpu.r[15];
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
    {0x002B, "GlibGetOSVersionInfo", sc_os_version},
    {0x0031, "GetSetupSetting", sc_get_setup_setting},
    {0x0032, "SetSetupSetting", sc_set_setup_setting},
    {0x0111, "Bkey_ClrAllFlags", sc_bkey_clr_all_flags},
    {0x0112, "Bkey_SetFlag", sc_bkey_set_flag},
    {0x0129, "Set_FKeys2", sc_nothing},
    {0x012B, "Set_FKeys1", sc_nothing},
    {0x0131, "FKey_Display", sc_fkey_display},
    {0x01E6, "GetVRAMAddress", sc_get_vram_address},
    {0x01E9, "GetMiniGlyphPtr", sc_get_mini_glyph_ptr},
    {0x01F9, "Print_OS", sc_print_os},
    {0x021B, "PrintMiniMini", sc_print_mini_mini},
    {0x0239, "PrintCXY", sc_print_cxy},
    {0x023B, "PrintMiniGlyph", sc_print_mini_glyph},
    {0x023C, "PrintMini", sc_print_mini},
    {0x025F, "Bdisp_PutDisp_DD", sc_bdisp_putdisp_dd},
    {0x0260, "Bdisp_PutDisp_DD_stripe", sc_putdisp_stripe},
    {0x0262, "Bdisp_SetPointWB_VRAM", sc_bdisp_setpoint_vram},
    {0x0263, "Bdisp_SetPoint_VRAM", sc_bdisp_setpoint_vram},
    {0x0267, "Bdisp_GetPoint_VRAM", sc_get_point_vram},
    {0x026B, "Bdisp_SetPoint_DD", sc_set_point_dd},
    {0x026F, "Bdisp_GetPoint_DD", sc_get_point_dd},
    {0x0272, "Bdisp_AllClr_VRAM", sc_bdisp_allclr_vram},
    {0x0275, "Bdisp_Fill_VRAM", sc_fill_vram},
    {0x028F, "Bdisp_WriteGraphVRAM_WB", sc_write_graph_vram},
    {0x0290, "Bdisp_WriteGraphVRAM", sc_write_graph_vram},
    {0x0291, "Bdisp_WriteGraphDD_WB", sc_write_graph_dd},
    {0x02A3, "FrameColor", sc_frame_color},
    {0x02A8, "DrawFrame", sc_draw_frame},
    {0x02AA, "DirectDrawRectangle", sc_direct_draw_rectangle},
    {0x02B2, "Bdisp_AreaClr", sc_area_clr},
    {0x02B7, "EnableStatusArea", sc_enable_status_area},
    {0x02B8, "DefineStatusAreaFlags", sc_define_status_area_flags},
    {0x02BB, "DrawHeaderLine", sc_draw_header_line},
    {0x02BF, "RTC_Reset", sc_rtc_reset},
    {0x02C0, "RTC_GetTime", sc_rtc_get_time},
    {0x02C1, "RTC_GetTicks", sc_rtc_get_ticks},
    {0x02C2, "RTC_Elapsed_ms", sc_rtc_elapsed_ms},
    {0x02C7, "HourGlass", sc_nothing},       /* status bar busy icon: no status bar */
    {0x08C7, "Cursor_SetFlashOn", sc_nothing}, /* we don't draw the text cursor */
    {0x08C8, "Cursor_SetFlashOff", sc_nothing},
    {0x08CA, "Keyboard_CursorFlash", sc_nothing}, /* function unknown; no text cursor drawn */
    {0x08D2, "SetCursorFlashToggle", sc_nothing}, /* parameter unknown */
    {0x08D9, "Timer_Install", sc_timer_install},
    {0x08DA, "Timer_Deinstall", sc_timer_deinstall},
    {0x08DB, "Timer_Start", sc_timer_start},
    {0x08DC, "Timer_Stop", sc_timer_stop},
    {0x0921, "Bdisp_EnableColor", sc_nothing},
    {0x0924, "Bdisp_Rectangle", sc_rectangle},
    {0x0925, "Bdisp_FilledRectangle", sc_filled_rectangle},
    {0x0926, "Bdisp_FilledRectangleFullColor", sc_filled_rectangle_full},
    {0x0D39, "PRGM_GetKey_OS", sc_prgm_get_key_os},
    {0x0EA0, "Bkey_GetAllFlags", sc_bkey_get_all_flags},
    {0x0EA1, "Bkey_SetAllFlags", sc_bkey_set_all_flags},
    {0x0EAB, "GetKey", sc_get_key},
    {0x1170, "itoa", sc_itoa},
    {0x1186, "GetMainBatteryVoltage", sc_battery_voltage},
    {0x11D6, "CMT_Delay_micros", sc_delay_micros},
    {0x11D7, "CMT_Delay_100micros", sc_delay_100micros},
    {0x11DE, "RTC_SetDateTime", sc_rtc_set_date_time},
    {0x120A, "EditMBStringCtrl2", sc_edit_mbstring_ctrl2},
    {0x1218, "DisplayMBString2", sc_display_mbstring2},
    {0x1224, "EditMBStringChar", sc_edit_mbstring_char},
    {0x12BF, "GetKeyWait_OS", sc_get_key_wait_os},
    {0x12C6, "Keyboard_PutKeycode", sc_put_keycode},
    {0x12C7, "Keyboard_ClearKeyBuffer", sc_nothing},
    {0x12C9, "Keyboard_SpyMatrixCode", sc_spy_matrix_code},
    {0x12D4, "SetBatteryType", sc_set_battery_type},
    {0x12D5, "GetBatteryType", sc_get_battery_type},
    {0x12D8, "SetBacklightDuration", sc_set_backlight},
    {0x12D9, "GetBacklightDuration", sc_get_backlight},
    {0x12F3, "GetFKeyPtr", sc_get_fkey_ptr},
    {0x12FC, "LocalizeMessage1", sc_localize_message1},
    {0x1343, "HexToNibble", sc_hex_to_nibble},
    {0x1344, "HexToByte", sc_hex_to_byte},
    {0x1345, "HexToWord", sc_hex_to_word},
    {0x1346, "NibbleToHex", sc_nibble_to_hex},
    {0x1347, "ByteToHex", sc_byte_to_hex},
    {0x1348, "WordToHex", sc_word_to_hex},
    {0x1349, "strlen", sc_strlen},
    {0x1511, "memset", sc_memset},
    {0x154B, "MCS_CreateDirectory", sc_mcs_create_dir},
    {0x154D, "MCSPutVar2", sc_mcs_put_var2},
    {0x1552, "MCSOvwDat2", sc_mcs_ovw_dat2},
    {0x1558, "MCSDelVar2", sc_mcs_del_var2},
    {0x1562, "MCSGetDlen2", sc_mcs_get_dlen2},
    {0x1563, "MCSGetData1", sc_mcs_get_data1},
    {0x1630, "App_Optimize", sc_nothing},     /* storage optimisation: nothing to do */
    {0x17F7, "MsgBoxPush", sc_msgbox_push},
    {0x17F9, "MsgBoxPop", sc_msgbox_pop},
    {0x17FB, "BoxYLimits", sc_box_y_limits},
    {0x17FD, "BoxInnerClear", sc_box_inner_clear},
    {0x1809, "ProgressBar2", sc_progress_bar2},
    {0x180B, "ProgressBar0", sc_progress_bar0},
    {0x180E, "ProgressBar", sc_progress_bar},
    {0x1815, "ColorIndexDialog1", sc_color_index_dialog},
    {0x1839, "PowerOff", sc_nothing},         /* off and on again at once */
    {0x1841, "LongToAscHex", sc_long_to_asc_hex},
    {0x1863, "locate_OS", sc_locate},
    {0x1883, "PrintLine", sc_print_line},
    {0x18EC, "PrintXY_2", sc_print_xy_2},
    {0x18F9, "PrintXY", sc_print_xy},
    {0x1A0A, "Scrollbar", sc_scrollbar},
    {0x1A2C, "GetStackPtr", sc_get_stack_ptr},
    {0x1BB4, "OS_InnerWait_ms", sc_wait_ms},
    {0x1BB7, "Serial_Open", sc_serial_open},
    {0x1BBA, "Serial_Read", sc_serial_read},
    {0x1BBE, "Serial_Write", sc_serial_write},
    {0x1BBF, "Serial_PollRX", sc_serial_poll_rx},
    {0x1BC0, "Serial_PollTX", sc_serial_poll_tx},
    {0x1BC6, "Serial_IsOpen", sc_serial_is_open},
    {0x1D77, "DefineStatusMessage", sc_define_status_message},
    {0x1D7B, "DefineStatusGlyph", sc_define_status_glyph},
    {0x1D7F, "EnableDisplayHeader", sc_enable_display_header},
    {0x1D81, "DisplayStatusArea", sc_display_status_area},
    {0x1D82, "Bdisp_HeaderText", sc_header_text},
    {0x1D86, "Bdisp_HeaderFill", sc_header_fill},
    {0x1D89, "BatteryIcon", sc_battery_icon},
    /* Status icons with unknown parameters: the flags draw these items. */
    {0x1D8A, "KeyboardIcon", sc_nothing},
    {0x1D8B, "RadIcon", sc_nothing},
    {0x1D8C, "RealIcon", sc_nothing},
    {0x1D8D, "NormIcon", sc_nothing},
    {0x1D8E, "d_c_Icon", sc_nothing},
    {0x1D8F, "LineIcon", sc_nothing},
    {0x1DA3, "Bfile_OpenFile_OS", sc_bfile_open},
    {0x1DA4, "Bfile_CloseFile_OS", sc_bfile_close},
    {0x1DA5, "Bfile_GetMediaFree_OS", sc_bfile_media_free},
    {0x1DA6, "Bfile_GetFileSize_OS", sc_bfile_size},
    {0x1DA9, "Bfile_SeekFile_OS", sc_bfile_seek},
    {0x1DAA, "Bfile_GetBlockAddress", sc_bfile_block_address},
    {0x1DAB, "Bfile_TellFile_OS", sc_bfile_tell},
    {0x1DAC, "Bfile_ReadFile_OS", sc_bfile_read},
    {0x1DAE, "Bfile_CreateEntry_OS", sc_bfile_create},
    {0x1DAF, "Bfile_WriteFile_OS", sc_bfile_write},
    {0x1DB3, "Bfile_RenameEntry", sc_bfile_rename},
    {0x1DB4, "Bfile_DeleteEntry", sc_bfile_delete},
    {0x1DB6, "Bfile_FindFirst_NON_SMEM", sc_bfile_find_first},
    {0x1DB7, "Bfile_FindFirst", sc_bfile_find_first},
    {0x1DB8, "Bfile_FindNext_NON_SMEM", sc_bfile_find_next},
    {0x1DB9, "Bfile_FindNext", sc_bfile_find_next},
    {0x1DBA, "Bfile_FindClose", sc_bfile_find_close},
    {0x1DD0, "memcpy", sc_memcpy},
    {0x1DD1, "memcmp", sc_memcmp},
    {0x1DDA, "Bfile_Name_MatchMask", sc_bfile_match_mask},
    {0x1DDB, "Bfile_NameToStr_ncpy", sc_bfile_name_to_str},
    {0x1DDC, "Bfile_StrToName_ncpy", sc_bfile_str_to_name},
    {0x1E0D, "APP_SYSTEM_LANGUAGE", sc_app_system_language},
    {0x1E23, "ResetAllDialog", sc_nothing},   /* nothing to reset */
    {0x1E4B, "DisplayMessageBox", sc_display_message_box},
    {0x1E50, "GetSecondaryVRAMAddress", sc_get_secondary_vram_address},
    {0x1E60, "SpecialMatrixcodeProcessing", sc_nothing}, /* OS update contact...: nothing here */
    {0x1E62, "SaveVRAM_1", sc_save_vram_1},
    {0x1E63, "LoadVRAM_1", sc_load_vram_1},
    {0x1E6A, "DisplayMainMenu", sc_nothing},  /* no OS main menu */
    {0x1E6E, "SetQuitHandler", sc_nothing}, /* no OS menu to quit through */
    {0x1E90, "SetAutoPowerOffTime", sc_set_power_off},
    {0x1E91, "GetAutoPowerOffTime", sc_get_power_off},
    {0x1E99, "GetGetkeyToMainFunctionReturnFlag", sc_get_menu_return},
    {0x1E9F, "GetAppName", sc_get_app_name},
    {0x1EA6, "EnableGetkeyToMainFunctionReturn", sc_enable_menu_return},
    {0x1EA7, "DisableGetkeyToMainFunctionReturn", sc_disable_menu_return},
    {0x1EF8, "SetBackGround", sc_nothing},
    {0x1F42, "sys_free", sc_free},
    {0x1F44, "sys_malloc", sc_malloc},
    {0x1F46, "sys_realloc", sc_realloc},
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
        sc_spend(emu, SYSCALL_CYCLES);
        cpu->r[0] = SYSCALLS[i].fn(emu, args);
        cpu->pc = cpu->pr;
        if (emu->mem.fault) {
            cpu_report(cpu, "syscall %04X %s: bad address 0x%08X", number, SYSCALLS[i].name,
                    emu->mem.fault_addr);
            cpu->state = CPU_ERROR;
        }
        return;
    }
    cpu_report(cpu, "unimplemented syscall 0x%04X (r4=%08X r5=%08X r6=%08X r7=%08X), called from 0x%08X",
            number, args[0], args[1], args[2], args[3], cpu->pr);
    cpu->state = CPU_ERROR;
}

void syscalls_install(Emu *emu)
{
    emu->cpu.syscall = dispatch;
    emu->cpu.syscall_user = emu;
}
