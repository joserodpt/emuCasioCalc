/* TextSyscalls: tests the text and multi-byte string syscalls
   (emuCasioCalc).

   From the OS world (gint_world_switch), like a libfxcg add-in; drawing is
   checked by reading VRAM back. If every check passes, \\fls0\TEXTTEST.txt
   is written with "ok", else with the first failing check.

   Keys (emuCasioCalc: --keys RIGHT,RIGHT,EXE): the character dialog,
   choosing its third character (r, 0xCD). */

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

SYSCALL(DisableGB18030, 0x01DF);
SYSCALL(EnableGB18030, 0x01E0);
SYSCALL(GetMiniGlyphPtr, 0x01E9);
SYSCALL(PrintMiniMini, 0x021B);
SYSCALL(PrintGlyph, 0x0238);
SYSCALL(DispInt, 0x0259);
SYSCALL(Bdisp_GetPoint_VRAM, 0x0267);
SYSCALL(Bdisp_AllClr_VRAM, 0x0272);
SYSCALL(CharacterSelectDialog, 0x07A2);
SYSCALL(Bdisp_MMPrintRef, 0x0D08);
SYSCALL(MB_IsLead, 0x1161);
SYSCALL(MB_ElementCount, 0x1163);
SYSCALL(EditMBStringCtrl2, 0x120A);
SYSCALL(EditMBStringCtrl, 0x120E);
SYSCALL(DisplayMBString, 0x121D);
SYSCALL(ProcessPrintChars, 0x1300);
SYSCALL(locate_OS, 0x1863);
SYSCALL(PrintLine2, 0x1865);
SYSCALL(PrintXY, 0x18F9);

void DisableGB18030(void);
void EnableGB18030(void);
void *GetMiniGlyphPtr(unsigned short glyph, unsigned short *info);
void PrintMiniMini(int *x, int *y, char const *s, int mode, char color, int P6);
void PrintGlyph(int x, int y, void *glyph, int mode, int color, int back, int P7);
void DispInt(int a, int b);
unsigned short Bdisp_GetPoint_VRAM(int x, int y);
void Bdisp_AllClr_VRAM(void);
short CharacterSelectDialog(void);
void Bdisp_MMPrintRef(int *x, int *y, char const *s, int mode, int xlimit, int P6, int P7,
    int color, int back, int writeflag, int P11);
int MB_IsLead(char c);
int MB_ElementCount(char const *s);
void EditMBStringCtrl2(char *s, int xposmax, int *start, int *xpos, int *key, int x, int y,
    int enable_pos_to_clear, int pos_to_clear);
void EditMBStringCtrl(char *s, int posmax, int *start, int *xpos, int *key, int x, int y);
void DisplayMBString(char const *s, int start, int cursor, int x, int y);
int ProcessPrintChars(unsigned short code);
void locate_OS(int x, int y);
void PrintLine2(int P1, int mode, char const *msg, int P4, int P5, int imax, int color1, int color2);
void PrintXY(int x, int y, char const *s, int mode, int color);

#define KEY_CTRL_LEFT  30020
#define KEY_CTRL_RIGHT 30021
#define KEY_CTRL_DEL   30025

#define CHECKS 14
static char const *names[CHECKS] = {
    "MB_IsLead", "MB_ElementCount", "ProcessPrintChars", "GB18030 on/off",
    "MMPrintRef", "PrintLine2", "DisplayMBString", "EditMBStringCtrl",
    "Ctrl2 scrolling", "PrintGlyph", "PrintMiniMini bold", "2-byte in PrintXY",
    "DispInt", "CharacterSelect",
};
static int results[CHECKS];
static int values[CHECKS];

static void check(int i, int ok, int value)
{
    results[i] = ok;
    values[i] = value;
}

/* Pixels of a colour in a rectangle */
static int count(int x0, int y0, int x1, int y1, unsigned short c)
{
    int n = 0;
    for(int y = y0; y <= y1; y++)
    for(int x = x0; x <= x1; x++)
        n += Bdisp_GetPoint_VRAM(x, y) == c;
    return n;
}

