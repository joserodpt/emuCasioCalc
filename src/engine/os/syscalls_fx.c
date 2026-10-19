/*
 * The fx-9860G OS (monochrome models), for add-ins built with Casio's SDK
 * (fxlib): the calls not shared with the fx-CG ones. Numbers and arguments
 * from Simon Lothar's documentation of the legacy fx syscalls
 * (https://media.taricorp.net/prizm/simon_chm/fx_legacy_syscalls.htm).
 *
 * The OS draws into its VRAM, 1 kB at emu->vram: 64 rows of 16 bytes, bit 7
 * leftmost, 1 = dark. Bdisp_PutDisp_DD (and GetKey) put it on the display
 * (t6k11_show); _DD functions draw on the display directly, _DDVRAM on both.
 * Text: PrintXY and locate/Print in 6x8 cells (21 x 8 characters) with
 * Spleen 5x8, PrintMini with the same font (Casio's is a smaller one).
 */
#include <string.h>

#include "display.h"
#include "font.h"
#include "keys.h"
#include "syscalls_impl.h"

#define ROW_BYTES (MONO_WIDTH / 8)
#define SAVED_DISPS 3 /* SaveDisp / RestoreDisp: pages 1..3 */

/*---------------------------------------------------------------------------
 * Pixels
 *-------------------------------------------------------------------------*/

enum { PX_CLEAR, PX_SET, PX_INVERT };

static void pixel(uint8_t *vram, int x, int y, int how)
{
    if (x < 0 || y < 0 || x >= MONO_WIDTH || y >= MONO_HEIGHT)
        return;
    uint8_t *b = &vram[y * ROW_BYTES + x / 8], bit = (uint8_t)(0x80 >> (x & 7));
    if (how == PX_SET)
        *b |= bit;
    else if (how == PX_CLEAR)
        *b &= (uint8_t)~bit;
    else
        *b ^= bit;
}

static bool get_pixel(const uint8_t *vram, int x, int y)
{
    if (x < 0 || y < 0 || x >= MONO_WIDTH || y >= MONO_HEIGHT)
        return false;
    return vram[y * ROW_BYTES + x / 8] & (0x80 >> (x & 7));
}

void fx_show_vram(Emu *emu)
{
    t6k11_show(&emu->mono, emu->vram);
}

/* A change made on the display alone (_DD): the VRAM is left as it was. */
static void on_display(Emu *emu, void (*draw)(uint8_t *vram, const uint32_t *a), const uint32_t *a)
{
    uint8_t dd[MONO_VRAM_SIZE];
    t6k11_read(&emu->mono, dd);
    draw(dd, a);
    t6k11_show(&emu->mono, dd);
}

static void line(uint8_t *vram, int x0, int y0, int x1, int y1, int how)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        pixel(vram, x0, y0, how);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void area(uint8_t *vram, int x0, int y0, int x1, int y1, int how)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            pixel(vram, x, y, how);
}

/*---------------------------------------------------------------------------
 * System
 *-------------------------------------------------------------------------*/

/* GlibAddinAplExecutionCheck(0, 1, 1): fxlib's start-up; nothing to check. */
SC(sc_fx_glib_execution_check)
{
    (void)emu; (void)a;
    return 0;
}

SC(sc_fx_glib_lib_info)
{
    (void)emu; (void)a;
    return 0;
}

/* GlibGetOSVersionInfo(char *major, char *minor, short *local, short
 * *revision): from the OS header, "02.05.2201" -> 2, 5, 0x2201 (with the
 * fx-9860GII's 0x2000 marking). Returns 1. */
