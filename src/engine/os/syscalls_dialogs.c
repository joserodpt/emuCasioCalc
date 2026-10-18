/*
 * HLE syscalls: OS dialogs and UI pieces drawn in message boxes.
 *
 * Documented on WikiPrizm: DisplayMessageBox, ColorIndexDialog1,
 * ProgressBar2, the scrollbar structure (libfxcg's display.h). ProgressBar,
 * ProgressBar0, BoxInnerClear and BoxYLimits are our reading of their names
 * and parameters. Box geometry is the same guess as MsgBoxPush's.
 */
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "keys.h"
#include "syscalls_impl.h"

/* Reads an OS string (kept in the OS character set). */
static void read_line(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    sc_string(emu, addr, out, cap);
}

/* DisplayMessageBox(message): a message box with the text (split at
 * newlines and wrapped, up to 6 lines), until EXIT, AC/ON or the left or
 * right cursor keys are pressed. The caller adds any "Press:[EXIT]". */
SC(sc_display_message_box)
{
    char text[256], lines[6][64];
    read_line(emu, a[0], text, sizeof text);
    int n = 0;
    for (const char *s = text; *s && n < 6;) {
        /* up to 18 characters, or to the newline */
        int bytes = 0, chars = 0;
        for (int l; chars < 18 && s[bytes] != '\n' && (l = text_char_len(s + bytes)); chars++)
            bytes += l;
        snprintf(lines[n++], sizeof lines[0], "%.*s", bytes, s);
        s += bytes;
        if (*s == '\n')
            s++;
    }
    if (!n)
        lines[n++][0] = '\0';
    msgbox_open(emu, n);
    for (int i = 0; i < n; i++)
        display_print_xy(emu->vram, 2, 2 + i, lines[i], TEXT_MODE_NORMAL, 0);
    for (;;) {
        int key = sc_wait_key(emu);
        if (key < 0 || key == KEY_CTRL_EXIT || key == KEY_CTRL_AC || key == KEY_CTRL_LEFT ||
            key == KEY_CTRL_RIGHT)
            break;
    }
    msgbox_close(emu);
    return 0;
}

/* ColorIndexDialog1(initial_index, disable_mask): pick one of the 8 PrintXY
 * colours. Bits 0x01..0x80 of the mask disable colours 1..8. Returns 0-7,
 * or 0xFF on EXIT / AC/ON. Up/down and EXE, or the digit keys 1-8. */
SC(sc_color_index_dialog)
{
    static const char *NAMES[8] = {"Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Yellow", "White"};
    uint32_t mask = a[1];
    int sel = (int)(a[0] & 7);
    for (int tries = 0; tries < 8 && (mask >> sel & 1); tries++)
        sel = (sel + 1) % 8;

    msgbox_open(emu, 6);
    BoxRect r = msgbox_rect(6);
    uint32_t ret = 0xFF;
    for (;;) {
        display_rect(emu->vram, r.x0 + 2, r.y0 + 2, r.x1 - 2, r.y1 - 2, 0xFFFF);
        display_print_xy(emu->vram, 2, 2, "Color", TEXT_MODE_NORMAL, 0);
        /* two columns of four: "1:Black" ... */
        for (int i = 0; i < 8; i++) {
            char item[16];
            snprintf(item, sizeof item, "%d:%s", i + 1, NAMES[i]);
            int x = 2 + (i / 4) * 9, y = 3 + i % 4;
            bool off = mask >> i & 1;
            display_print_xy(emu->vram, x, y, item, i == sel ? TEXT_MODE_INVERT : TEXT_MODE_NORMAL,
                             off ? 0 : (i == 7 ? 0 : i));
        }
        int key = sc_wait_key(emu);
        if (key < 0 || key == KEY_CTRL_EXIT || key == KEY_CTRL_AC)
            break;
        if (key == KEY_CTRL_EXE && !(mask >> sel & 1)) {
            ret = (uint32_t)sel;
            break;
        }
        if (key >= KEY_CHAR_0 + 1 && key <= KEY_CHAR_0 + 8 && !(mask >> (key - KEY_CHAR_0 - 1) & 1)) {
            ret = (uint32_t)(key - KEY_CHAR_0 - 1);
            break;
        }
        int step = key == KEY_CTRL_DOWN ? 1 : key == KEY_CTRL_UP ? 7 : key == KEY_CTRL_RIGHT ? 4 :
                   key == KEY_CTRL_LEFT ? 4 : 0;
        for (int tries = 0; step && tries < 8; tries++) {
            sel = (sel + step) % 8;
            if (!(mask >> sel & 1))
                break;
        }
    }
    msgbox_close(emu);
    return ret;
}

/* The bar of the progress dialogs: a frame and a filled part, in a box of
 * 4 lines whose heading (if any) is on the first. The box is opened when
 * current is 0; the add-in closes it with MsgBoxPop. */
static void progress(Emu *emu, const char *heading, int32_t current, int32_t max)
{
    if (current == 0)
        msgbox_open(emu, 4);
    BoxRect r = msgbox_rect(4);
    display_rect(emu->vram, r.x0 + 2, r.y0 + 2, r.x1 - 2, r.y1 - 2, 0xFFFF);
    if (heading)
        display_print_xy(emu->vram, 2, 2, heading, TEXT_MODE_NORMAL, 0);
    int x0 = r.x0 + 12, x1 = r.x1 - 12, y0 = 4 * BIG_CELL_H - 2, y1 = y0 + 16;
    display_rect(emu->vram, x0, y0, x1, y1, 0x0000);
    display_rect(emu->vram, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0xFFFF);
    if (max > 0) {
        if (current < 0) current = 0;
        if (current > max) current = max;
        int w = (int)((int64_t)(x1 - x0 - 3) * current / max);
        if (w > 0)
            display_rect(emu->vram, x0 + 2, y0 + 2, x0 + 1 + w, y1 - 2, 0x001F);
    }
    r61524_blit_vram(&emu->lcd, emu->vram);
}

