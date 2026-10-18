#include "keys.h"

#include <ctype.h>
#include <string.h>

const KeyInfo KEYS[] = {
    {"EXE", KEY_CTRL_EXE, 0x15},
    {"EXIT", KEY_CTRL_EXIT, 0x74},
    {"MENU", KEY_CTRL_MENU, 0x84},
    {"SHIFT", KEY_CTRL_SHIFT, 0x81},
    {"ALPHA", KEY_CTRL_ALPHA, 0x71},
    {"OPTN", KEY_CTRL_OPTN, 0x82},
    {"AC", KEY_CTRL_AC, 0x07},
    {"DEL", KEY_CTRL_DEL, 0x44},
    {"UP", KEY_CTRL_UP, 0x86},
    {"DOWN", KEY_CTRL_DOWN, 0x75},
    {"LEFT", KEY_CTRL_LEFT, 0x85},
    {"RIGHT", KEY_CTRL_RIGHT, 0x76},
    {"F1", KEY_CTRL_F1 + 0, 0x91},
    {"F2", KEY_CTRL_F1 + 1, 0x92},
    {"F3", KEY_CTRL_F1 + 2, 0x93},
    {"F4", KEY_CTRL_F1 + 3, 0x94},
    {"F5", KEY_CTRL_F1 + 4, 0x95},
    {"F6", KEY_CTRL_F1 + 5, 0x96},
    {"0", KEY_CHAR_0 + 0, 0x11},
    {"1", KEY_CHAR_0 + 1, 0x21},
    {"2", KEY_CHAR_0 + 2, 0x22},
    {"3", KEY_CHAR_0 + 3, 0x23},
    {"4", KEY_CHAR_0 + 4, 0x31},
    {"5", KEY_CHAR_0 + 5, 0x32},
    {"6", KEY_CHAR_0 + 6, 0x33},
    {"7", KEY_CHAR_0 + 7, 0x41},
    {"8", KEY_CHAR_0 + 8, 0x42},
    {"9", KEY_CHAR_0 + 9, 0x43},
    {"DP", KEY_CHAR_DP, 0x12},
    {"EXP", KEY_CHAR_EXP, 0x13},
    {"PLUS", KEY_CHAR_PLUS, 0x24},
    {"MINUS", KEY_CHAR_MINUS, 0x25},
    {"MULT", KEY_CHAR_MULT, 0x34},
    {"DIV", KEY_CHAR_DIV, 0x35},
    {"LPAR", KEY_CHAR_LPAR, 0x53},
    {"RPAR", KEY_CHAR_RPAR, 0x54},
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

void key_matrix(const bool *down, uint8_t rows[12])
{
    memset(rows, 0, 12);
    for (int i = 0; i < KEY_COUNT; i++)
        if (down[i])
            rows[KEYS[i].matrix >> 4] |= (uint8_t)(1 << (7 - (KEYS[i].matrix & 0xF)));
}