SC(sc_fx_os_version)
{
    const char *v = emu->layout->os_version ? emu->layout->os_version : "02.05.2201";
    int major = (v[0] - '0') * 10 + (v[1] - '0'), minor = (v[3] - '0') * 10 + (v[4] - '0');
    unsigned local = (unsigned)((v[6] - '0') << 12 | (v[7] - '0') << 8 | (v[8] - '0') << 4 | (v[9] - '0'));
    if (a[0])
        mem_write8(&emu->mem, a[0], (uint8_t)major);
    if (a[1])
        mem_write8(&emu->mem, a[1], (uint8_t)minor);
    if (a[2])
        mem_write16(&emu->mem, a[2], (uint16_t)(local & 0xFF00));
    if (a[3])
        mem_write16(&emu->mem, a[3], (uint16_t)(local & 0x00FF));
    return 1;
}

/* Hmem_SetMMU(virtual, physical, pages): fxlib maps the add-in's static RAM
 * (0x08100000..) onto physical RAM, 1 kB pages. Mapped here, in whole 4 kB
 * pages. */
SC(sc_fx_set_mmu)
{
    const Layout *l = emu->layout;
    uint32_t va = a[0] & ~(PAGE_SIZE - 1), pa = a[1] & 0x1FFFFFFFu, size = a[2] * 1024;
    uint32_t start = pa & ~(PAGE_SIZE - 1);
    size = (size + (pa - start) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start < l->ram_phys || start + size > l->ram_phys + l->ram_size) {
        cpu_report(&emu->cpu, "Hmem_SetMMU(0x%08X, 0x%08X, %u): outside RAM", a[0], a[1], a[2]);
        emu->cpu.state = CPU_ERROR;
        return 0;
    }
    mem_map(&emu->mem, va, size, emu->ram + (start - l->ram_phys), true);
    return 0;
}

/*---------------------------------------------------------------------------
 * Display
 *-------------------------------------------------------------------------*/

SC(sc_fx_dd_clear)
{
    (void)a;
    uint8_t blank[MONO_VRAM_SIZE] = {0};
    t6k11_show(&emu->mono, blank);
    return 0;
}

/* Bdisp_GetDisp_DD(buffer): what the display shows, as a VRAM. */
SC(sc_fx_get_disp_dd)
{
    uint8_t dd[MONO_VRAM_SIZE];
    t6k11_read(&emu->mono, dd);
    for (int i = 0; i < MONO_VRAM_SIZE; i++)
        mem_write8(&emu->mem, a[0] + (uint32_t)i, dd[i]);
    return 0;
}

SC(sc_fx_putdisp_dd)
{
    (void)a;
    fx_show_vram(emu);
    return 0;
}

/* Bdisp_DrawLineVRAM / Bdisp_ClearLineVRAM(x1, y1, x2, y2) */
SC(sc_fx_draw_line_vram)
{
    line(emu->vram, (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], PX_SET);
    return 0;
}

SC(sc_fx_clear_line_vram)
{
    line(emu->vram, (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], PX_CLEAR);
    return 0;
}

SC(sc_fx_allclr_dd)
{
    return sc_fx_dd_clear(emu, a);
}

SC(sc_fx_allclr_vram)
{
    (void)a;
    memset(emu->vram, 0, MONO_VRAM_SIZE);
    return 0;
}

SC(sc_fx_allclr_ddvram)
{
    sc_fx_allclr_vram(emu, a);
    fx_show_vram(emu);
    return 0;
}

/* Bdisp_GetDisp_VRAM(buffer): a copy of the VRAM. */
SC(sc_fx_get_disp_vram)
{
    for (int i = 0; i < MONO_VRAM_SIZE; i++)
        mem_write8(&emu->mem, a[0] + (uint32_t)i, emu->vram[i]);
    return 0;
}

/* Bdisp_SetPoint_*(x, y, point): 1 dark, 0 light. */
static void set_point(uint8_t *vram, const uint32_t *a)
{
    pixel(vram, (int32_t)a[0], (int32_t)a[1], a[2] & 0xFF ? PX_SET : PX_CLEAR);
}

SC(sc_fx_setpoint_vram)
{
    set_point(emu->vram, a);
    return 0;
}

SC(sc_fx_setpoint_dd)
{
    on_display(emu, set_point, a);
    return 0;
}

