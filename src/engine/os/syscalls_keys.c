/*
 * HLE syscalls: keyboard and line editing.
 *
 * Blocking calls take keys from --keys, then from the host keyboard.
 * Non-blocking calls (PRGM_GetKey_OS, GetKeyWait_OS without waiting) look at
 * the keys held right now, which the main loop updates every 10 ms.
 */
#include <string.h>

#include "display.h"
#include "keys.h"
#include "platform.h"
#include "syscalls.h"
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

/* The OS's non-blocking key reads "stall for a small amount of time"
 * (WikiPrizm, PRGM_GetKey): about 20 us here, an estimate. */
#define KEY_POLL_CYCLES 2400

/* First key held right now, or -1. */
static int held_key(Emu *emu)
{
    emu->polls_keys = true;
    sc_spend(emu, KEY_POLL_CYCLES);
    for (int k = 0; k < KEY_COUNT; k++)
        if (emu->key_down[k])
            return k;
    return -1;
}

/* KEYS index of an OS matrix position, or -1. */
static int key_at(int col, int row)
{
    for (int k = 0; k < KEY_COUNT; k++) {
        int c, r;
        os_matrix(k, &c, &r);
        if (c == col && r == row)
            return k;
    }
    return -1;
}

/* Takes injected key i out of the queue. */
static void inject_take(Emu *emu, int i)
{
    memmove(&emu->inject[i], &emu->inject[i + 1], (size_t)(emu->inject_count - i - 1) * sizeof emu->inject[0]);
    emu->inject_count--;
}

/* The next key for a blocking read: an injected one first (GetKey codes
 * only for GetKey: getkey set), else the script or the keyboard. Returns a
 * KEYS index, -2 with a GetKey code in *code, or -1 to stop. */
static int next_input(Emu *emu, bool getkey, int *code)
{
    for (;;) {
        for (int i = 0; i < emu->inject_count; i++) {
            if (emu->inject[i].key < 0 && !getkey)
                continue;
            int key = emu->inject[i].key;
            *code = emu->inject[i].code;
            inject_take(emu, i);
            return key < 0 ? -2 : key;
        }
        int key = emu_next_key(emu);
        if (key != -3) /* -3: a timer injected a key, look again */
            return key;
    }
}

static void set_mode(Emu *emu, bool shift, KeyAlpha alpha)
{
    emu->key_shift = shift;
    emu->key_alpha = alpha;
    platform_set_key_mode(shift, (int)alpha);
}

/* GetKey handles SHIFT and ALPHA itself, as the OS does: they change the
 * mode and the next key returns its yellow (SHIFT) or red (ALPHA) function.
 * SHIFT then ALPHA locks ALPHA until ALPHA is pressed again; SHIFT keeps
 * the lock. SHIFT AC/ON switches the calculator off: here, nothing. */
static void show(Emu *emu)
{
    if (emu->getkey_status)
        status_area_draw(emu);
    r61524_blit_vram(&emu->lcd, emu->vram);
}

/* With GetKey's return to the main menu on (the OS default), MENU leaves
 * the add-in for the main menu, which isn't emulated: the key is dropped. */
static bool menu_dropped(Emu *emu, int code)
{
    if (code != KEY_CTRL_MENU || emu->menu_return_off)
        return false;
    platform_flash("MENU: the calculator would show its main menu (not emulated)");
    return true;
}

/* GetKey draws the status area (unless disabled) before waiting, and again
 * when SHIFT or ALPHA changes its indicator. Keys injected with
 * Keyboard_PutKeycode come first. */
