/*
 * HLE syscalls: VRAM drawing and text.
 *
 * Text is drawn with open fonts close to the OS's sizes (font.h), from
 * strings in the OS character set (display.h decodes them). Where WikiPrizm
 * doesn't pin down a detail (message box geometry, PrintCXY's origin), the
 * choice is noted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "font.h"
#include "syscalls_impl.h"

/* Fake addresses for GetMiniGlyphPtr / GetFKeyPtr: unused ROM, reads as 0.
 * A glyph pointer encodes the character code: base + 4 * code. */
#define MINI_GLYPH_BASE 0x81F00000u
#define MINI_GLYPH_END  (MINI_GLYPH_BASE + 4 * 0x10000u)
#define FKEY_BITMAP     0x81F80000u

/* Reads an OS string (kept in the OS character set). */
static void read_text(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    sc_string(emu, addr, out, cap);
}

/* A character code (1 or 2 bytes) as a string. */
static void code_text(uint32_t code, char out[3])
{
    if (code > 0xFF) {
        out[0] = (char)(code >> 8);
        out[1] = (char)code;
        out[2] = '\0';
    } else {
        out[0] = (char)code;
        out[1] = '\0';
    }
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
    /* The two bytes go to ProcessPrintChars: 03 A8 selects GB 18030 for
     * this string; the setting is back as it was afterwards. */
    uint16_t charset = (uint16_t)(mem_read8(&emu->mem, a[2]) << 8 | mem_read8(&emu->mem, a[2] + 1));
    bool before = display_gb18030();
    if (charset == 0x03A8)
        display_set_gb18030(true);
    else if (charset == 0x0000)
        display_set_gb18030(false);
    display_print_xy(emu->vram, (int32_t)a[0], (int32_t)a[1], text, (int32_t)a[3], (int32_t)a[4]);
    display_set_gb18030(before);
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
    emu->cursor_x += text_length(text);
    return 0;
}

/* At the cursor: at most imax characters of text, the cells after it up to
 * imax cleared (PrintLine, PrintLine2). */
static void print_padded(Emu *emu, const char *text, int imax, uint16_t fg, uint16_t bg)
{
    char shown[128];
    size_t n = 0;
    int chars = 0;
    for (int len; chars < imax && (len = text_char_len(text + n)) && n + (size_t)len < sizeof shown; chars++)
        n += (size_t)len;
    memcpy(shown, text, n);
    for (; chars < imax && n + 1 < sizeof shown; chars++)
        shown[n++] = ' ';
    shown[n] = '\0';
    int x = (emu->cursor_x - 1) * BIG_CELL_W, y = emu->cursor_y * BIG_CELL_H;
    display_text_big(emu->vram, x, y, shown, fg, bg, true);
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
    print_padded(emu, text, imax, 0x0000, 0xFFFF);
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

/* PrintMiniMini(int *x, int *y, string, mode, color, P6): the 10 px font,
 * y under the status bar unless mode bit 6. Mode bits (WikiPrizm): 0 white
 * text on `color`, 2 black background, 4 bold, 7 leaves the background
 * (our reading of "whether the background is cleared"). Advances *x. */
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
    x = display_text_prop(emu->vram, &FONT_TINY, x, py, SMALL_CELL_H, 1, text, fg, bg, !(mode & 0x80),
                          (mode & 0x10) != 0, LCD_WIDTH, true);
    mem_write32(mem, a[0], (uint32_t)x);
    return 0;
}

/* GetMiniGlyphPtr(glyph_no, unsigned short *glyph_info): we hand out a fake
 * pointer that encodes the character, for PrintMiniGlyph to decode, and
 * report its width in glyph_info. */
SC(sc_get_mini_glyph_ptr)
{
    uint16_t glyph = (uint16_t)a[0];
    char text[3];
    code_text(glyph, text);
    if (a[1])
        mem_write16(&emu->mem, a[1], (uint16_t)display_text_mini(emu->vram, 0, 0, text, 0, 0, false, 1 << 20, false));
    return MINI_GLYPH_BASE + glyph * 4u;
}

/* PrintMiniGlyph(x, y, glyph, mode_flags, glyph_width, P6..P9, color,
 * back_color, P12): one mini-font character (mode bits as PrintMini), from
 * a GetMiniGlyphPtr pointer; other glyphs (the add-in's own bitmaps, whose
 * format isn't documented) leave a blank cell. */
