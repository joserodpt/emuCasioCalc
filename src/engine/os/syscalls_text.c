/*
 * HLE syscalls: VRAM drawing and text.
 *
 * The OS fonts are approximated with an 8x8 font (see display.c), and
 * multi-byte characters show as '?'. Where WikiPrizm doesn't pin down a
 * detail (message box geometry, PrintCXY's origin), the choice is noted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "syscalls_impl.h"

/* Fake addresses for GetMiniGlyphPtr / GetFKeyPtr: unused ROM, reads as 0. */
#define MINI_GLYPH_BASE 0x81F00000u
#define FKEY_BITMAP     0x81F80000u

/* Reads an OS (multi-byte) string; 2-byte characters become '?'. */
static void read_text(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    char raw[256];
    sc_string(emu, addr, raw, sizeof raw);
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)raw; *s && n + 1 < cap; s++) {
        bool lead = *s == 0x7F || *s == 0xE5 || *s == 0xE6 || *s == 0xE7 || *s == 0xF7 || *s == 0xF9;
        out[n++] = lead || *s >= 0x80 ? '?' : (char)*s;
        if (lead && s[1])
            s++;
    }
    out[n] = '\0';
}

SC(sc_get_vram_address)
{
    (void)emu; (void)a;
    return emu->layout->vram;
}

/* The OS fills VRAM and sends it to the display with the DMA, and waits
 * for it: same cost as those DMA transfers. */
SC(sc_bdisp_allclr_vram)
{
    (void)a;
    display_fill(emu->vram, 0xFFFF);
    sc_spend(emu, VRAM_SIZE * DMA_RAM_CYCLES_PER_BYTE);
    return 0;
}

SC(sc_bdisp_putdisp_dd)
{
    (void)a;
    r61524_blit_vram(&emu->lcd, emu->vram);
    sc_spend(emu, VRAM_SIZE * DMA_LCD_CYCLES_PER_BYTE);
    return 0;
}

SC(sc_bdisp_setpoint_vram)
{
    display_set_pixel(emu->vram, (int32_t)a[0], (int32_t)a[1], (uint16_t)a[2]);
    return 0;
}

/* SaveVRAM_1() / LoadVRAM_1(): copy VRAM to the secondary VRAM and back
 * (GetSecondaryVRAMAddress gives add-ins its address). */
SC(sc_save_vram_1)
{
    (void)a;
    memcpy(emu->vram2, emu->vram, VRAM_SIZE);
    return 0;
}

SC(sc_load_vram_1)
{
    (void)a;
    memcpy(emu->vram, emu->vram2, VRAM_SIZE);
    return 0;
}

SC(sc_get_secondary_vram_address)
{
    (void)a;
    return emu->layout->vram2;
}

/* PrintXY(int x, int y, const char *string, int mode, int color).
 * The OS skips the first two bytes of the string whatever they are (they
 * select a font page), which is why add-ins write PrintXY(1, 1, "  Hello",
 * 0, 0), or pass a pointer 2 bytes before their text. */
SC(sc_print_xy)
{
    char text[128];
    read_text(emu, a[2] + 2, text, sizeof text);
    display_print_xy(emu->vram, (int32_t)a[0], (int32_t)a[1], text, (int32_t)a[3], (int32_t)a[4]);
    return 0;
}

/* locate_OS(x, y): text cursor for PrintLine, in PrintXY cells (1-based). */
SC(sc_locate)
{
    emu->cursor_x = (int32_t)a[0];
    emu->cursor_y = (int32_t)a[1];
    return 0;
}

/* Print_OS(msg, mode, 0): the whole string at the cursor (no 2-byte skip,
 * unlike PrintXY), inverted if mode is nonzero; the cursor moves past it. */
SC(sc_print_os)
{
    char text[128];
    read_text(emu, a[0], text, sizeof text);
    display_print_xy(emu->vram, emu->cursor_x, emu->cursor_y, text,
                     a[1] ? TEXT_MODE_INVERT : TEXT_MODE_NORMAL, 0);
    emu->cursor_x += (int)strlen(text);
    return 0;
}

/* PrintLine(msg, imax): the message at the cursor, at most imax characters,
 * the rest of the line up to imax cleared; black on white. */
SC(sc_print_line)
{
    char text[128];
    read_text(emu, a[0], text, sizeof text);
    int imax = (int32_t)a[1];
    if (imax < 0 || imax >= (int)sizeof text)
        imax = (int)sizeof text - 1;
    char line[128];
    snprintf(line, sizeof line, "%-*.*s", imax, imax, text);
    display_print_xy(emu->vram, emu->cursor_x, emu->cursor_y, line, TEXT_MODE_NORMAL, 0);
    return 0;
}