SC(sc_fx_setpoint_ddvram)
{
    set_point(emu->vram, a);
    on_display(emu, set_point, a);
    return 0;
}

SC(sc_fx_getpoint_vram)
{
    return get_pixel(emu->vram, (int32_t)a[0], (int32_t)a[1]);
}

/* Bdisp_AreaClr_*(const DISPBOX *box): left, top, right, bottom (ints). */
static void area_clear(Emu *emu, uint8_t *vram, uint32_t box)
{
    int l = (int32_t)mem_read32(&emu->mem, box), t = (int32_t)mem_read32(&emu->mem, box + 4);
    int r = (int32_t)mem_read32(&emu->mem, box + 8), b = (int32_t)mem_read32(&emu->mem, box + 12);
    area(vram, l, t, r, b, PX_CLEAR);
}

SC(sc_fx_areaclr_vram)
{
    area_clear(emu, emu->vram, a[0]);
    return 0;
}

SC(sc_fx_areaclr_dd)
{
    uint8_t dd[MONO_VRAM_SIZE];
    t6k11_read(&emu->mono, dd);
    area_clear(emu, dd, a[0]);
    t6k11_show(&emu->mono, dd);
    return 0;
}

SC(sc_fx_areaclr_ddvram)
{
    sc_fx_areaclr_vram(emu, a);
    sc_fx_areaclr_dd(emu, a);
    return 0;
}

/* Bdisp_AreaReverseVRAM(x1, y1, x2, y2) */
SC(sc_fx_area_reverse_vram)
{
    area(emu->vram, (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], PX_INVERT);
    return 0;
}

/* SaveDisp / RestoreDisp(page 1..3): the VRAM in one of three slots. */
static uint8_t saved[SAVED_DISPS][MONO_VRAM_SIZE];

SC(sc_fx_save_disp)
{
    if (a[0] >= 1 && a[0] <= SAVED_DISPS)
        memcpy(saved[a[0] - 1], emu->vram, MONO_VRAM_SIZE);
    return 0;
}

SC(sc_fx_restore_disp)
{
    if (a[0] >= 1 && a[0] <= SAVED_DISPS)
        memcpy(emu->vram, saved[a[0] - 1], MONO_VRAM_SIZE);
    return 0;
}

/*---------------------------------------------------------------------------
 * Text
 *-------------------------------------------------------------------------*/

/* One character at (x, y) in a cell adv wide: dark on light, or the
 * reverse (invert); over: the cell's light pixels are drawn too. */
static void draw_char(uint8_t *vram, int x, int y, uint32_t cp, int adv, bool invert, bool over)
{
    const FontGlyph *g = font_glyph(&FONT_TINY, cp);
    if (!g)
        g = font_glyph(&FONT_TINY, '?');
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < adv; col++) {
            bool on = g && col < g->w && FONT_TINY.bits[g->offset + row * ((g->w + 7) / 8) + col / 8] & (0x80 >> (col & 7));
            if (invert)
                on = !on;
            if (on)
                pixel(vram, x + col, y + row, PX_SET);
            else if (over || invert)
                pixel(vram, x + col, y + row, PX_CLEAR);
        }
}

/* A string at (x, y); returns the x after it. */
static int draw_text(Emu *emu, int x, int y, uint32_t addr, int adv, bool invert, bool over)
{
    char text[256];
    sc_string(emu, addr, text, sizeof text);
    for (const char *p = text; *p;) {
        int len;
        uint32_t cp = text_decode(p, &len);
        draw_char(emu->vram, x, y, cp, adv, invert, over);
        x += adv;
        p += len > 0 ? len : 1;
    }
    return x;
}

/* PrintXY(x, y, string, type): at pixel (x, y), 6x8 cells; type 1 reversed. */
SC(sc_fx_print_xy)
{
    draw_text(emu, (int32_t)a[0], (int32_t)a[1], a[2], 6, a[3] & 1, true);
    return 0;
}

/* PrintMini(x, y, string, mode): MINI_OVER 0x10, MINI_OR 0x11, MINI_REV
 * 0x12, MINI_REVOR 0x13. Casio's font is 3x5 in cells about 4 wide; this
 * one is wider. */