int sc_wait_key(Emu *emu)
{
    show(emu);
    for (;;) {
        int injected = 0;
        int key = next_input(emu, true, &injected);
        if (key == -2)
            return injected;
        if (key < 0) {
            emu->cpu.state = CPU_EXITED;
            return -1;
        }
        const KeyInfo *k = &KEYS[key];
        bool shift = emu->key_shift;
        KeyAlpha alpha = emu->key_alpha;
        if (k->getkey == KEY_CTRL_SHIFT) {
            set_mode(emu, !shift, alpha);
            show(emu);
            continue;
        }
        if (k->getkey == KEY_CTRL_ALPHA) {
            if (shift)
                set_mode(emu, false, ALPHA_LOCK);
            else
                set_mode(emu, false, alpha == ALPHA_OFF ? ALPHA_ONCE : ALPHA_OFF);
            show(emu);
            continue;
        }
        set_mode(emu, false, alpha == ALPHA_ONCE ? ALPHA_OFF : alpha);
        int code = k->getkey;
        if (shift) {
            if (k->getkey == KEY_CTRL_AC)
                continue;
            if (k->getkey == KEY_CTRL_DEL && emu->model == MODEL_CG50)
                code = KEY_CTRL_UNDO;
            else if (k->shift)
                code = k->shift;
        } else if (alpha != ALPHA_OFF && k->alpha) {
            code = k->alpha;
        }
        if (menu_dropped(emu, code))
            continue;
        return code;
    }
}

