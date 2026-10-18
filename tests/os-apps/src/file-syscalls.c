/* FileSyscalls: tests the storage file syscalls (emuCasioFxCG).

   From the OS world (gint_world_switch), like a libfxcg add-in: makes a
   folder \\fls0\FTEST with a few files, lists it with Bfile_FindFirst /
   FindNext, and checks sizes, names, masks, free space and flash addresses.
   Everything is removed at the end. If every check passes,
   \\fls0\FILETEST.txt is written with "ok", else with the first failing
   check. No keys are needed until the result screen (EXIT). */

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

SYSCALL(Bfile_GetMediaFree_OS, 0x1DA5);
SYSCALL(Bfile_GetFileSize_OS, 0x1DA6);
SYSCALL(Bfile_GetBlockAddress, 0x1DAA);
SYSCALL(Bfile_FindFirst, 0x1DB7);
SYSCALL(Bfile_FindNext, 0x1DB9);
SYSCALL(Bfile_FindClose, 0x1DBA);
SYSCALL(Bfile_Name_MatchMask, 0x1DDA);
SYSCALL(Bfile_NameToStr_ncpy, 0x1DDB);

typedef struct {
    unsigned short id, type;
    unsigned long fsize, dsize;
    unsigned int property;
    unsigned long address;
} file_type_t;

int Bfile_GetMediaFree_OS(uint16_t const *media, int *freespace);
int Bfile_GetFileSize_OS(int handle);
int Bfile_GetBlockAddress(int handle, int offset, unsigned char **ptr);
int Bfile_FindFirst(uint16_t const *pattern, int *handle, uint16_t *found, file_type_t *info);
int Bfile_FindNext(int handle, uint16_t *found, file_type_t *info);
int Bfile_FindClose(int handle);
int Bfile_Name_MatchMask(uint16_t const *mask, uint16_t const *name);
void Bfile_NameToStr_ncpy(char *dest, uint16_t const *source, int n);

static uint16_t const DIR[]   = u"\\\\fls0\\FTEST";
static uint16_t const SUB[]   = u"\\\\fls0\\FTEST\\SUB";
static uint16_t const FA[]    = u"\\\\fls0\\FTEST\\A.txt";
static uint16_t const FB[]    = u"\\\\fls0\\FTEST\\B.txt";
static uint16_t const FC[]    = u"\\\\fls0\\FTEST\\C.dat";
static uint16_t const FBIG[]  = u"\\\\fls0\\FTEST\\BIG.bin";

#define CHECKS 12
static char const *names[CHECKS] = {
    "find *.txt", "find next", "find end", "find * (all)",
    "find no match", "find *.TXT", "GetFileSize", "NameToStr",
    "MatchMask", "GetMediaFree", "BlockAddress", "4 handles",
};
static int results[CHECKS];
static int values[CHECKS];

static void check(int i, int ok, int value)
{
    results[i] = ok;
    values[i] = value;
}

static int same(uint16_t const *a, char const *b)
{
    while(*b) if(*a++ != (uint8_t)*b++) return 0;
    return *a == 0;
}

static void make_file(uint16_t const *path, char const *data, int size)
{
    BFile_Remove(path);
    BFile_Create(path, BFile_File, &size);
    int fd = BFile_Open(path, BFile_WriteOnly);
    if(fd >= 0) {
        BFile_Write(fd, data, size);
        BFile_Close(fd);
    }
}

static int pattern_count(uint16_t const *pattern)
{
    int h = -1, n = 0;
    uint16_t found[64];
    file_type_t info;
    int rc = Bfile_FindFirst(pattern, &h, found, &info);
    while(rc == 0) {
        n++;
        rc = Bfile_FindNext(h, found, &info);
    }
    Bfile_FindClose(h);
    return n;
}

