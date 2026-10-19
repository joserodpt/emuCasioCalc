/* StatusBar: tests the OS status area syscalls (emuCasioCalc).

   Each syscall is called from the OS world (gint_world_switch), like a
   libfxcg add-in would, and checked by reading VRAM back. If every check
   passes, \\fls0\STATTEST.txt is written with "ok", else with the first
   failing check.

   Run with the battery low and these keys (emuCasioCalc):
     --battery low --keys ALPHA,7
   ALPHA,7 goes to GetKey (it returns 'M' and draws the ALPHA indicator). */

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

SYSCALL(Bdisp_GetPoint_VRAM, 0x0267);
SYSCALL(Bdisp_Fill_VRAM, 0x0275);
SYSCALL(EnableStatusArea, 0x02B7);
SYSCALL(DefineStatusAreaFlags, 0x02B8);
SYSCALL(DrawHeaderLine, 0x02BB);
SYSCALL(OS_GetKey, 0x0EAB);
SYSCALL(GetMainBatteryVoltage, 0x1186);
SYSCALL(DefineStatusMessage, 0x1D77);
SYSCALL(DefineStatusGlyph, 0x1D7B);
SYSCALL(EnableDisplayHeader, 0x1D7F);
SYSCALL(DisplayStatusArea, 0x1D81);
SYSCALL(Bdisp_HeaderFill, 0x1D86);
SYSCALL(BatteryIcon, 0x1D89);

unsigned short Bdisp_GetPoint_VRAM(int x, int y);
void Bdisp_Fill_VRAM(int color, int mode);
void EnableStatusArea(int opt);
int DefineStatusAreaFlags(int mode, int flags, char *color1, char *color2);
void DrawHeaderLine(void);
int OS_GetKey(int *key);
int GetMainBatteryVoltage(int one);
void DefineStatusMessage(char *msg, short P2, char color, char P4);
struct glyph_def {
    short dx, dy;
    void *shortcolor_bitmap;
    short xalignment, xoffset;
};
void DefineStatusGlyph(int idx, struct glyph_def *glyph);
int EnableDisplayHeader(int action, int value);
void DisplayStatusArea(void);
void Bdisp_HeaderFill(unsigned char c1, unsigned char c2);
void BatteryIcon(unsigned int show);

#define SAF_BATTERY     0x0001
#define SAF_ALPHA_SHIFT 0x0002
#define SAF_TEXT        0x0100
#define SAF_GLYPH       0x0200

#define WHITE 0xFFFF
#define BLUE  0x001F

#define CHECKS 12
static char const *names[CHECKS] = {
    "flags mode 3", "colours mode 2", "flags add/del", "battery icon",
    "message", "disabled", "HeaderFill", "flags mode 1", "header line",
    "glyph", "display header", "GetKey ALPHA",
};
static int results[CHECKS];
static int values[CHECKS];

static void check(int i, int ok, int value)
{
    results[i] = ok;
    values[i] = value;
}

/* Is there a pixel of this colour in the status area, right of x0? */
static int header_has(int x0, unsigned short color)
{
    for(int y = 0; y < 24; y++)
    for(int x = x0; x < 384; x++)
        if(Bdisp_GetPoint_VRAM(x, y) == color) return 1;
    return 0;
}

