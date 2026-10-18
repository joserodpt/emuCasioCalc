#ifndef EMU_KEYS_H
#define EMU_KEYS_H

#include <stdbool.h>
#include <stdint.h>

/* GetKey() key codes, as defined in libfxcg's include/fxcg/keyboard.h.
 * Verify against that header when adding more. */
#define KEY_CHAR_0      0x30
#define KEY_CHAR_DP     0x2E
#define KEY_CHAR_EXP    0x0F
#define KEY_CHAR_PLUS   0x89
#define KEY_CHAR_MINUS  0x99
#define KEY_CHAR_MULT   0xA9
#define KEY_CHAR_DIV    0xB9
#define KEY_CHAR_LPAR   0x28
#define KEY_CHAR_RPAR   0x29

#define KEY_CTRL_EXE    30004
#define KEY_CTRL_EXIT   30002
#define KEY_CTRL_MENU   30003
#define KEY_CTRL_SHIFT  30006
#define KEY_CTRL_ALPHA  30007
#define KEY_CTRL_OPTN   30008
#define KEY_CTRL_F1     30009  /* F1..F6 are consecutive */
#define KEY_CTRL_AC     30015
#define KEY_CTRL_UP     30018
#define KEY_CTRL_LEFT   30020
#define KEY_CTRL_RIGHT  30021
#define KEY_CTRL_DOWN   30023
#define KEY_CTRL_DEL    30025

/*
 * A calculator key: its name (for --keys), the GetKey code the OS returns
 * for it, and its position in the key matrix as gint numbers it (row << 4 |
 * (7 - column), see gint's <gint/keycodes.h>). The KEYSC hardware reports
 * row r in bit "column" of byte r.
 */
typedef struct {
    const char *name;
    int getkey;
    uint8_t matrix;
} KeyInfo;

extern const KeyInfo KEYS[];
extern const int KEY_COUNT;

/* Index into KEYS, or -1. Case-insensitive. */
int key_find(const char *name);

/* Sets the matrix bits of every key with down[i] set (rows: 12 bytes). */
void key_matrix(const bool *down, uint8_t rows[12]);

#endif