static int run(void)
{
    int size = 0;
    BFile_Create(DIR, BFile_Folder, &size);
    BFile_Create(SUB, BFile_Folder, &size);
    make_file(FA, "AAAA", 4);
    make_file(FB, "BBBBBB", 6);
    make_file(FC, "ABCDEFGH", 8);

    uint16_t found[64];
    file_type_t info;
    int h = -1;

    /* 0-2: *.txt gives A.txt then B.txt, then the end (-16) */
    int rc = Bfile_FindFirst(u"\\\\fls0\\FTEST\\*.txt", &h, found, &info);
    check(0, rc == 0 && same(found, "A.txt") && info.type == 1 && info.fsize == 4, rc);
    rc = Bfile_FindNext(h, found, &info);
    check(1, rc == 0 && same(found, "B.txt") && info.fsize == 6, rc);
    rc = Bfile_FindNext(h, found, &info);
    int rc2 = Bfile_FindClose(h);
    check(2, rc == -16 && rc2 == 0, rc);

    /* 3: everything, the folder first */
    rc = Bfile_FindFirst(u"\\\\fls0\\FTEST\\*", &h, found, &info);
    int sub_first = rc == 0 && same(found, "SUB") && info.type == 0;
    Bfile_FindClose(h);
    int n = pattern_count(u"\\\\fls0\\FTEST\\*");
    check(3, sub_first && n == 4, n);

    /* 4: no match: an error, and the handle still closes */
    h = -1;
    rc = Bfile_FindFirst(u"\\\\fls0\\FTEST\\*.zip", &h, found, &info);
    rc2 = Bfile_FindClose(h);
    check(4, rc < 0 && rc2 == 0, rc);

    /* 5: case-insensitive */
    n = pattern_count(u"\\\\fls0\\FTEST\\*.TXT");
    check(5, n == 2, n);

    /* 6: size of an open file */
    int fd = BFile_Open(FC, BFile_ReadOnly);
    int fsize = Bfile_GetFileSize_OS(fd);
    check(6, fsize == 8, fsize);

    /* 10: its 4 kB block in flash holds the data */
    unsigned char *p = 0;
    rc = Bfile_GetBlockAddress(fd, 0, &p);
    check(10, rc == 0 && p && !memcmp(p, "ABCDEFGH", 8), rc);
    BFile_Close(fd);

    /* 7: 16-bit name to 8-bit string */
    char str[16];
    memset(str, 'x', sizeof str);
    Bfile_NameToStr_ncpy(str, u"HELLO", 16);
    check(7, !strcmp(str, "HELLO"), str[0]);

    /* 8: masks */
    int m1 = Bfile_Name_MatchMask(u"*.g3a", u"Game.G3A");
    int m2 = Bfile_Name_MatchMask(u"a?c", u"abd");
    int m3 = Bfile_Name_MatchMask(u"a?c", u"abc");
    check(8, m1 == 1 && m2 == 0 && m3 == 1, m1 * 100 + m2 * 10 + m3);

    /* 9: free space shrinks by what a new file takes */
    int free1 = 0, free2 = 0;
    Bfile_GetMediaFree_OS(u"\\\\fls0", &free1);
    static char big[4096];
    make_file(FBIG, big, sizeof big);
    Bfile_GetMediaFree_OS(u"\\\\fls0", &free2);
    check(9, free1 > 0 && free1 <= 16 << 20 && free1 - free2 >= 4096, (free1 - free2));

    /* 11: four searches at once, a fifth fails */
    int hs[5], ok = 1;
    for(int i = 0; i < 5; i++) {
        hs[i] = -1;
        rc = Bfile_FindFirst(u"\\\\fls0\\FTEST\\*", &hs[i], found, &info);
        if(i < 4 && rc != 0) ok = 0;
        if(i == 4 && rc == 0) ok = 0;
    }
    for(int i = 0; i < 4; i++)
        if(Bfile_FindClose(hs[i]) != 0) ok = 0;
    check(11, ok, rc);

    BFile_Remove(FA);
    BFile_Remove(FB);
    BFile_Remove(FC);
    BFile_Remove(FBIG);
    BFile_Remove(SUB);
    BFile_Remove(DIR);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\FILETEST.txt";
    char text[32] = "ok";
    if(failed >= 0) {
        strcpy(text, "fail: ");
        strcat(text, names[failed]);
    }
    int tsize = (strlen(text) + 1) & ~1;
    make_file(path, text, tsize);
    return 0;
}

int main(void)
{
    gint_world_switch(GINT_CALL(run));

    dclear(C_WHITE);
    row_title("FileSyscalls");
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
