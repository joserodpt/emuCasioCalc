/* DrawSyscalls: tests OS drawing and memory/string syscalls (emuCasioFxCG).

   Each syscall is called from the OS world (gint_world_switch), like a
   libfxcg add-in would, and checked by reading pixels back. If every check
   passes, \\fls0\DRAWTEST.txt is written with "ok", else with the first
   failing check. No keys are needed until the result screen (EXIT). */

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

SYSCALL(GetVRAMAddress, 0x01E6);
SYSCALL(Bdisp_PutDisp_DD_stripe, 0x0260);
SYSCALL(Bdisp_SetPointWB_VRAM, 0x0262);
SYSCALL(Bdisp_GetPoint_VRAM, 0x0267);
SYSCALL(Bdisp_SetPoint_DD, 0x026B);
SYSCALL(Bdisp_GetPoint_DD, 0x026F);
SYSCALL(Bdisp_Fill_VRAM, 0x0275);
SYSCALL(Bdisp_WriteGraphVRAM, 0x0290);
SYSCALL(FrameColor, 0x02A3);
SYSCALL(DirectDrawRectangle, 0x02AA);
SYSCALL(Bdisp_AreaClr, 0x02B2);
SYSCALL(Bdisp_Rectangle, 0x0924);
SYSCALL(Bdisp_FilledRectangleFullColor, 0x0926);
SYSCALL(HexToByte, 0x1344);
SYSCALL(HexToWord, 0x1345);
SYSCALL(ByteToHex, 0x1347);
SYSCALL(WordToHex, 0x1348);
SYSCALL(LongToAscHex, 0x1841);
SYSCALL(OS_memcmp, 0x1DD1);
SYSCALL(GetSecondaryVRAMAddress, 0x1E50);
SYSCALL(SaveVRAM_1, 0x1E62);

void *GetVRAMAddress(void);
void Bdisp_PutDisp_DD_stripe(int y1, int y2);
void Bdisp_SetPointWB_VRAM(int x, int y, unsigned short color);
unsigned short Bdisp_GetPoint_VRAM(int x, int y);
void Bdisp_SetPoint_DD(int x, int y, unsigned short color);
unsigned short Bdisp_GetPoint_DD(int x, int y);
void Bdisp_Fill_VRAM(int color, int mode);
unsigned short FrameColor(int mode, unsigned short color);
void DirectDrawRectangle(int x1, int y1, int x2, int y2, unsigned short color);
void Bdisp_Rectangle(int x1, int y1, int x2, int y2, char color);
void Bdisp_FilledRectangleFullColor(int x1, int y1, int x2, int y2, unsigned short color);
int HexToByte(unsigned char const *value, unsigned char *result);
int HexToWord(unsigned char const *value, unsigned short *result);
void ByteToHex(unsigned char value, unsigned char *result);
void WordToHex(unsigned short value, unsigned char *result);
void LongToAscHex(int value, unsigned char *result, int digits);
int OS_memcmp(void const *p1, void const *p2, int n);
void *GetSecondaryVRAMAddress(void);
void SaveVRAM_1(void);

struct display_fill {
    int x1, y1, x2, y2;
    unsigned char mode;
};
void Bdisp_AreaClr(struct display_fill *area, unsigned char target, unsigned short color);

struct display_graph {
    int x, y, xofs, yofs, width, height;
    char colormode, zero4, P20_1, P20_2;
    int bitmap;
    char color_idx1, color_idx2, color_idx3, P20_3;
    char writemodify, writekind, zero6, one1;
    int transparency;
};
void Bdisp_WriteGraphVRAM(struct display_graph *gd);

#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F
#define WHITE 0xFFFF

#define CHECKS 16
static char const *names[CHECKS] = {
    "Fill_VRAM all", "Fill_VRAM body", "AreaClr colour", "AreaClr invert",
    "FilledRectFull", "Rectangle", "SetPointWB", "Set/GetPoint_DD",
    "WriteGraph 565", "WriteGraph 1bpp", "PutDisp_stripe", "DirectDrawRect",
    "FrameColor", "secondary VRAM", "memcmp", "hex",
};
static int results[CHECKS];
static int values[CHECKS];

static void check(int i, int ok, int value)
{
    results[i] = ok;
    values[i] = value;
}