static int run(void)
{
    /* 0, 1: lead bytes; 2-byte characters count as one */
    check(0, MB_IsLead((char)0xE5) == 1 && MB_IsLead('A') == 0 && MB_IsLead(0x7F) == 1, 0);
    int n = MB_ElementCount("A\xE6\x02" "B");
    check(1, n == 3, n);

    /* 2, 3: character sets */
    int p1 = ProcessPrintChars(0x03A8), lead_gb = MB_IsLead((char)0x81);
    int p2 = ProcessPrintChars(0x0000), lead_latin = MB_IsLead((char)0x81);
    int p3 = ProcessPrintChars(0x0005);
    check(2, p1 == 1 && p2 == 1 && p3 == 0 && lead_gb == 1 && lead_latin == 0, p3);
    EnableGB18030();
    int on = MB_IsLead((char)0x90);
    DisableGB18030();
    int off = MB_IsLead((char)0x90);
    check(3, on == 1 && off == 0, on * 10 + off);

    /* 4: the 16 px font; writeflag 0 only measures */
    Bdisp_AllClr_VRAM();
    int x = 10, y = 0;
    Bdisp_MMPrintRef(&x, &y, "Hello", 0, -1, 0, 0, 0x0000, 0xFFFF, 0, 0);
    int measured = x - 10;
    int dark0 = count(0, 24, 100, 40, 0x0000);
    x = 10;
    Bdisp_MMPrintRef(&x, &y, "Hello", 0, -1, 0, 0, 0x0000, 0xFFFF, 1, 0);
    int dark = count(0, 24, 100, 40, 0x0000);
    check(4, measured > 20 && measured < 80 && x - 10 == measured && dark0 == 0 && dark > 20, measured);

    /* 5: PrintLine2, red on white, row 2 */
    Bdisp_AllClr_VRAM();
    locate_OS(1, 2);
    PrintLine2(0, 1, "Hi", 0, -1, 4, 4, 7);
    int red = count(0, 48, 18 * 5, 71, 0xF800);
    check(5, red > 20, red);

    /* 6: DisplayMBString with the cursor before the third character: a bar
       at x = 36 */
    Bdisp_AllClr_VRAM();
    DisplayMBString("Hello", 0, 2, 1, 3);
    int text = count(0, 72, 35, 95, 0x0000), bar = count(36, 74, 37, 93, 0x0000);
    check(6, text > 20 && bar >= 30, bar);

    /* 7: editing over a 2-byte character */
    static char buf[16] = "A\xE6\x02" "B";
    int start = 0, xpos = 4, key = KEY_CTRL_LEFT;
    EditMBStringCtrl(buf, 16, &start, &xpos, &key, 1, 24);
    int k1 = key, x1 = xpos;          /* before B: 3 */
    key = KEY_CTRL_LEFT;
    EditMBStringCtrl(buf, 16, &start, &xpos, &key, 1, 24);
    int x2 = xpos;                    /* before the 2-byte one: 1 */
    key = KEY_CTRL_DEL;
    EditMBStringCtrl(buf, 16, &start, &xpos, &key, 1, 24);
    check(7, k1 == 0 && x1 == 3 && x2 == 1 && xpos == 0 && !strcmp(buf, "\xE6\x02" "B"), x2);

    /* 8: a 30-character line, cursor at its end: the start scrolls so the
       cursor fits in 21 cells */
    static char longline[32] = "abcdefghijklmnopqrstuvwxyz0123";
    start = 0;
    xpos = 30;
    key = KEY_CTRL_RIGHT;
    EditMBStringCtrl2(longline, 31, &start, &xpos, &key, 1, 24, 0, 0);
    check(8, start == 10 && xpos == 30, start);

    /* 9: PrintGlyph with a glyph from GetMiniGlyphPtr */
    Bdisp_AllClr_VRAM();
    unsigned short info = 0;
    void *w = GetMiniGlyphPtr('W', &info);
    PrintGlyph(100, 50, w, 0, 0x0000, 0xFFFF, 0);
    int g = count(100, 74, 117, 97, 0x0000);
    check(9, g > 20 && info > 0, g);

    /* 10: bold PrintMiniMini is wider */
    int xa = 0, ya = 100, xb = 0, yb = 120;
    PrintMiniMini(&xa, &ya, "Hello", 0, 0, 0);
    PrintMiniMini(&xb, &yb, "Hello", 0x10, 0, 0);
    check(10, xa > 10 && xb > xa, xb - xa);

    /* 11: a 2-byte character in PrintXY draws something (one cell) */
    Bdisp_AllClr_VRAM();
    PrintXY(1, 1, "  \xE6\x02", 0, 0);
    int c = count(0, 24, 35, 47, 0x0000);
    check(11, c > 10 && count(18, 24, 35, 47, 0x0000) == 0, c);

    /* 12: DispInt is accepted */
    DispInt(1, 2);
    check(12, 1, 0);

    /* 13: the character dialog (RIGHT, RIGHT, EXE): 'r' */
    int ch = CharacterSelectDialog();
    check(13, ch == 0xCD, ch);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\TEXTTEST.txt";
    char result[32] = "ok";
    if(failed >= 0) {
        strcpy(result, "fail: ");
        strcat(result, names[failed]);
    }
    int size = (strlen(result) + 1) & ~1;
    BFile_Remove(path);
    BFile_Create(path, BFile_File, &size);
    int fd = BFile_Open(path, BFile_WriteOnly);
    if(fd >= 0) {
        BFile_Write(fd, result, size);
        BFile_Close(fd);
    }
    return 0;
}

int main(void)
{
    gint_world_switch(GINT_CALL(run));

    dclear(C_WHITE);
    row_title("TextSyscalls");
    for(int k = 0; k < CHECKS; k++) {
        int row = k % 8 + 2, x = 1 + k / 8 * 24;
        if(results[k])
            row_print_color(row, x, C_RGB(0, 20, 0), C_WHITE, "%-18s ok", names[k]);
        else
            row_print_color(row, x, C_RED, C_WHITE, "%-18s %d", names[k], values[k]);
    }
    dupdate();

    while(getkey().key != KEY_EXIT) {}
    return 1;
}