/* PrintCXY(x, y, string, mode, P5, color, back_color, P8, P9): big font at
 * pixel coordinates. Assumed relative to the area under the status bar
 * like PrintMini (WikiPrizm doesn't say). Mode: 0x01 invert, 0x20 no background. */
SC(sc_print_cxy)
{
    char text[128];
    read_text(emu, a[2], text, sizeof text);
    uint16_t fg = (uint16_t)a[5], bg = (uint16_t)a[6];
    if (a[3] & TEXT_MODE_INVERT) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    display_text_big(emu->vram, (int32_t)a[0], (int32_t)a[1] + STATUS_HEIGHT, text, fg, bg,
                     !(a[3] & TEXT_MODE_TRANSPARENT_BACKGROUND));
    return 0;
}

/* PrintMini(int *x, int *y, string, mode_flags, xlimit, P6, P7, color,
 * back_color, writeflag, P11): mini font, y under the status bar unless
 * mode 0x40; mode 0x04 swaps the colours, 0x02 leaves the background.
 * Advances *x; with writeflag 0 it only measures. */
SC(sc_print_mini)
{
    Memory *mem = &emu->mem;
    char text[256];
    read_text(emu, a[2], text, sizeof text);
    int x = (int32_t)mem_read32(mem, a[0]), y = (int32_t)mem_read32(mem, a[1]);
    uint32_t mode = a[3];
    uint16_t fg = (uint16_t)a[7], bg = (uint16_t)a[8];
    if (mode & 0x04) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    int xlimit = a[4] > LCD_WIDTH ? LCD_WIDTH : (int)a[4];
    int py = y + ((mode & 0x40) ? 0 : STATUS_HEIGHT);
    x = display_text_mini(emu->vram, x, py, text, fg, bg, !(mode & 0x02), xlimit, a[9] != 0);
    mem_write32(mem, a[0], (uint32_t)x);
    return 0;
}

/* PrintMiniMini(int *x, int *y, string, mode, color, P6): the 10 px font
 * (approximated by the 8x8 one), y under the status bar unless mode bit 6.
 * Mode bits (WikiPrizm): 0 white text on `color`, 2 black background, 4
 * bold (not shown), 7 leaves the background (our reading of "whether the
 * background is cleared"). Advances *x. */
SC(sc_print_mini_mini)
{
    Memory *mem = &emu->mem;
    char text[256];
    read_text(emu, a[2], text, sizeof text);
    int x = (int32_t)mem_read32(mem, a[0]), y = (int32_t)mem_read32(mem, a[1]);
    uint32_t mode = a[3];
    uint16_t fg = display_text_color((int)(a[4] & 7)), bg = 0xFFFF;
    if (mode & 0x01) {
        bg = fg;
        fg = 0xFFFF;
    }
    if (mode & 0x04)
        bg = 0x0000;
    int py = y + ((mode & 0x40) ? 0 : STATUS_HEIGHT);
    int w = (int)strlen(text) * SMALL_CELL_W;
    if (!(mode & 0x80))
        display_rect(emu->vram, x, py, x + w - 1, py + SMALL_CELL_H - 1, bg);
    display_text_small(emu->vram, x, py, text, fg);
    mem_write32(mem, a[0], (uint32_t)(x + w));
    return 0;
}

/* GetMiniGlyphPtr(glyph_no, unsigned short *glyph_info): we hand out a fake
 * pointer that encodes the character, for PrintMiniGlyph to decode, and
 * report its width in glyph_info. */
SC(sc_get_mini_glyph_ptr)
{
    uint16_t glyph = (uint16_t)a[0];
    if (a[1])
        mem_write16(&emu->mem, a[1], MINI_CELL_W);
    return MINI_GLYPH_BASE + (glyph < 0x80 ? glyph : '?') * 4u;
}

/* PrintMiniGlyph(x, y, glyph, mode_flags, glyph_width, P6..P9, color,
 * back_color, P12): one mini-font character (mode bits as PrintMini). */
SC(sc_print_mini_glyph)
{
    uint32_t mode = a[3];
    char text[2] = {'?', 0};
    if (a[2] >= MINI_GLYPH_BASE && a[2] < MINI_GLYPH_BASE + 0x400) {
        int c = (int)(a[2] - MINI_GLYPH_BASE) / 4;
        if (c >= 0x20 && c < 0x7F)
            text[0] = (char)c;
    }
    uint16_t fg = (uint16_t)a[9], bg = (uint16_t)a[10];
    if (mode & 0x04) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    int py = (int32_t)a[1] + ((mode & 0x40) ? 0 : STATUS_HEIGHT);
    display_text_mini(emu->vram, (int32_t)a[0], py, text, fg, bg, !(mode & 0x02), LCD_WIDTH, true);
    return 0;
}

