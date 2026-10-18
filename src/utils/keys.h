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
#define KEY_CHAR_PMINUS 0x87
#define KEY_CHAR_FRAC   0xBB
#define KEY_CHAR_COMMA  0x2C
#define KEY_CHAR_STORE  0x0E
#define KEY_CHAR_LOG    0x95
#define KEY_CHAR_LN     0x85
#define KEY_CHAR_SIN    0x81
#define KEY_CHAR_COS    0x82
#define KEY_CHAR_TAN    0x83
#define KEY_CHAR_SQUARE 0x8B
#define KEY_CHAR_POW    0xA8
#define KEY_CHAR_A      0x41  /* A..Z are ASCII */
#define KEY_CHAR_SPACE  0x20

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
#define KEY_CTRL_XTT    30001
#define KEY_CTRL_VARS   30016
#define KEY_CTRL_FD     30046
#define KEY_CTRL_UNDO   30045  /* SHIFT DEL on the fx-CG50 */
#define KEY_CTRL_INS    30033  /* SHIFT DEL on the fx-CG10/20 */
#define KEY_CTRL_QUIT   30029

/*
 * A calculator key: its name (for --keys), the GetKey code the OS returns
 * for it, the codes after SHIFT and after ALPHA (0: none, the key acts
 * as without the modifier), and its position in the key matrix as gint numbers it (row << 4 |
 * (7 - column), see gint's <gint/keycodes.h>). The KEYSC hardware reports
 * row r in bit "column" of byte r.
 */
typedef struct {
    const char *name;
    int getkey;
    uint8_t matrix;
    int shift, alpha;
} KeyInfo;

extern const KeyInfo KEYS[];
extern const int KEY_COUNT;

/* Index into KEYS, or -1. Case-insensitive. */
int key_find(const char *name);

/* The key whose ALPHA function types c (a letter, either case, or a
 * space), or -1. */
int key_for_letter(int c);

/* Sets the matrix bits of every key with down[i] set (rows: 12 bytes). */
void key_matrix(const bool *down, uint8_t rows[12]);

#endif