/* ProgressBar2(heading, current, max) */
SC(sc_progress_bar2)
{
    char heading[64];
    read_line(emu, a[0], heading, sizeof heading);
    progress(emu, heading, (int32_t)a[1], (int32_t)a[2]);
    return 0;
}

/* ProgressBar(current, max): as ProgressBar2 without a heading. */
SC(sc_progress_bar)
{
    progress(emu, NULL, (int32_t)a[0], (int32_t)a[1]);
    return 0;
}

/* ProgressBar0(P1, P2, P3, current, max): the first three are unknown. */
SC(sc_progress_bar0)
{
    progress(emu, NULL, (int32_t)a[3], (int32_t)a[4]);
    return 0;
}

/* Scrollbar(struct scrollbar *): I1, indicatormaximum, indicatorheight,
 * indicatorpos, I5 (32-bit), then barleft, bartop, barheight, barwidth
 * (16-bit), in pixels of the area under the status bar. */
SC(sc_scrollbar)
{
    Memory *mem = &emu->mem;
    uint32_t max = mem_read32(mem, a[0] + 4), height = mem_read32(mem, a[0] + 8);
    uint32_t pos = mem_read32(mem, a[0] + 12);
    int left = mem_read16(mem, a[0] + 20), top = mem_read16(mem, a[0] + 22) + STATUS_HEIGHT;
    int bar_h = mem_read16(mem, a[0] + 24), bar_w = mem_read16(mem, a[0] + 26);
    if (bar_w <= 0 || bar_h <= 0)
        return 0;
    display_rect(emu->vram, left, top, left + bar_w - 1, top + bar_h - 1, 0x0000);
    display_rect(emu->vram, left + 1, top + 1, left + bar_w - 2, top + bar_h - 2, 0xFFFF);
    if (max) {
        int inner = bar_h - 2;
        int y = (int)((uint64_t)inner * pos / max), h = (int)((uint64_t)inner * height / max);
        if (h < 2) h = 2;
        if (y + h > inner) y = inner - h;
        display_rect(emu->vram, left + 1, top + 1 + y, left + bar_w - 2, top + y + h, 0x0000);
    }
    return 0;
}

/* BoxInnerClear(lines): clears the inside of a message box of that size. */
SC(sc_box_inner_clear)
{
    BoxRect r = msgbox_rect((int32_t)a[0]);
    display_rect(emu->vram, r.x0 + 2, r.y0 + 2, r.x1 - 2, r.y1 - 2, 0xFFFF);
    return 0;
}

/* BoxYLimits(lines, int *top, int *bottom): the inner top and bottom of a
 * message box of that size, in VRAM pixels. */
SC(sc_box_y_limits)
{
    BoxRect r = msgbox_rect((int32_t)a[0]);
    if (a[1])
        mem_write32(&emu->mem, a[1], (uint32_t)(r.y0 + 2));
    if (a[2])
        mem_write32(&emu->mem, a[2], (uint32_t)(r.y1 - 2));
    return 0;
}

/* CharacterSelectDialog(): picks a character in a box: the OS's symbols we
 * know the codes of, then ASCII punctuation. Arrows move, EXE chooses (its
 * code: 1 byte, or 2 for 2-byte characters), EXIT / AC give 0. The real
 * dialog's pages and layout aren't documented. */
SC(sc_character_select)
{
    (void)a;
    static const uint16_t CHARS[] = {
        0xD0, 0xCE, 0xCD, 0x7F50, 0x7F54, 0x86, 0x96, 0x8B, 0xA9, 0xB9, 0x99, 0x87, 0x0E, 0x10, 0x11, 0x12,
        0x13, 0xBB, 0x0F, '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', '-',
        '.', '/', ':', ';', '<', '=', '>', '?', '@', '[', '\\', ']', '^', '_', '`', '{',
        '|', '}', '~',
    };
    const int count = (int)(sizeof CHARS / sizeof CHARS[0]), per_row = 16, rows = (count + 15) / 16;
    int sel = 0;
    msgbox_open(emu, rows + 1);
    BoxRect r = msgbox_rect(rows + 1);
    uint32_t ret = 0;
    for (;;) {
        display_rect(emu->vram, r.x0 + 2, r.y0 + 2, r.x1 - 2, r.y1 - 2, 0xFFFF);
        display_print_xy(emu->vram, 2, 2, "Character", TEXT_MODE_NORMAL, 0);
        for (int i = 0; i < count; i++) {
            char c[3];
            if (CHARS[i] > 0xFF) {
                c[0] = (char)(CHARS[i] >> 8);
                c[1] = (char)CHARS[i];
                c[2] = '\0';
            } else {
                c[0] = (char)CHARS[i];
                c[1] = '\0';
            }
            display_print_xy(emu->vram, 3 + i % per_row, 3 + i / per_row, c,
                             i == sel ? TEXT_MODE_INVERT : TEXT_MODE_NORMAL, 0);
        }
        int key = sc_wait_key(emu);
        if (key < 0 || key == KEY_CTRL_EXIT || key == KEY_CTRL_AC)
            break;
        if (key == KEY_CTRL_EXE) {
            ret = CHARS[sel];
            break;
        }
        int step = key == KEY_CTRL_RIGHT ? 1 : key == KEY_CTRL_LEFT ? -1 : key == KEY_CTRL_DOWN ? per_row :
                   key == KEY_CTRL_UP ? -per_row : 0;
        if (sel + step >= 0 && sel + step < count)
            sel += step;
    }
    msgbox_close(emu);
    return ret;
}