static int run(void)
{
    /* 0, 1: Bdisp_Fill_VRAM, whole VRAM then below the status bar */
    Bdisp_Fill_VRAM(RED, 3);
    int p = Bdisp_GetPoint_VRAM(0, 0);
    check(0, p == RED && Bdisp_GetPoint_VRAM(383, 215) == RED, p);
    Bdisp_Fill_VRAM(WHITE, 1);
    p = Bdisp_GetPoint_VRAM(0, 30);
    check(1, Bdisp_GetPoint_VRAM(0, 0) == RED && p == WHITE, p);

    /* 2, 3: Bdisp_AreaClr on VRAM: colour, then invert */
    struct display_fill area = { 100, 100, 120, 110, 1 };
    Bdisp_AreaClr(&area, 1, GREEN);
    p = Bdisp_GetPoint_VRAM(110, 105);
    check(2, p == GREEN && Bdisp_GetPoint_VRAM(121, 105) == WHITE, p);
    area.mode = 4;
    Bdisp_AreaClr(&area, 1, 0);
    p = Bdisp_GetPoint_VRAM(110, 105);
    check(3, p == (unsigned short)~GREEN, p);

    /* 4, 5: rectangles, y counted under the status bar */
    Bdisp_FilledRectangleFullColor(10, 10, 20, 20, GREEN);
    p = Bdisp_GetPoint_VRAM(15, 15 + 24);
    check(4, p == GREEN && Bdisp_GetPoint_VRAM(15, 15) != GREEN, p);
    Bdisp_Rectangle(30, 10, 40, 20, 1); /* TEXT_COLOR_BLUE outline */
    p = Bdisp_GetPoint_VRAM(30, 15 + 24);
    check(5, p == BLUE && Bdisp_GetPoint_VRAM(35, 15 + 24) == WHITE, p);

    /* 6: Bdisp_SetPointWB_VRAM */
    Bdisp_SetPointWB_VRAM(200, 200, 0x1234);
    p = Bdisp_GetPoint_VRAM(200, 200);
    check(6, p == 0x1234, p);

    /* 7: a pixel straight on the display */
    Bdisp_SetPoint_DD(300, 50, 0x4321);
    p = Bdisp_GetPoint_DD(300, 50);
    check(7, p == 0x4321 && Bdisp_GetPoint_VRAM(300, 50) != 0x4321, p);

    /* 8: a 2x2 RGB565 bitmap */
    static unsigned short const img[4] = { RED, GREEN, BLUE, 0x0841 };
    struct display_graph gd;
    memset(&gd, 0, sizeof gd);
    gd.x = 50; gd.y = 150; gd.width = 2; gd.height = 2;
    gd.colormode = 2; gd.bitmap = (int)img;
    gd.writemodify = 1; gd.writekind = 1; gd.one1 = 1;
    Bdisp_WriteGraphVRAM(&gd);
    p = Bdisp_GetPoint_VRAM(51, 151);
    check(8, Bdisp_GetPoint_VRAM(50, 150) == RED && Bdisp_GetPoint_VRAM(51, 150) == GREEN &&
        Bdisp_GetPoint_VRAM(50, 151) == BLUE && p == 0x0841, p);

    /* 9: 1 bpp, set bits black (index 0), clear bits transparent */
    static unsigned char const bits[2] = { 0xA0, 0x40 }; /* 8x2: 1010.. / 0100.. */
    gd.x = 60; gd.width = 8; gd.colormode = 3; gd.bitmap = (int)bits;
    gd.color_idx1 = 0; gd.color_idx2 = 7; gd.color_idx3 = 7;
    Bdisp_Fill_VRAM(GREEN, 3);
    Bdisp_WriteGraphVRAM(&gd);
    p = Bdisp_GetPoint_VRAM(60, 150);
    check(9, p == 0 && Bdisp_GetPoint_VRAM(61, 150) == GREEN &&
        Bdisp_GetPoint_VRAM(61, 151) == 0, p);

    /* 10: rows 150-151 of VRAM onto the display, row 100 untouched */
    Bdisp_SetPoint_DD(0, 100, RED);
    Bdisp_PutDisp_DD_stripe(150, 151);
    p = Bdisp_GetPoint_DD(60, 150);
    check(10, p == 0 && Bdisp_GetPoint_DD(51, 150) == GREEN &&
        Bdisp_GetPoint_DD(0, 100) == RED, p);

    /* 11: panel coordinates: panel (16, 14) is VRAM (10, 10) */
    DirectDrawRectangle(16, 14, 20, 18, BLUE);
    p = Bdisp_GetPoint_DD(10, 10);
    check(11, p == BLUE && Bdisp_GetPoint_DD(9, 10) != BLUE, p);

    /* 12: the border around the VRAM area */
    unsigned short f1 = FrameColor(1, BLUE);
    unsigned short f0 = FrameColor(0, 0);
    check(12, f1 == BLUE && f0 == WHITE, f1);

    /* 13: SaveVRAM_1 copies VRAM to the secondary VRAM */
    unsigned short *vram = GetVRAMAddress();
    unsigned short *vram2 = GetSecondaryVRAMAddress();
    SaveVRAM_1();
    check(13, vram2 && vram2 != vram && vram2[150 * 384 + 60] == 0 &&
        vram2[150 * 384 + 51] == GREEN, (int)((unsigned)vram2 >> 16));

    /* 14: memcmp */
    int c1 = OS_memcmp("abcd", "abcd", 4), c2 = OS_memcmp("abcd", "abce", 4);
    check(14, c1 == 0 && c2 < 0 && OS_memcmp("b", "a", 1) > 0, c2);

    /* 15: hex conversions */
    unsigned char byte = 0, hex[9] = "........";
    unsigned short word = 0;
    int ok = HexToByte((unsigned char const *)"a5", &byte) == 0 && byte == 0xA5;
    ok = ok && HexToWord((unsigned char const *)"BEEF", &word) == 0 && word == 0xBEEF;
    ok = ok && HexToByte((unsigned char const *)"g1", &byte) != 0 && byte == 0xA5;
    ByteToHex(0x3C, hex);
    ok = ok && !memcmp(hex, "3C......", 8);
    WordToHex(0xABCD, hex);
    ok = ok && !memcmp(hex, "ABCD....", 8);
    LongToAscHex(0x12345678, hex, 6);
    ok = ok && !memcmp(hex, "345678..", 8);
    check(15, ok, word);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\DRAWTEST.txt";
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
    row_title("DrawSyscalls");
    for(int k = 0; k < CHECKS; k++) {
        /* two columns of 8 */
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