static int run(void)
{
    char c1 = 7, c2 = 7;

    /* 0: mode 3 sets colours and flags */
    EnableStatusArea(0);
    int f = DefineStatusAreaFlags(3, SAF_BATTERY | SAF_TEXT, &c1, &c2);
    check(0, f == (SAF_BATTERY | SAF_TEXT), f);

    /* 1: mode 2 reads the colours back */
    char r1 = 0, r2 = 0;
    DefineStatusAreaFlags(2, 0, &r1, &r2);
    check(1, r1 == 7 && r2 == 7, r1);

    /* 2: modes 4 and 5 add and remove flags */
    f = DefineStatusAreaFlags(4, SAF_ALPHA_SHIFT, 0, 0);
    int f2 = DefineStatusAreaFlags(5, SAF_ALPHA_SHIFT, 0, 0);
    check(2, f == 0x103 && f2 == 0x101, f);

    /* 3: battery outline at the left; --battery low: 1 bar, red; the
       voltage follows the emulator's setting */
    Bdisp_Fill_VRAM(BLUE, 3);
    DisplayStatusArea();
    int mv = GetMainBatteryVoltage(1);
    check(3, Bdisp_GetPoint_VRAM(3, 6) == 0 && header_has(0, 0xF800) && mv == 440, mv);

    /* 4: the message, in red (PrintXY colour 4), right of the battery */
    DefineStatusAreaFlags(3, SAF_TEXT, &c1, &c2);
    DefineStatusMessage("Hello", 0, 4, 0);
    DisplayStatusArea();
    check(4, header_has(0, 0xF800) && Bdisp_GetPoint_VRAM(0, 0) == WHITE, 0);

    /* 5: disabled: DisplayStatusArea leaves VRAM alone */
    EnableStatusArea(3);
    Bdisp_Fill_VRAM(BLUE, 3);
    DisplayStatusArea();
    check(5, Bdisp_GetPoint_VRAM(0, 0) == BLUE, Bdisp_GetPoint_VRAM(0, 0));
    EnableStatusArea(0);

    /* 6: Bdisp_HeaderFill with blue (colour 1) */
    Bdisp_HeaderFill(1, 1);
    check(6, Bdisp_GetPoint_VRAM(0, 0) == BLUE && Bdisp_GetPoint_VRAM(0, 30) == BLUE, 0);

    /* 7: mode 1 fills white and sets all flags */
    f = DefineStatusAreaFlags(1, 0, 0, 0);
    check(7, f == 0x303 && Bdisp_GetPoint_VRAM(0, 0) == WHITE, f);

    /* 8: the line under the area */
    DrawHeaderLine();
    check(8, Bdisp_GetPoint_VRAM(100, 23) != WHITE, Bdisp_GetPoint_VRAM(100, 23));

    /* 9: a 4x4 green glyph at x = 300 (alignment 3: absolute) */
    static unsigned short const green[16] = {
        0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0,
        0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0,
    };
    struct glyph_def g = { 4, 4, (void *)green, 3, 300 };
    DefineStatusGlyph(0, &g);
    DefineStatusAreaFlags(3, SAF_GLYPH, &c1, &c2);
    DisplayStatusArea();
    check(9, Bdisp_GetPoint_VRAM(301, 11) == 0x07E0 && !header_has(0, 0) , Bdisp_GetPoint_VRAM(301, 11));
    DefineStatusGlyph(0, 0);

    /* 10: EnableDisplayHeader sets and returns GetKey's flag */
    int h = EnableDisplayHeader(2, 5);
    int h2 = EnableDisplayHeader(1, 0);
    EnableDisplayHeader(2, 1);
    check(10, h == 5 && h2 == 5, h);

    /* 11: GetKey draws the area, with the ALPHA indicator after ALPHA */
    DefineStatusAreaFlags(3, SAF_BATTERY | SAF_ALPHA_SHIFT, &c1, &c2);
    BatteryIcon(0);
    int key = 0;
    OS_GetKey(&key);
    check(11, key == 'M' && header_has(0, 0xD000) && Bdisp_GetPoint_VRAM(3, 6) != 0, key);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\STATTEST.txt";
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
    row_title("StatusBar");
    for(int k = 0; k < CHECKS; k++) {
        int row = k % 8 + 2, x = 1 + k / 8 * 24;
        if(results[k])
            row_print_color(row, x, C_RGB(0, 20, 0), C_WHITE, "%-15s ok", names[k]);
        else
            row_print_color(row, x, C_RED, C_WHITE, "%-15s %d", names[k], values[k]);
    }
    dupdate();

    while(getkey().key != KEY_EXIT) {}
    return 1;
}
