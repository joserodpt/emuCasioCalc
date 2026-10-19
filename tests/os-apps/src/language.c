/* Language: tests the message language syscalls (emuCasioCalc).

   From the OS world (gint_world_switch), like a libfxcg add-in. The
   language is learnt as add-ins do: LocalizeMessage1(1) gives its name.
   If every check passes, \\fls0\LANGTEST.txt is written with "ok", else
   with the first failing check.

   Run with German and these keys (emuCasioCalc):
     --language de --keys DOWN,EXE,EXIT,UP,EXE,F6
   DOWN,EXE,EXIT   the System language screen: French, then back
   UP,EXE,F6       the wizard's language screen: German again, then Next */

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

SYSCALL(Bdisp_AllClr_VRAM, 0x0272);
SYSCALL(Bdisp_GetPoint_VRAM, 0x0267);
SYSCALL(LocalizeMessage1, 0x12FC);
SYSCALL(PrintXY_2, 0x18EC);
SYSCALL(APP_SYSTEM_LANGUAGE, 0x1E0D);

void Bdisp_AllClr_VRAM(void);
unsigned short Bdisp_GetPoint_VRAM(int x, int y);
int LocalizeMessage1(int msgno, char *result);
void PrintXY_2(int mode, int x, int y, int msgno, int color);
void APP_SYSTEM_LANGUAGE(int opt);

#define CHECKS 7
static char const *names[CHECKS] = {
    "language name", "message 3", "PrintXY_2", "System screen",
    "unknown message", "message 80", "wizard screen",
};
static int results[CHECKS];
static char got[CHECKS][24];

static void check(int i, int ok, char const *text)
{
    results[i] = ok;
    strncpy(got[i], text, sizeof got[i] - 1);
}

static char buf[88];

static char const *message(int no)
{
    memset(buf, 0, sizeof buf);
    LocalizeMessage1(no, buf);
    return buf;
}

static int run(void)
{
    /* 0, 1: started in German */
    check(0, !strcmp(message(1), "Deutsch"), buf);
    check(1, !strcmp(message(3), "Ja:[F1]"), buf);

    /* 2: PrintXY_2 draws message 2 on row 1 */
    Bdisp_AllClr_VRAM();
    PrintXY_2(0, 1, 1, 2, 0);
    int dark = 0;
    for(int y = 24; y < 48; y++)
    for(int x = 0; x < 200; x++)
        dark += Bdisp_GetPoint_VRAM(x, y) == 0;
    check(2, dark > 50, dark > 50 ? "drawn" : "nothing");

    /* 3: the System menu's language screen: DOWN, EXE (French), EXIT */
    APP_SYSTEM_LANGUAGE(0);
    check(3, !strcmp(message(1), "Francais"), buf);

    /* 4, 5: an unknown number, and a known one in French */
    check(4, !strcmp(message(9999), "<msg 9999>"), buf);
    check(5, !strcmp(message(80), "ERREUR syntaxe"), buf);

    /* 6: the wizard's screen: UP, EXE (German), F6 */
    APP_SYSTEM_LANGUAGE(1);
    check(6, !strcmp(message(1), "Deutsch"), buf);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\LANGTEST.txt";
    char text[32] = "ok";
    if(failed >= 0) {
        strcpy(text, "fail: ");
        strcat(text, names[failed]);
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
    row_title("Language");
    for(int k = 0; k < CHECKS; k++) {
        row_print_color(k + 2, 1, results[k] ? C_RGB(0, 20, 0) : C_RED, C_WHITE,
            "%-16s %s  %s", names[k], results[k] ? "ok" : "FAIL", got[k]);
    }
    dupdate();

    while(getkey().key != KEY_EXIT) {}
    return 1;
}
