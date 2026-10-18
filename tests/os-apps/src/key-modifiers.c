/* KeyModifiers: tests SHIFT and ALPHA in the OS GetKey (emuCasioFxCG).

   GetKey is called from the OS world (gint_world_switch), like a libfxcg
   add-in would, and each code it returns is checked. If all match,
   \\fls0\KEYTEST.txt is written with "ok", else with the first mismatch.

   Keys, in this order (emuCasioFxCG: --keys ...; fx-CG50):
     ALPHA,7                 'M'
     SHIFT,EXIT              QUIT
     SHIFT,ALPHA,XOT,LOG,DP  alpha lock: 'A' 'B' ' '
     ALPHA,7                 lock off: '7'
     SHIFT,SHIFT,7           SHIFT cancelled: '7'
     ALPHA,SQUARE            r
     SHIFT,LOG               10^x
     ALPHA,EXE,7             EXE, then '7' (ALPHA was for one key)
     SHIFT,DEL               UNDO
     SHIFT,AC,1              AC/ON ignored (power off): '1'
     NEG,FD,VARS             (-) F<>D VARS */

#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/gint.h>
#include <gint/bfile.h>
#include <string.h>

#include "rows.h"

/* A syscall stub: jump to the OS's syscall entry with the number in r0. */
#define SYSCALL(name, number) \
    __asm__(".text\n.global _" #name "\n.align 1\n_" #name ":\n" \
        "\tmov.l 1f, r2\n\tmov.l 2f, r0\n\tjmp @r2\n\tnop\n" \
        ".align 2\n1:\t.long 0x80020070\n2:\t.long " #number "\n")

SYSCALL(OS_GetKey, 0x0EAB);
int OS_GetKey(int *key);

/* libfxcg's keyboard.h */
#define KEY_CTRL_EXE    30004
#define KEY_CTRL_QUIT   30029
#define KEY_CTRL_UNDO   30045
#define KEY_CTRL_FD     30046
#define KEY_CTRL_VARS   30016
#define KEY_CHAR_VALR   0xcd
#define KEY_CHAR_EXPN10 0xb5
#define KEY_CHAR_PMINUS 0x87

static int const expected[] = {
    'M', KEY_CTRL_QUIT, 'A', 'B', ' ', '7', '7', KEY_CHAR_VALR,
    KEY_CHAR_EXPN10, KEY_CTRL_EXE, '7', KEY_CTRL_UNDO, '1',
    KEY_CHAR_PMINUS, KEY_CTRL_FD, KEY_CTRL_VARS,
};
#define CHECKS ((int)(sizeof expected / sizeof expected[0]))
static int got[CHECKS];

static int run(void)
{
    for(int i = 0; i < CHECKS; i++) {
        int key = 0;
        OS_GetKey(&key);
        got[i] = key;
    }

    /* Marker for automated runs */
    int failed = -1;
    for(int i = 0; i < CHECKS; i++)
        if(got[i] != expected[i] && failed < 0) failed = i;
    static uint16_t const path[] = u"\\\\fls0\\KEYTEST.txt";
    char text[32] = "ok";
    if(failed >= 0) {
        strcpy(text, "fail: key ");
        text[10] = '0' + failed / 10;
        text[11] = '0' + failed % 10;
        text[12] = 0;
    }
    int size = (strlen(text) + 1) & ~1;
    BFile_Remove(path);
    BFile_Create(path, BFile_File, &size);
    int fd = BFile_Open(path, BFile_WriteOnly);
    if(fd >= 0) {
        BFile_Write(fd, text, size);
        BFile_Close(fd);
    }
    return 0;
}

int main(void)
{
    gint_world_switch(GINT_CALL(run));

    dclear(C_WHITE);
    row_title("KeyModifiers");
    for(int i = 0; i < CHECKS; i++) {
        int ok = got[i] == expected[i];
        /* two columns of 8 */
        row_print_color(i % 8 + 2, 1 + i / 8 * 24, ok ? C_RGB(0, 20, 0) : C_RED, C_WHITE,
            ok ? "%2d: %d ok" : "%2d: %d (want %d)", i + 1, got[i], expected[i]);
    }
    dupdate();

    while(getkey().key != KEY_EXIT) {}
    return 1;
}
