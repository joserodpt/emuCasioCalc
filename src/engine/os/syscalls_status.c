/*
 * HLE syscalls: the status area, the top 24 rows of VRAM.
 *
 * From WikiPrizm: EnableStatusArea (0/2 on, 3 off), DefineStatusAreaFlags'
 * modes and SAF_* flags, DefineStatusMessage's text and colour,
 * DefineStatusGlyph's structure and alignments, DisplayStatusArea drawing
 * it unless disabled (GetKey does too), EnableDisplayHeader,
 * Bdisp_HeaderText, BatteryIcon. Our reading where it is silent: what the
 * area looks like (layout, sizes, the battery's 3 bars), the header
 * colours as a gradient from the first to the second, and the setup icons,
 * which show angle mode from the setup and fixed defaults for the rest.
 */
#include <string.h>

#include "display.h"
#include "platform.h"
#include "syscalls_impl.h"

#define SAF_BATTERY      0x0001
#define SAF_ALPHA_SHIFT  0x0002
#define SAF_INPUT_OUTPUT 0x0004
#define SAF_FRAC_RESULT  0x0008
#define SAF_ANGLE        0x0010
#define SAF_COMPLEX_MODE 0x0020
#define SAF_DISPLAY      0x0040
#define SAF_TEXT         0x0100
#define SAF_GLYPH        0x0200

#define SETUP_ANGLE 0x13 /* 0 Deg, 1 Rad, 2 Gra */

#define HEADER_H 24
#define TEXT_GREY 0x52AA

static void header_fill(Emu *emu)
{
    uint16_t c1 = display_text_color(emu->status_color[0]);
    uint16_t c2 = display_text_color(emu->status_color[1]);
    for (int y = 0; y < HEADER_H - 2; y++) {
        int t = y * 32 / (HEADER_H - 3);
        int r = ((c1 >> 11) * (32 - t) + (c2 >> 11) * t) / 32;
        int g = ((c1 >> 5 & 63) * (32 - t) + (c2 >> 5 & 63) * t) / 32;
        int b = ((c1 & 31) * (32 - t) + (c2 & 31) * t) / 32;
        display_rect(emu->vram, 0, y, LCD_WIDTH - 1, y, (uint16_t)(r << 11 | g << 5 | b));
    }
    display_rect(emu->vram, 0, HEADER_H - 2, LCD_WIDTH - 1, HEADER_H - 1, c2);
}

/* Battery: outline, terminal, 3 bars (red when one or none is left). */
static int draw_battery(Emu *emu, int x)
{
    uint8_t *v = emu->vram;
    int y = 6, w = 22, h = 12;
    display_rect(v, x, y, x + w - 1, y, 0x0000);
    display_rect(v, x, y + h - 1, x + w - 1, y + h - 1, 0x0000);
    display_rect(v, x, y, x, y + h - 1, 0x0000);
    display_rect(v, x + w - 1, y, x + w - 1, y + h - 1, 0x0000);
    display_rect(v, x + w, y + 3, x + w + 1, y + h - 4, 0x0000);
    int bars = battery_bars(emu->battery_cv);
    uint16_t c = bars <= 1 ? 0xF800 : 0x0560;
    for (int i = 0; i < bars; i++)
        display_rect(v, x + 2 + 6 * i, y + 2, x + 6 + 6 * i, y + h - 3, c);
    return x + w + 6;
}

/* SHIFT: "S" on yellow; ALPHA: "A" on red, underlined when locked. */
static int draw_modifier(Emu *emu, int x)
{
    if (!emu->key_shift && emu->key_alpha == ALPHA_OFF)
        return x;
    uint8_t *v = emu->vram;
    bool shift = emu->key_shift;
    display_rect(v, x, 4, x + 15, 19, shift ? 0xFE00 : 0xD000);
    display_text_small(v, x + 4, 8, shift ? "S" : "A", shift ? 0x0000 : 0xFFFF);
    if (!shift && emu->key_alpha == ALPHA_LOCK)
        display_rect(v, x + 3, 17, x + 12, 17, 0xFFFF);
    return x + 22;
}

static int draw_label(Emu *emu, int x, const char *s)
{
    display_text_small(emu->vram, x, 8, s, TEXT_GREY);
    return x + 8 * (int)strlen(s) + 8;
}

static void draw_glyphs(Emu *emu, int x)
{
    Memory *mem = &emu->mem;
    for (int i = 0; i < 4; i++) {
        const StatusGlyph *g = &emu->status_glyph[i];
        if (!g->bitmap || g->dx <= 0 || g->dy <= 0 || g->dx > LCD_WIDTH || g->dy > HEADER_H)
            continue;
        int gx = g->xalign == 2 ? LCD_WIDTH - g->dx - g->xoffset : g->xalign == 3 ? g->xoffset : x + g->xoffset;
        for (int y = 0; y < g->dy; y++)
            for (int col = 0; col < g->dx; col++)
                display_set_pixel(emu->vram, gx + col, y + (HEADER_H - g->dy) / 2,
                                  mem_read16(mem, g->bitmap + (uint32_t)(y * g->dx + col) * 2));
        if (g->xalign == 1)
            x = gx + g->dx + 4;
    }
}

static void draw_message(Emu *emu, int x)
{
    display_text_mini(emu->vram, x, 3, emu->status_msg, display_text_color(emu->status_msg_color), 0,
                      false, LCD_WIDTH, true);
}

