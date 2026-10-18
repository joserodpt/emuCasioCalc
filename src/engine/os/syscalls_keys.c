/*
 * HLE syscalls: keyboard and line editing.
 *
 * Blocking calls take keys from --keys, then from the host keyboard.
 * Non-blocking calls (PRGM_GetKey_OS, GetKeyWait_OS without waiting) look at
 * the keys held right now, which the main loop updates every 10 ms.
 */
#include <string.h>

#include "keys.h"
#include "syscalls_impl.h"

/* OS matrix position of KEYS[k]: columns 7..2 left to right, rows 2..10
 * bottom to top; AC/on is (1, 1). gint's numbering is row << 4 | (7 - column
 * bit), with rows one lower. */
static void os_matrix(int k, int *col, int *row)
{
    uint8_t m = KEYS[k].matrix;
    if (m == 0x07) {
        *col = *row = 1;
        return;
    }
    *col = 8 - (m & 0xF);
    *row = (m >> 4) + 1;
}

/* First key held right now, or -1. */
static int held_key(Emu *emu)
{
    emu->polls_keys = true;
    for (int k = 0; k < KEY_COUNT; k++)
        if (emu->key_down[k])
            return k;
    return -1;
}

/* GetKey(int *key): refreshes the screen, blocks for a key. */
SC(sc_get_key)
{
    r61524_blit_vram(&emu->lcd, emu->vram);
    int key = emu_next_key(emu);
    if (key < 0) {
        emu->cpu.state = CPU_EXITED;
        return 0;
    }
    mem_write32(&emu->mem, a[0], (uint32_t)KEYS[key].getkey);
    return 1;
}

/* GetKeyWait_OS(int *column, int *row, type, timeout_s, menu, unsigned short
 * *keycode). type 1 doesn't wait (returns 0 if no key); 0 waits; 2 waits up
 * to timeout_s seconds (returns 2 on timeout). Returns 1 with a key. */
SC(sc_get_key_wait_os)
{
    Memory *mem = &emu->mem;
    uint32_t type = a[2];
    int key;
    if (type == 1) {
        key = held_key(emu);
        if (key < 0)
            return 0;
    } else if (type == 2 && emu->headless && emu->script_pos >= emu->script_len) {
        emu->cpu.instructions += (uint64_t)a[3] * sh7305_iphi_hz(&emu->hw);
        return 2;
    } else {
        key = emu_next_key(emu);
        if (key < 0) {
            emu->cpu.state = CPU_EXITED;
            return 0;
        }
    }
    int col, row;
    os_matrix(key, &col, &row);
    mem_write32(mem, a[0], (uint32_t)col);
    mem_write32(mem, a[1], (uint32_t)row);
    if (a[5])
        mem_write16(mem, a[5], 0);
    return 1;
}

/* PRGM_GetKey_OS(unsigned char buffer[12]): the key held now as a BCD
 * number, column * 10 + row - 1 (the fx-9860G BASIC Getkey code, e.g. EXE
 * = 31, F1 = 79); 0 for no key; AC/on has exponent 0xF00 and mantissa 1.
 * Returns 0 if AC/on is held, else 1. */
SC(sc_prgm_get_key_os)
{
    uint8_t bcd[12] = {0};
    int key = held_key(emu);
    int ret = 1;
    if (key >= 0) {
        int col, row;
        os_matrix(key, &col, &row);
        if (col == 1 && row == 1) {
            bcd[0] = 0xF0;
            bcd[1] = 0x01;
            ret = 0;
        } else {
            /* exponent 0x101 (10^1), digits col, row - 1 */
            bcd[0] = 0x10;
            bcd[1] = (uint8_t)(0x10 | col);
            bcd[2] = (uint8_t)((row - 1) << 4);
        }
    }
    for (int i = 0; i < 12; i++)
        mem_write8(&emu->mem, a[0] + (uint32_t)i, bcd[i]);
    return (uint32_t)ret;
}

/* EditMBStringChar(string, posmax, xpos, char): inserts a character at
 * xpos if the string has room; returns the new cursor position. */
SC(sc_edit_mbstring_char)
{
    Memory *mem = &emu->mem;
    char s[256];
    sc_string(emu, a[0], s, sizeof s);
    int len = (int)strlen(s), posmax = (int32_t)a[1], xpos = (int32_t)a[2];
    uint32_t c = a[3];
    int width = c > 0xFF ? 2 : 1;
    if (xpos < 0 || xpos > len || len + width > posmax || len + width >= (int)sizeof s)
        return (uint32_t)xpos;
    memmove(s + xpos + width, s + xpos, (size_t)(len - xpos + 1));
    if (width == 2)
        s[xpos] = (char)(c >> 8);
    s[xpos + width - 1] = (char)c;
    for (int i = 0; i <= len + width; i++)
        mem_write8(mem, a[0] + (uint32_t)i, (uint8_t)s[i]);
    return (uint32_t)(xpos + width);
}

/* EditMBStringCtrl2(string, xposmax, int *start, int *xpos, int *key, x, y,
 * enable_pos_to_clear, pos_to_clear): applies a control key (cursor
 * movement, DEL, AC) to the line and clears *key once handled. */
SC(sc_edit_mbstring_ctrl2)
{
    Memory *mem = &emu->mem;
    char s[256];
    sc_string(emu, a[0], s, sizeof s);
    int len = (int)strlen(s);
    int xpos = (int32_t)mem_read32(mem, a[3]);
    int key = (int32_t)mem_read32(mem, a[4]);
    bool handled = true;

    switch (key) {
    case KEY_CTRL_LEFT: if (xpos > 0) xpos--; break;
    case KEY_CTRL_RIGHT: if (xpos < len) xpos++; break;
    case KEY_CTRL_DEL:
        if (xpos > 0) {
            memmove(s + xpos - 1, s + xpos, (size_t)(len - xpos + 1));
            xpos--;
        }
        break;
    case KEY_CTRL_AC: s[0] = '\0'; xpos = 0; break;
    default: handled = false;
    }
    if (xpos > len)
        xpos = len;
    for (int i = 0; i <= len; i++)
        mem_write8(mem, a[0] + (uint32_t)i, (uint8_t)s[i]);
    mem_write32(mem, a[3], (uint32_t)xpos);
    if (a[2])
        mem_write32(mem, a[2], 0); /* lines are short: always show from the start */
    if (handled)
        mem_write32(mem, a[4], 0);
    return 0;
}