/* DrawFrame(color): colour of the screen border, drawn directly. */
SC(sc_draw_frame)
{
    r61524_set_border(&emu->lcd, (uint16_t)a[0]);
    return 0;
}

/* GetFKeyPtr(id, void **bitmap): the function key label bitmaps are in the
 * OS; we return a blank one. */
SC(sc_get_fkey_ptr)
{
    if (a[1])
        mem_write32(&emu->mem, a[1], FKEY_BITMAP);
    return 0;
}

/* FKey_Display(n, bitmap): an F-key label over F1..F6 (n = 0..5), in the
 * last 24 rows. The bitmaps' format isn't documented (and GetFKeyPtr gives
 * blank ones): we draw an empty label tab. */
SC(sc_fkey_display)
{
    int n = (int32_t)a[0];
    if (n < 0 || n > 5)
        return 0;
    int x0 = n * 64, y0 = LCD_HEIGHT - 24;
    display_rect(emu->vram, x0, y0, x0 + 63, LCD_HEIGHT - 1, 0xFFFF);
    display_rect(emu->vram, x0 + 1, y0 + 1, x0 + 62, y0 + 1, 0x0000);
    display_rect(emu->vram, x0 + 1, LCD_HEIGHT - 2, x0 + 62, LCD_HEIGHT - 2, 0x0000);
    display_rect(emu->vram, x0 + 1, y0 + 1, x0 + 1, LCD_HEIGHT - 2, 0x0000);
    display_rect(emu->vram, x0 + 62, y0 + 1, x0 + 62, LCD_HEIGHT - 2, 0x0000);
    return 0;
}

/* Message box geometry is our guess: a box of `lines` lines frames PrintXY
 * rows 2..lines+1, columns 2..19, which is where add-ins print their
 * message text. */
BoxRect msgbox_rect(int lines)
{
    if (lines < 1) lines = 1;
    if (lines > 6) lines = 6;
    return (BoxRect){BIG_CELL_W - 6, 2 * BIG_CELL_H - 6, 20 * BIG_CELL_W + 5, (2 + lines) * BIG_CELL_H + 5};
}

void msgbox_open(Emu *emu, int lines)
{
    if (emu->msgbox_depth < (int)(sizeof emu->msgbox / sizeof emu->msgbox[0])) {
        uint8_t *save = malloc(VRAM_SIZE);
        if (save)
            memcpy(save, emu->vram, VRAM_SIZE);
        emu->msgbox[emu->msgbox_depth++] = save;
    }
    BoxRect r = msgbox_rect(lines);
    display_rect(emu->vram, r.x0, r.y0, r.x1, r.y1, 0x001F);
    display_rect(emu->vram, r.x0 + 2, r.y0 + 2, r.x1 - 2, r.y1 - 2, 0xFFFF);
}

void msgbox_close(Emu *emu)
{
    if (emu->msgbox_depth <= 0)
        return;
    uint8_t *save = emu->msgbox[--emu->msgbox_depth];
    if (save)
        memcpy(emu->vram, save, VRAM_SIZE);
    free(save);
}

/* MsgBoxPush(lines): saves VRAM and draws an empty box with a blue border. */
SC(sc_msgbox_push)
{
    msgbox_open(emu, (int32_t)a[0]);
    return 0;
}

SC(sc_msgbox_pop)
{
    (void)a;
    msgbox_close(emu);
    return 0;
}

/* DisplayMBString2(select, string, start, xpos, sel_start, x, y,
 * pos_to_clear, right_margin, key): shows the line being edited from
 * character `start`, at pixel (x, y) under the status bar, clearing
 * pos_to_clear cells, with the cursor at xpos drawn inverted. */
SC(sc_display_mbstring2)
{
    char text[256];
    read_text(emu, a[1], text, sizeof text);
    int start = (int32_t)a[2], xpos = (int32_t)a[3];
    int x = (int32_t)a[5], y = (int32_t)a[6] + STATUS_HEIGHT, cells = (int32_t)a[7];
    if (cells <= 0 || cells > 21)
        cells = 21 - x / BIG_CELL_W;
    int len = (int)strlen(text);
    if (start < 0 || start > len)
        start = 0;

    display_rect(emu->vram, x, y, x + cells * BIG_CELL_W - 1, y + BIG_CELL_H - 1, 0xFFFF);
    char visible[64];
    snprintf(visible, sizeof visible, "%.*s", cells, text + start);
    display_text_big(emu->vram, x, y, visible, 0x0000, 0xFFFF, true);
    int cursor = xpos - start;
    if (cursor >= 0 && cursor < cells) {
        char c[2] = {xpos < len ? text[xpos] : ' ', 0};
        display_text_big(emu->vram, x + cursor * BIG_CELL_W, y, c, 0xFFFF, 0x0000, true);
    }
    return 0;
}