void status_area_draw(Emu *emu)
{
    if (emu->status_off)
        return;
    uint32_t f = emu->status_flags;
    header_fill(emu);
    int x = 3;
    if (f & SAF_BATTERY)
        x = draw_battery(emu, x);
    if (f & SAF_ALPHA_SHIFT)
        x = draw_modifier(emu, x);
    if (f & SAF_INPUT_OUTPUT)
        x = draw_label(emu, x, "Math");
    if (f & SAF_FRAC_RESULT)
        x = draw_label(emu, x, "d/c");
    if (f & SAF_ANGLE) {
        static const char *const ANGLE[] = {"Deg", "Rad", "Gra"};
        uint8_t a = emu->setup[SETUP_ANGLE];
        x = draw_label(emu, x, ANGLE[a < 3 ? a : 0]);
    }
    if (f & SAF_COMPLEX_MODE)
        x = draw_label(emu, x, "Real");
    if (f & SAF_DISPLAY)
        x = draw_label(emu, x, "Norm1");
    if (f & SAF_TEXT)
        draw_message(emu, x);
    if (f & SAF_GLYPH)
        draw_glyphs(emu, x);
}

/* EnableStatusArea(opt): 0 and 2 enable, 3 disables, 1 nothing. */
SC(sc_enable_status_area)
{
    if (a[0] == 0 || a[0] == 2)
        emu->status_off = false;
    else if (a[0] == 3)
        emu->status_off = true;
    return 0;
}

/* DefineStatusAreaFlags(mode, flags, char *color1, char *color2). Modes 0
 * and 1 fill the header white and clear / set all flags (0x303); 2 reads
 * the colours; 3 sets the colours (non-null pointers) and the flags; 4 and
 * 5 add and remove flags. Returns the flags. */
SC(sc_define_status_area_flags)
{
    Memory *mem = &emu->mem;
    switch (a[0]) {
    case 0: case 1:
        emu->status_color[0] = emu->status_color[1] = 7;
        emu->status_flags = a[0] ? 0x0303 : 0;
        if (!emu->status_off)
            header_fill(emu);
        break;
    case 2:
        if (a[2]) mem_write8(mem, a[2], emu->status_color[0]);
        if (a[3]) mem_write8(mem, a[3], emu->status_color[1]);
        break;
    case 3:
        if (a[2]) emu->status_color[0] = mem_read8(mem, a[2]);
        if (a[3]) emu->status_color[1] = mem_read8(mem, a[3]);
        emu->status_flags = a[1];
        break;
    case 4: emu->status_flags |= a[1]; break;
    case 5: emu->status_flags &= ~a[1]; break;
    }
    return emu->status_flags;
}

/* DefineStatusMessage(msg, P2, colour, P4): P2 and P4 unknown. */
SC(sc_define_status_message)
{
    sc_string(emu, a[0], emu->status_msg, sizeof emu->status_msg);
    emu->status_msg_color = (uint8_t)(a[2] & 0xFF);
    platform_set_os_status(emu->status_msg);
    return 0;
}

/* DefineStatusGlyph(idx, struct glyph_def *): dx, dy (short), bitmap,
 * xalignment, xoffset (short). */
SC(sc_define_status_glyph)
{
    if (a[0] >= 4)
        return 0;
    Memory *mem = &emu->mem;
    StatusGlyph *g = &emu->status_glyph[a[0]];
    if (!a[1]) {
        memset(g, 0, sizeof *g);
        return 0;
    }
    g->dx = (int16_t)mem_read16(mem, a[1]);
    g->dy = (int16_t)mem_read16(mem, a[1] + 2);
    g->bitmap = mem_read32(mem, a[1] + 4);
    g->xalign = (int16_t)mem_read16(mem, a[1] + 8);
    g->xoffset = (int16_t)mem_read16(mem, a[1] + 10);
    return 0;
}

SC(sc_display_status_area)
{
    (void)a;
    status_area_draw(emu);
    return 0;
}

/* Bdisp_HeaderText(): only the message, after the icons' place. */
SC(sc_header_text)
{
    (void)a;
    if (!emu->status_off)
        draw_message(emu, 3);
    return 0;
}

/* Bdisp_HeaderFill(color_idx1, color_idx2) */
SC(sc_header_fill)
{
    emu->status_color[0] = (uint8_t)a[0];
    emu->status_color[1] = (uint8_t)a[1];
    header_fill(emu);
    return 0;
}

/* DrawHeaderLine(): the line under the status area. */
SC(sc_draw_header_line)
{
    (void)a;
    display_rect(emu->vram, 0, HEADER_H - 1, LCD_WIDTH - 1, HEADER_H - 1, 0x8410);
    return 0;
}

/* EnableDisplayHeader(action, value): 0 clears, 2 sets whether GetKey
 * draws the status area; returns it. */
SC(sc_enable_display_header)
{
    if (a[0] == 0)
        emu->getkey_status = 0;
    else if (a[0] == 2)
        emu->getkey_status = a[1];
    return emu->getkey_status;
}

/* BatteryIcon(show) */
SC(sc_battery_icon)
{
    if (a[0])
        emu->status_flags |= SAF_BATTERY;
    else
        emu->status_flags &= ~(uint32_t)SAF_BATTERY;
    return 0;
}
