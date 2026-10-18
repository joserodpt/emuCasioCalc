/*
 * HLE syscalls: the OS's messages in the chosen language (Emulation's
 * Language menu, --language, APP_SYSTEM_LANGUAGE).
 *
 * From WikiPrizm: six message languages (English, Spanish, German, French,
 * Portuguese, Chinese); messages by number, 1 being the language's name
 * (so add-ins learn the language with LocalizeMessage1(1, buf));
 * LocalizeMessage1 copies a message into a buffer of 88 bytes; PrintXY_2
 * draws one like PrintXY; APP_SYSTEM_LANGUAGE opens the language screen
 * (opt 1: as in the start-up wizard, leaves with F6; 0: as in System,
 * leaves with EXIT), and choosing applies at once.
 *
 * Our choices: only the ~40 messages WikiPrizm lists are known (others
 * read as "<msg N>"); the non-English texts are our translations; letters
 * with accents are written without them, as the OS's codes for them are
 * not documented (Chinese is GB 18030, the calculator's encoding for it).
 */
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "keys.h"
#include "platform.h"
#include "syscalls_impl.h"

typedef struct {
    int no;
    const char *text[LANG_COUNT];
} Message;

#include "messages_table.h"

#define MESSAGE_MAX 88 /* LocalizeMessage1's buffer */

/* A Latin letter with an accent (UTF-8, U+00C0-U+00FF) without it; the
 * inverted ¡ and ¿ are dropped. */
static void plain_latin(const char *utf8, char *out, size_t cap)
{
    static const char BASE[65] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)utf8; *s && n + 1 < cap; s++) {
        if (*s < 0x80) {
            out[n++] = (char)*s;
        } else if (*s == 0xC3 && s[1] >= 0x80 && s[1] <= 0xBF) {
            out[n++] = BASE[s[1] - 0x80];
            s++;
        } else if (*s == 0xC2 && (s[1] == 0xA1 || s[1] == 0xBF)) {
            s++; /* ¡ ¿ */
        } else {
            out[n++] = '?';
            while ((s[1] & 0xC0) == 0x80)
                s++;
        }
    }
    out[n] = '\0';
}

bool os_message(int lang, int no, char *out, size_t cap)
{
    if (lang < 0 || lang >= LANG_COUNT)
        lang = LANG_EN;
    for (size_t i = 0; i < sizeof MESSAGES / sizeof MESSAGES[0]; i++) {
        if (MESSAGES[i].no != no)
            continue;
        if (lang == LANG_ZH)
            snprintf(out, cap, "%s", MESSAGES[i].text[lang]);
        else
            plain_latin(MESSAGES[i].text[lang], out, cap);
        return true;
    }
    snprintf(out, cap, "<msg %d>", no);
    return false;
}

/* LocalizeMessage1(msgno, char *result): the message, NUL-terminated.
 * Returns 0 (the real return value isn't documented). */
SC(sc_localize_message1)
{
    char text[MESSAGE_MAX];
    os_message(emu->language, (int32_t)a[0], text, sizeof text);
    if (a[1])
        for (size_t i = 0; i <= strlen(text); i++)
            mem_write8(&emu->mem, a[1] + (uint32_t)i, (uint8_t)text[i]);
    return 0;
}

/* PrintXY_2(mode, x, y, msgno, color): the message drawn like PrintXY. */
SC(sc_print_xy_2)
{
    char text[MESSAGE_MAX];
    os_message(emu->language, (int32_t)a[3], text, sizeof text);
    display_print_xy(emu->vram, (int32_t)a[1], (int32_t)a[2], text, (int32_t)a[0], (int32_t)a[4]);
    return 0;
}

void set_language(Emu *emu, int lang)
{
    if (lang < 0 || lang >= LANG_COUNT)
        return;
    emu->language = lang;
    platform_set_language(lang);
}

/* APP_SYSTEM_LANGUAGE(opt): the Message Language screen. UP/DOWN move,
 * EXE chooses (at once, like the OS); EXIT leaves (opt 0), F6 leaves
 * (opt 1, the wizard). */
SC(sc_app_system_language)
{
    bool wizard = a[0] == 1;
    int sel = emu->language;
    for (;;) {
        display_fill(emu->vram, 0xFFFF);
        display_print_xy(emu->vram, 1, 1, "Message Language", TEXT_MODE_NORMAL, 0);
        for (int i = 0; i < LANG_COUNT; i++) {
            char name[MESSAGE_MAX], line[MESSAGE_MAX + 8];
            os_message(i, 1, name, sizeof name);
            if (i == LANG_ZH)
                snprintf(name, sizeof name, "Chinese"); /* GB 18030 text: not drawn yet */
            snprintf(line, sizeof line, "%c %s", i == emu->language ? '*' : ' ', name);
            display_print_xy(emu->vram, 2, 2 + i, line, i == sel ? TEXT_MODE_INVERT : TEXT_MODE_NORMAL, 0);
        }
        display_print_xy(emu->vram, 1, 8, wizard ? "EXE:Select  F6:Next" : "EXE:Select  EXIT:Back",
                         TEXT_MODE_NORMAL, 1);
        int key = sc_wait_key(emu);
        if (key < 0)
            return 0;
        if (key == KEY_CTRL_UP)
            sel = (sel + LANG_COUNT - 1) % LANG_COUNT;
        else if (key == KEY_CTRL_DOWN)
            sel = (sel + 1) % LANG_COUNT;
        else if (key == KEY_CTRL_EXE)
            set_language(emu, sel);
        else if (key >= KEY_CHAR_0 + 1 && key < KEY_CHAR_0 + 1 + LANG_COUNT)
            set_language(emu, sel = key - KEY_CHAR_0 - 1);
        else if (wizard ? key == KEY_CTRL_F1 + 5 : key == KEY_CTRL_EXIT)
            return 0;
    }
}