SC(sc_fx_print_mini)
{
    int mode = (int)(a[3] & 3);
    draw_text(emu, (int32_t)a[0], (int32_t)a[1], a[2], 5, mode >= 2, mode == 0 || mode == 2);
    return 0;
}

/* locate (shared) + Print / PrintRev / PrintC / PrintRevC: at the cursor,
 * columns 1..21 and rows 1..8 of 6x8 cells; the cursor moves past. */
static uint32_t print_at_cursor(Emu *emu, uint32_t addr, bool invert, bool one)
{
    int x = (emu->cursor_x - 1) * 6, y = (emu->cursor_y - 1) * 8;
    if (one) {
        char c[2] = {(char)mem_read8(&emu->mem, addr), 0};
        draw_char(emu->vram, x, y, (uint8_t)c[0], 6, invert, true);
        emu->cursor_x++;
    } else {
        int end = draw_text(emu, x, y, addr, 6, invert, true);
        emu->cursor_x += (end - x) / 6;
    }
    return 0;
}

SC(sc_fx_print)
{
    return print_at_cursor(emu, a[0], false, false);
}

SC(sc_fx_print_rev)
{
    return print_at_cursor(emu, a[0], true, false);
}

SC(sc_fx_print_c)
{
    return print_at_cursor(emu, a[0], false, true);
}

SC(sc_fx_print_rev_c)
{
    return print_at_cursor(emu, a[0], true, true);
}

/*---------------------------------------------------------------------------
 * Keyboard
 *-------------------------------------------------------------------------*/

/* Keyboard_IsSpecialKeyDown(short *matrixcode): 1 if that key is held. The
 * matrix code is GetKeyWait's minus 1 in each byte (columns 6..1, rows 1..9;
 * AC/on 0x0000). */
SC(sc_fx_key_down_at)
{
    uint16_t m = mem_read16(&emu->mem, a[0]);
    return sc_key_held_at(emu, (m >> 8) + 1, (m & 0xFF) + 1);
}

/* Keyboard_IsAnyKeyDown(short *matrixcode): 1 with the key, if one is held. */
SC(sc_fx_any_key_down)
{
    int col, row;
    if (sc_key_held(emu, &col, &row) < 0)
        return 0;
    if (a[0])
        mem_write16(&emu->mem, a[0], (uint16_t)((col - 1) << 8 | (row - 1)));
    return 1;
}

/* Keyboard_KeyDown(): 1 if any key is held. */
SC(sc_fx_key_down)
{
    (void)a;
    int col, row;
    return sc_key_held(emu, &col, &row) >= 0;
}

/* 0x1032: the OS's keycode table, int table[12][7]: the unshifted GetKey
 * code of the key at each matrix position (row 0..11, column 0..6, as for
 * Keyboard_IsSpecialKeyDown), -1 where there is none. fxlib's IsKeyDown
 * looks keycodes up in it. Kept in the OS's part of ROM. */
#define KEYCODE_TABLE 0x00380000u

SC(sc_fx_keycode_table)
{
    (void)a;
    uint8_t *t = emu->rom + KEYCODE_TABLE;
    for (int i = 0; i < 12 * 7; i++)
        be_write(t + 4 * i, 0xFFFFFFFFu, 4);
    for (int k = 0; k < KEY_COUNT; k++) {
        int col, row;
        sc_os_matrix(k, &col, &row);
        if (row - 1 < 12 && col - 1 < 7)
            be_write(t + 4 * ((row - 1) * 7 + (col - 1)), (uint32_t)KEYS[k].getkey, 4);
    }
    return P1(ROM_PHYS + KEYCODE_TABLE);
}

/* PutKey(keycode, mode): the next GetKey returns keycode. */
SC(sc_fx_put_key)
{
    uint32_t args[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, a[0]};
    return sc_put_keycode(emu, args) == 0 ? 1 : 0;
}