SC(sc_print_mini_glyph)
{
    uint32_t mode = a[3];
    uint16_t fg = (uint16_t)a[9], bg = (uint16_t)a[10];
    if (mode & 0x04) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    int x = (int32_t)a[0], py = (int32_t)a[1] + ((mode & 0x40) ? 0 : STATUS_HEIGHT);
    if (a[2] >= MINI_GLYPH_BASE && a[2] < MINI_GLYPH_END) {
        char text[3];
        code_text((a[2] - MINI_GLYPH_BASE) / 4, text);
        display_text_mini(emu->vram, x, py, text, fg, bg, !(mode & 0x02), LCD_WIDTH, true);
    } else if (!(mode & 0x02)) {
        /* the add-in's own glyph, in a format we don't know: its cell,
         * blank (glyph_width wide) */
        int w = (int32_t)a[4] > 0 && (int32_t)a[4] < 64 ? (int32_t)a[4] : MINI_CELL_W;
        display_rect(emu->vram, x, py, x + w - 1, py + MINI_CELL_H - 1, bg);
    }
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

/* An edited line: the text from byte `start` in `cells` big-font cells at
 * pixel (px, py), the rest cleared, and the cursor (byte `cursor`) as a
 * bar at the left of its cell. */
static void edit_line(Emu *emu, const char *text, int start, int cursor, int px, int py, int cells)
{
    int len = (int)strlen(text);
    if (start < 0 || start > len)
        start = 0;
    display_rect(emu->vram, px, py, px + cells * BIG_CELL_W - 1, py + BIG_CELL_H - 1, 0xFFFF);
    char visible[128];
    size_t n = 0;
    int shown = 0, cursor_cell = -1;
    for (int i = start, l; shown <= cells; i += l, shown++) {
        if (i == cursor)
            cursor_cell = shown;
        l = text_char_len(text + i);
        if (!l || shown == cells || n + (size_t)l >= sizeof visible)
            break;
        memcpy(visible + n, text + i, (size_t)l);
        n += (size_t)l;
    }
    visible[n] = '\0';
    display_text_big(emu->vram, px, py, visible, 0x0000, 0xFFFF, true);
    if (cursor_cell >= 0 && cursor_cell <= cells) {
        int cx = px + cursor_cell * BIG_CELL_W;
        display_rect(emu->vram, cx, py + 2, cx + 1, py + BIG_CELL_H - 3, 0x0000);
    }
}

/* DisplayMBString2(select, string, start, xpos, sel_start, x, y,
 * pos_to_clear, right_margin, key): the line being edited from byte
 * `start`, at pixel (x, y) under the status bar, over pos_to_clear cells,
 * the cursor at byte xpos. */
SC(sc_display_mbstring2)
{
    char text[256];
    read_text(emu, a[1], text, sizeof text);
    int x = (int32_t)a[5], cells = (int32_t)a[7];
    if (cells <= 0 || cells > 21)
        cells = 21 - x / BIG_CELL_W;
    edit_line(emu, text, (int32_t)a[2], (int32_t)a[3], x, (int32_t)a[6] + STATUS_HEIGHT, cells);
    return 0;
}

/* DisplayMBString(string, start, cursor, x, y): the same at PrintXY's
 * homescreen position (x 1-21, y row), up to the end of the line. */
SC(sc_display_mbstring)
{
    char text[256];
    read_text(emu, a[0], text, sizeof text);
    int x = (int32_t)a[3], y = (int32_t)a[4];
    if (x < 1 || x > 21)
        x = 1;
    edit_line(emu, text, (int32_t)a[1], (int32_t)a[2], (x - 1) * BIG_CELL_W, y * BIG_CELL_H, 22 - x);
    return 0;
}

/* Bdisp_MMPrint(x, y, string, mode, xlimit, P6, P7, color, back_color,
 * writeflag, P11) and Bdisp_MMPrintRef(int *x, int *y, ...): the 16 px
 * font (as PrintMini's, in 16 px lines). y under the status bar unless mode
 * 0x40; mode 1 swaps the colours, 2 leaves the background, 4 inverts them.
 * The Ref version advances *x (writeflag 0: only measures). */
static int mm_print(Emu *emu, int x, int y, const uint32_t *a)
{
    char text[256];
    read_text(emu, a[2], text, sizeof text);
    uint32_t mode = a[3];
    uint16_t fg = (uint16_t)a[7], bg = (uint16_t)a[8];
    if (mode & 0x01) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    if (mode & 0x04) {
        fg = (uint16_t)~fg;
        bg = (uint16_t)~bg;
    }
    int xlimit = (int32_t)a[4] < 0 || a[4] > LCD_WIDTH ? LCD_WIDTH : (int)a[4];
    int py = y + ((mode & 0x40) ? 0 : STATUS_HEIGHT);
    return display_text_prop(emu->vram, &FONT_MINI, x, py, FONT_MINI.h, 0, text, fg, bg, !(mode & 0x02), false,
                             xlimit, a[9] != 0);
}

SC(sc_mmprint)
{
    mm_print(emu, (int32_t)a[0], (int32_t)a[1], a);
    return 0;
}

SC(sc_mmprint_ref)
{
    Memory *mem = &emu->mem;
    int x = mm_print(emu, (int32_t)mem_read32(mem, a[0]), (int32_t)mem_read32(mem, a[1]), a);
    mem_write32(mem, a[0], (uint32_t)x);
    return 0;
}

/* PrintGlyph(x, y, glyph, mode, color, back_color, P7): one big-font
 * character at pixel (x, y), y under the status bar unless mode 0x40;
 * mode as PrintXY (0x01 inverse, 0x20 no background). The glyph format
 * isn't documented: the pointers GetMiniGlyphPtr gives are understood,
 * others draw U+FFFD. */
SC(sc_print_glyph)
{
    uint32_t mode = a[3];
    char text[3] = "\xFF";
    if (a[2] >= MINI_GLYPH_BASE && a[2] < MINI_GLYPH_END)
        code_text((a[2] - MINI_GLYPH_BASE) / 4, text);
    uint16_t fg = (uint16_t)a[4], bg = (uint16_t)a[5];
    if (mode & TEXT_MODE_INVERT) {
        uint16_t t = fg;
        fg = bg;
        bg = t;
    }
    int py = (int32_t)a[1] + ((mode & 0x40) ? 0 : STATUS_HEIGHT);
    display_text_big(emu->vram, (int32_t)a[0], py, text, fg, bg, !(mode & TEXT_MODE_TRANSPARENT_BACKGROUND));
    return 0;
}

/* PrintLine2(P1, mode, msg, P4, P5, imax, color1, color2): PrintLine with
 * colours (PrintXY indices). Mode bit 0: the whole line up to imax (else
 * only the message); bits 7 and 5: color2 on color1, else color1 on
 * color2. imax is zero-based. */
SC(sc_print_line2)
{
    char text[128];
    read_text(emu, a[2], text, sizeof text);
    uint32_t mode = a[1];
    int imax = (int32_t)a[5] + 1;
    if (imax < 0 || imax > 64)
        imax = 64;
    uint16_t c1 = display_text_color((int)(a[6] & 7)), c2 = display_text_color((int)(a[7] & 7));
    bool swap = (mode & 0xA0) == 0xA0;
    if (!(mode & 1)) {
        int n = text_length(text);
        if (n < imax)
            imax = n;
    }
    print_padded(emu, text, imax, swap ? c2 : c1, swap ? c1 : c2);
    return 0;
}

/* ProcessPrintChars(code): the character set of the text that follows,
 * 0x0000 Latin, 0x03A8 GB 18030. Returns 1 if known. */
SC(sc_process_print_chars)
{
    uint16_t code = (uint16_t)a[0];
    if (code == 0x0000)
        display_set_gb18030(false);
    else if (code == 0x03A8)
        display_set_gb18030(true);
    else
        return 0;
    (void)emu;
    return 1;
}

SC(sc_enable_gb18030)
{
    (void)emu; (void)a;
    display_set_gb18030(true);
    return 0;
}

SC(sc_disable_gb18030)
{
    (void)emu; (void)a;
    display_set_gb18030(false);
    return 0;
}

/* MB_IsLead(c): 1 if c starts a 2-byte character. */
SC(sc_mb_is_lead)
{
    (void)emu;
    return text_is_lead((uint8_t)a[0]);
}

/* MB_ElementCount(string): characters as printed, 2-byte ones counting one. */
SC(sc_mb_element_count)
{
    char text[512];
    read_text(emu, a[0], text, sizeof text);
    return (uint32_t)text_length(text);
}