/* GetKey(int *key): refreshes the screen, blocks for a key. */
SC(sc_get_key)
{
    int key = sc_wait_key(emu);
    if (key < 0)
        return 0;
    mem_write32(&emu->mem, a[0], (uint32_t)key);
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
    int ignored = 0;
    key = -1;
    for (int i = 0; i < emu->inject_count && key < 0; i++) { /* injected matrix codes first */
        if (emu->inject[i].key >= 0) {
            key = emu->inject[i].key;
            inject_take(emu, i);
        }
    }
    if (key >= 0) {
        /* injected */
    } else if (type == 1) {
        key = held_key(emu);
        if (key < 0)
            return 0;
    } else if (type == 2 && emu->headless && emu->script_pos >= emu->script_len) {
        time_advance(emu, (uint64_t)a[3] * sh7305_iphi_hz(&emu->hw));
        return 2;
    } else {
        key = next_input(emu, false, &ignored);
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

/* The control keys of the line editors: LEFT / RIGHT move over whole
 * characters, DEL removes the one before the cursor, AC clears the line;
 * *key is cleared once handled. Positions are byte offsets, as
 * EditMBStringChar returns. *start (if given) scrolls so the cursor stays
 * within `cells` cells. */
static void edit_ctrl(Emu *emu, uint32_t str, uint32_t start_p, uint32_t xpos_p, uint32_t key_p, int cells)
{
    Memory *mem = &emu->mem;
    char s[256];
    sc_string(emu, str, s, sizeof s);
    int len = (int)strlen(s);
    int xpos = (int32_t)mem_read32(mem, xpos_p);
    int key = (int32_t)mem_read32(mem, key_p);
    if (xpos < 0 || xpos > len)
        xpos = len;
    /* the character before xpos starts at prev */
    int prev = 0;
    for (int i = 0, l; i < xpos && (l = text_char_len(s + i)); i += l)
        prev = i;
    bool handled = true;
    switch (key) {
    case KEY_CTRL_LEFT: xpos = xpos > 0 ? prev : 0; break;
    case KEY_CTRL_RIGHT: if (xpos < len) xpos += text_char_len(s + xpos); break;
    case KEY_CTRL_DEL:
        if (xpos > 0) {
            memmove(s + prev, s + xpos, (size_t)(len - xpos + 1));
            xpos = prev;
        }
        break;
    case KEY_CTRL_AC: s[0] = '\0'; xpos = 0; break;
    default: handled = false;
    }
    len = (int)strlen(s);
    for (int i = 0; i <= len; i++)
        mem_write8(mem, str + (uint32_t)i, (uint8_t)s[i]);
    mem_write32(mem, xpos_p, (uint32_t)xpos);
    if (start_p) {
        int start = (int32_t)mem_read32(mem, start_p);
        if (start < 0 || start > xpos)
            start = xpos;
        if (cells < 1)
            cells = 1;
        /* the cursor needs a cell of its own after the text before it */
        for (;;) {
            int n = 0;
            for (int i = start, l; i < xpos && (l = text_char_len(s + i)); i += l)
                n++;
            if (n < cells || start >= xpos)
                break;
            start += text_char_len(s + start);
        }
        mem_write32(mem, start_p, (uint32_t)start);
    }
    if (handled)
        mem_write32(mem, key_p, 0);
}

/* Cells from homescreen column x (1-21) to the end of the line. */
static int cells_from(int32_t x)
{
    return x >= 1 && x <= 21 ? 22 - x : 21;
}

/* EditMBStringCtrl(string, posmax, int *start, int *xpos, int *key, x, y) */
SC(sc_edit_mbstring_ctrl)
{
    edit_ctrl(emu, a[0], a[2], a[3], a[4], cells_from((int32_t)a[5]));
    return 0;
}

/* EditMBStringCtrl2(string, xposmax, int *start, int *xpos, int *key, x, y,
 * enable_pos_to_clear, pos_to_clear); Ctrl3 and Ctrl4 take more parameters
 * of unknown meaning after the same ones (libfxcg's prototypes), and are
 * handled the same way. */
SC(sc_edit_mbstring_ctrl2)
{
    int cells = a[7] && (int32_t)a[8] > 0 ? (int32_t)a[8] : cells_from((int32_t)a[5]);
    edit_ctrl(emu, a[0], a[2], a[3], a[4], cells);
    return 0;
}

/* Keyboard_PutKeycode(x, y, keycode): x and y below 0 put keycode in
 * GetKey's buffer; else (x, y) is a matrix code read as a key press by the
 * next keyboard read. Returns 0, or -1 if the buffer is full or (x, y)
 * isn't a key. */
SC(sc_put_keycode)
{
    int32_t x = (int32_t)a[0], y = (int32_t)a[1];
    int key = -1;
    if (x >= 0 || y >= 0) {
        key = key_at(x, y);
        if (key < 0)
            return (uint32_t)-1;
    }
    if (emu->inject_count == (int)(sizeof emu->inject / sizeof emu->inject[0]))
        return (uint32_t)-1;
    emu->inject[emu->inject_count].key = key;
    emu->inject[emu->inject_count].code = (int32_t)a[2];
    emu->inject_count++;
    emu->inject_serial++;
    return 0;
}

/* Keyboard_SpyMatrixCode(char *column, char *row): the next matrix code
 * waiting (injected, else a key held now), left in place. Returns how many
 * are waiting. */
SC(sc_spy_matrix_code)
{
    int count = 0, first = -1;
    for (int i = 0; i < emu->inject_count; i++)
        if (emu->inject[i].key >= 0 && count++ == 0)
            first = emu->inject[i].key;
    if (first < 0) {
        for (int k = 0; k < KEY_COUNT && first < 0; k++)
            if (emu->key_down[k])
                first = k;
        count = first >= 0;
    }
    if (first >= 0) {
        int col, row;
        os_matrix(first, &col, &row);
        if (a[0]) mem_write8(&emu->mem, a[0], (uint8_t)col);
        if (a[1]) mem_write8(&emu->mem, a[1], (uint8_t)row);
    }
    return (uint32_t)count;
}

/* Bkey flags: keyboard-related OS flags (OPTN, VARS, PRGM, CLIP, CATALOG
 * handling in GetKey); kept, not acted on. */
SC(sc_bkey_clr_all_flags) { (void)a; emu->bkey_flags = 0; return 0; }
SC(sc_bkey_set_flag) { emu->bkey_flags |= (uint16_t)a[0]; return 0; }
SC(sc_bkey_get_all_flags) { (void)a; return emu->bkey_flags; }
SC(sc_bkey_set_all_flags) { emu->bkey_flags = (uint16_t)a[0]; return 0; }

/* GetKey's MENU: back to the main menu (1, default) or a key (0). */
SC(sc_get_menu_return) { (void)a; return !emu->menu_return_off; }
SC(sc_enable_menu_return) { (void)a; emu->menu_return_off = false; return 0; }
SC(sc_disable_menu_return) { (void)a; emu->menu_return_off = true; return 0; }
