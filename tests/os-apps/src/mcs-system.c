/* McsSystem: tests the main memory (MCS) and system syscalls
   (emuCasioFxCG).

   From the OS world (gint_world_switch), like a libfxcg add-in. If every
   check passes, \\fls0\MCSTEST.txt is written with "ok", else with the
   first failing check. Last, Restart is called: the add-in ends there (the
   calculator would restart), so there is no result screen; the emulator
   prints a line about it. No keys needed. */

#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/gint.h>
#include <gint/bfile.h>
#include <string.h>

/* A syscall stub: jump to the OS's syscall entry with the number in r0. */
#define SYSCALL(name, number) \
    __asm__(".text\n.global _" #name "\n.align 1\n_" #name ":\n" \
        "\tmov.l 1f, r2\n\tmov.l 2f, r0\n\tjmp @r2\n\tnop\n" \
        ".align 2\n1:\t.long 0x80020070\n2:\t.long " #number "\n")

SYSCALL(Alpha_GetData, 0x0034);
SYSCALL(Alpha_SetData, 0x0035);
SYSCALL(BCDtoInternal, 0x0160);
SYSCALL(CLIP_Store, 0x07E5);
SYSCALL(SMEM_MapIconToExt, 0x0C2C);
SYSCALL(App_InitDlgDescriptor, 0x0D79);
SYSCALL(Restart, 0x1187);
SYSCALL(MCS_DeleteDirectory, 0x1516);
SYSCALL(MCS_WriteItem, 0x151A);
SYSCALL(MCS_GetState, 0x1529);
SYSCALL(MCS_GetCapa, 0x1532);
SYSCALL(MCS_GetMainMemoryStart, 0x1543);
SYSCALL(MCS_CreateDirectory, 0x154B);
SYSCALL(MCSDelVar2, 0x1558);
SYSCALL(MCSGetOpenItem, 0x1560);
SYSCALL(MCSGetDlen2, 0x1562);
SYSCALL(MCSGetData1, 0x1563);
SYSCALL(TakeScreenshot, 0x17E6);
SYSCALL(Bfile_FindFirst, 0x1DB7);
SYSCALL(Bfile_FindNext, 0x1DB9);
SYSCALL(Bfile_FindClose, 0x1DBA);
SYSCALL(Bdisp_AllClr_VRAM, 0x0272);
SYSCALL(Bdisp_WriteGraphVRAM, 0x0290);
SYSCALL(PrintXY, 0x18F9);

void Alpha_GetData(char name, unsigned char *bcd);
void Alpha_SetData(char name, unsigned char const *bcd);
int BCDtoInternal(void *target, unsigned char const *source);
int CLIP_Store(unsigned char const *buffer, int length);
int SMEM_MapIconToExt(char const *filename, uint16_t const *folder, unsigned *msgno, uint16_t *icon);
void App_InitDlgDescriptor(void *p, unsigned char v);
void Restart(void);
int MCS_DeleteDirectory(char const *dir);
int MCS_WriteItem(char const *dir, char const *item, short type, int len, void const *buffer);
int MCS_GetState(int *max, int *used, int *free);
int MCS_GetCapa(int *bottom);
int MCS_GetMainMemoryStart(void);
int MCS_CreateDirectory(char const *dir);
int MCSDelVar2(char const *dir, char const *item);
int MCSGetOpenItem(char *item);
int MCSGetDlen2(char const *dir, char const *item, int *len);
int MCSGetData1(int offset, int len, void *buffer);
void TakeScreenshot(void);
typedef struct { unsigned short id, type; unsigned long fsize, dsize; unsigned property; unsigned long address; } file_type_t;
int Bfile_FindFirst(uint16_t const *pattern, int *handle, uint16_t *found, file_type_t *info);
int Bfile_FindNext(int handle, uint16_t *found, file_type_t *info);
int Bfile_FindClose(int handle);
void Bdisp_AllClr_VRAM(void);
struct display_graph {
    int x, y, xofs, yofs, width, height;
    char colormode, zero4, P20_1, P20_2;
    int bitmap;
    char color_idx1, color_idx2, color_idx3, P20_3;
    char writemodify, writekind, zero6, one1;
    int transparency;
};
void Bdisp_WriteGraphVRAM(struct display_graph *gd);
void PrintXY(int x, int y, char const *s, int mode, int color);

/* An 18x24 icon at (x, y), scaled 2x by drawing each pixel as 2x2 */
static void show_icon(uint16_t const *icon, int x, int y)
{
    static uint16_t big[36 * 48];
    for(int j = 0; j < 48; j++)
    for(int i = 0; i < 36; i++)
        big[j * 36 + i] = icon[(j / 2) * 18 + i / 2];
    struct display_graph gd;
    memset(&gd, 0, sizeof gd);
    gd.x = x; gd.y = y; gd.width = 36; gd.height = 48;
    gd.colormode = 2; gd.bitmap = (int)big;
    gd.writemodify = 1; gd.writekind = 1; gd.one1 = 1;
    Bdisp_WriteGraphVRAM(&gd);
}

struct BCDinternal { int exponent, sign, unknown; char mantissa[15]; };

#define CHECKS 12
static char const *names[CHECKS] = {
    "MCS_WriteItem", "WriteItem errors", "MCSGetOpenItem", "MCS_GetState",
    "MCS_GetCapa", "DeleteDirectory", "CLIP_Store", "Alpha variables",
    "BCDtoInternal", "SMEM_MapIconToExt", "InitDlgDescriptor", "TakeScreenshot",
};
static int results[CHECKS];

static int captures(void)
{
    int h = -1, n = 0;
    uint16_t found[32];
    file_type_t info;
    int rc = Bfile_FindFirst(u"\\\\fls0\\CAPT*.bmp", &h, found, &info);
    while(rc == 0) {
        n++;
        rc = Bfile_FindNext(h, found, &info);
    }
    Bfile_FindClose(h);
    return n;
}

static int run(void)
{
    /* 0: an item written and read back */
    static char const data[8] = "MCSdata";
    char back[8];
    int len = 0;
    MCS_CreateDirectory("TESTD");
    int rc = MCS_WriteItem("TESTD", "ITEM1", 0, 8, data);
    int rc2 = MCSGetDlen2("TESTD", "ITEM1", &len);
    MCSGetData1(0, 8, back);
    results[0] = rc == 0 && rc2 == 0 && len == 8 && !memcmp(back, data, 8);

    /* 1: length not a multiple of 4, missing directory */
    results[1] = MCS_WriteItem("TESTD", "X", 0, 6, data) == 0x10 &&
        MCS_WriteItem("NODIR", "X", 0, 8, data) == 0x40;

    /* 2: the item MCSGetDlen2 selected */
    char item[16] = "";
    MCSGetDlen2("TESTD", "ITEM1", &len);
    results[2] = MCSGetOpenItem(item) == 0 && !strcmp(item, "ITEM1");

    /* 3, 4: sizes */
    int max = 0, used = 0, free = 0, bottom = 0;
    rc = MCS_GetState(&max, &used, &free);
    results[3] = rc == 0 && max > 0 && used >= 8 && free == max - used;
    int capa = MCS_GetCapa(&bottom);
    int start = MCS_GetMainMemoryStart();
    results[4] = capa == max && start != 0 && bottom == start + used;

    /* 5: a directory with items can't go; system ones never */
    int d1 = MCS_DeleteDirectory("TESTD");
    MCSDelVar2("TESTD", "ITEM1");
    int d2 = MCS_DeleteDirectory("TESTD");
    int d3 = MCS_DeleteDirectory("TESTD");
    results[5] = d1 == 0x45 && d2 == 0 && d3 == 0x40 && MCS_DeleteDirectory("main") == 0x46 &&
        MCS_DeleteDirectory("") == 0xF0;

    /* 6: the clipboard is the item @REV2/CLIP */
    static unsigned char const clip[4] = "abc";
    rc = CLIP_Store(clip, 4);
    rc2 = MCSGetDlen2("@REV2", "CLIP", &len);
    results[6] = rc == 0 && rc2 == 0 && len == 4;

    /* 7: A = 1.5, r = -31 */
    static unsigned char const one_half[12] = { 0x10, 0x01, 0x50 };
    static unsigned char const minus_31[12] = { 0x60, 0x13, 0x10 };
    unsigned char got[12];
    Alpha_SetData('A', one_half);
    Alpha_SetData((char)0xCD, minus_31);
    Alpha_GetData('A', got);
    int ok_a = !memcmp(got, one_half, 12);
    Alpha_GetData((char)0xCD, got);
    results[7] = ok_a && !memcmp(got, minus_31, 12);

    /* 8: -31 as exponent 1, negative, digits 3 1 0... */
    struct BCDinternal in;
    memset(&in, 0x55, sizeof in);
    rc = BCDtoInternal(&in, minus_31);
    results[8] = rc == 0 && in.exponent == 1 && in.sign == 1 && in.mantissa[0] == 3 &&
        in.mantissa[1] == 1 && in.mantissa[2] == 0;

    /* 9: icons: magenta background, an add-in's blue band, a folder */
    static uint16_t icon[18 * 24], folder_icon[18 * 24];
    unsigned msgno = 99;
    SMEM_MapIconToExt("GAME.g3a", u"", &msgno, icon);
    SMEM_MapIconToExt("", u"SAVES", &msgno, folder_icon);
    int magenta = 0, blue = 0, yellow = 0;
    for(int i = 0; i < 18 * 24; i++) {
        magenta += icon[i] == 0xF81F;
        blue += icon[i] == 0x2A7F;
        yellow += folder_icon[i] == 0xFE60;
    }
    results[9] = magenta > 20 && blue > 20 && yellow > 20 && msgno == 0;

    /* 10: App_InitDlgDescriptor's writes */
    static unsigned char desc[0x30];
    memset(desc, 0x55, sizeof desc);
    App_InitDlgDescriptor(desc, 7);
    results[10] = desc[0] == 7 && desc[2] == 0 && desc[0x12] == 0xFF && desc[0x1D] == 0xFF &&
        desc[0x20] == 0 && desc[0x2D] == 0;

    /* 11: a screenshot adds a CAPTnnnn.bmp (showing the two icons) */
    Bdisp_AllClr_VRAM();
    PrintXY(1, 1, "  SMEM_MapIconToExt", 0, 0);
    show_icon(icon, 20, 60);
    show_icon(folder_icon, 80, 60);
    SMEM_MapIconToExt("NOTES.txt", u"", &msgno, icon);
    show_icon(icon, 140, 60);
    SMEM_MapIconToExt("PHOTO.bmp", u"", &msgno, icon);
    show_icon(icon, 200, 60);
    int before = captures();
    TakeScreenshot();
    results[11] = captures() == before + 1;

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\MCSTEST.txt";
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

    /* Last: the calculator restarts, the add-in ends */
    Restart();
    return 0;
}

int main(void)
{
    gint_world_switch(GINT_CALL(run));
    return 1;
}
