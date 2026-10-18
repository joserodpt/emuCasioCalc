#include "keys.h"

#include <ctype.h>
#include <string.h>

/* SHIFT and ALPHA codes: the yellow and red labels of the fx-CG keyboard,
 * as libfxcg's keyboard.h names them. */
const KeyInfo KEYS[] = {
    {"EXE", KEY_CTRL_EXE, 0x15, 0x0D /* carriage return */, 0},
    {"EXIT", KEY_CTRL_EXIT, 0x74, KEY_CTRL_QUIT, 0},
    {"MENU", KEY_CTRL_MENU, 0x84, 30037 /* SETUP */, 0},
    {"SHIFT", KEY_CTRL_SHIFT, 0x81, 0, 0},
    {"ALPHA", KEY_CTRL_ALPHA, 0x71, 0, 0},
    {"OPTN", KEY_CTRL_OPTN, 0x82, 30059 /* KEY_SHIFT_OPTN */, 0},
    {"VARS", KEY_CTRL_VARS, 0x83, 30028 /* PRGM */, 0},
    {"AC", KEY_CTRL_AC, 0x07, 0, 0},
    {"DEL", KEY_CTRL_DEL, 0x44, KEY_CTRL_INS, 0},
    {"UP", KEY_CTRL_UP, 0x86, 30052 /* PAGEUP */, 0},
    {"DOWN", KEY_CTRL_DOWN, 0x75, 30053 /* PAGEDOWN */, 0},
    {"LEFT", KEY_CTRL_LEFT, 0x85, 30062 /* KEY_SHIFT_LEFT */, 0},
    {"RIGHT", KEY_CTRL_RIGHT, 0x76, 30063 /* KEY_SHIFT_RIGHT */, 0},
    {"F1", KEY_CTRL_F1 + 0, 0x91, 0, 0},
    {"F2", KEY_CTRL_F1 + 1, 0x92, 0, 0},
    {"F3", KEY_CTRL_F1 + 2, 0x93, 0, 0},
    {"F4", KEY_CTRL_F1 + 3, 0x94, 0, 0},
    {"F5", KEY_CTRL_F1 + 4, 0x95, 0, 0},
    {"F6", KEY_CTRL_F1 + 5, 0x96, 0, 0},
    {"XOT", KEY_CTRL_XTT, 0x61, 0x7F54 /* angle */, 'A'},
    {"LOG", KEY_CHAR_LOG, 0x62, 0xB5 /* 10^x */, 'B'},
    {"LN", KEY_CHAR_LN, 0x63, 0xA5 /* e^x */, 'C'},
    {"SIN", KEY_CHAR_SIN, 0x64, 0x91 /* asin */, 'D'},
    {"COS", KEY_CHAR_COS, 0x65, 0x92 /* acos */, 'E'},
    {"TAN", KEY_CHAR_TAN, 0x66, 0x93 /* atan */, 'F'},
    {"FRAC", KEY_CHAR_FRAC, 0x51, 30054 /* MIXEDFRAC */, 'G'},
    {"FD", KEY_CTRL_FD, 0x52, 30026 /* FRACCNVRT */, 'H'},
    {"LPAR", KEY_CHAR_LPAR, 0x53, 0x96 /* cube root */, 'I'},
    {"RPAR", KEY_CHAR_RPAR, 0x54, 0x9B /* x^-1 */, 'J'},
    {"COMMA", KEY_CHAR_COMMA, 0x55, 0, 'K'},
    {"ARROW", KEY_CHAR_STORE, 0x56, 0, 'L'},
    {"SQUARE", KEY_CHAR_SQUARE, 0x72, 0x86 /* square root */, 0xCD /* r */},
    {"POWER", KEY_CHAR_POW, 0x73, 0xB8 /* x-th root */, 0xCE /* theta */},
    {"0", KEY_CHAR_0 + 0, 0x11, 0x7F50 /* i */, 'Z'},
    {"1", KEY_CHAR_0 + 1, 0x21, 0x7F51 /* List */, 'U'},
    {"2", KEY_CHAR_0 + 2, 0x22, 0x7F40 /* Mat */, 'V'},
    {"3", KEY_CHAR_0 + 3, 0x23, 0, 'W'},
    {"4", KEY_CHAR_0 + 4, 0x31, 30100 /* CATALOG */, 'P'},
    {"5", KEY_CHAR_0 + 5, 0x32, 30101 /* FORMAT */, 'Q'},
    {"6", KEY_CHAR_0 + 6, 0x33, 0, 'R'},
    {"7", KEY_CHAR_0 + 7, 0x41, 30055 /* CAPTURE */, 'M'},
    {"8", KEY_CHAR_0 + 8, 0x42, 30050 /* CLIP */, 'N'},
    {"9", KEY_CHAR_0 + 9, 0x43, 30036 /* PASTE */, 'O'},
    {"DP", KEY_CHAR_DP, 0x12, '=', KEY_CHAR_SPACE},
    {"EXP", KEY_CHAR_EXP, 0x13, 0xD0 /* pi */, '"'},
    {"NEG", KEY_CHAR_PMINUS, 0x14, 0xC0 /* Ans */, 0},
    {"PLUS", KEY_CHAR_PLUS, 0x24, '[', 'X'},
    {"MINUS", KEY_CHAR_MINUS, 0x25, ']', 'Y'},
    {"MULT", KEY_CHAR_MULT, 0x34, '{', 'S'},
    {"DIV", KEY_CHAR_DIV, 0x35, '}', 'T'},
};

const int KEY_COUNT = (int)(sizeof KEYS / sizeof KEYS[0]);

int key_find(const char *name)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        const char *a = KEYS[i].name, *b = name;
        while (*a && toupper((unsigned char)*a) == toupper((unsigned char)*b))
            a++, b++;
        if (!*a && !*b)
            return i;
    }
    return -1;
}

int key_for_letter(int c)
{
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    for (int i = 0; i < KEY_COUNT; i++)
        if (KEYS[i].alpha == c)
            return i;
    return -1;
}

void key_matrix(const bool *down, uint8_t rows[12])
{
    memset(rows, 0, 12);
    for (int i = 0; i < KEY_COUNT; i++)
        if (down[i])
            rows[KEYS[i].matrix >> 4] |= (uint8_t)(1 << (7 - (KEYS[i].matrix & 0xF)));
}
