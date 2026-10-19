/* TimeKeys: tests the OS timer, RTC, delay and keyboard syscalls
   (emuCasioCalc).

   From the OS world (gint_world_switch), like a libfxcg add-in. Timer
   handlers are plain functions the OS calls like interrupts. If every check
   passes, \\fls0\TIMETEST.txt is written with "ok", else with the first
   failing check.

   Keys (emuCasioCalc: --keys 7,MENU; the screen asks for them): 7 for the
   GetKey with a timer running, MENU for GetKey with the return to the main
   menu disabled.
   Everything else is injected with Keyboard_PutKeycode. */

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

SYSCALL(Bkey_ClrAllFlags, 0x0111);
SYSCALL(Bkey_SetFlag, 0x0112);
SYSCALL(RTC_Reset, 0x02BF);
SYSCALL(RTC_GetTime, 0x02C0);
SYSCALL(RTC_GetTicks, 0x02C1);
SYSCALL(RTC_Elapsed_ms, 0x02C2);
SYSCALL(Timer_Install, 0x08D9);
SYSCALL(Timer_Deinstall, 0x08DA);
SYSCALL(Timer_Start, 0x08DB);
SYSCALL(Timer_Stop, 0x08DC);
SYSCALL(Bkey_GetAllFlags, 0x0EA0);
SYSCALL(Bkey_SetAllFlags, 0x0EA1);
SYSCALL(OS_GetKey, 0x0EAB);
SYSCALL(CMT_Delay_micros, 0x11D6);
SYSCALL(CMT_Delay_100micros, 0x11D7);
SYSCALL(RTC_SetDateTime, 0x11DE);
SYSCALL(GetKeyWait_OS, 0x12BF);
SYSCALL(Keyboard_PutKeycode, 0x12C6);
SYSCALL(Keyboard_SpyMatrixCode, 0x12C9);
SYSCALL(OS_InnerWait_ms, 0x1BB4);
SYSCALL(GetGetkeyToMainFunctionReturnFlag, 0x1E99);
SYSCALL(Bdisp_AllClr_VRAM, 0x0272);
SYSCALL(PrintXY, 0x18F9);
SYSCALL(EnableGetkeyToMainFunctionReturn, 0x1EA6);
SYSCALL(DisableGetkeyToMainFunctionReturn, 0x1EA7);

void Bkey_ClrAllFlags(void);
void Bkey_SetFlag(short flags);
void RTC_Reset(int mode);
void RTC_GetTime(unsigned *hour, unsigned *minute, unsigned *second, unsigned *ms);
int RTC_GetTicks(void);
int RTC_Elapsed_ms(int start, int duration_ms);
int Timer_Install(int id, void (*handler)(void), int elapse);
int Timer_Deinstall(int id);
int Timer_Start(int id);
int Timer_Stop(int id);
short Bkey_GetAllFlags(void);
void Bkey_SetAllFlags(short flags);
int OS_GetKey(int *key);
void CMT_Delay_micros(int us);
void CMT_Delay_100micros(int n);
void RTC_SetDateTime(unsigned char *timestr);
int GetKeyWait_OS(int *column, int *row, int type, int timeout, int menu, unsigned short *keycode);
int Keyboard_PutKeycode(int x, int y, int keycode);
int Keyboard_SpyMatrixCode(char *column, char *row);
void OS_InnerWait_ms(int ms);
int GetGetkeyToMainFunctionReturnFlag(void);
void EnableGetkeyToMainFunctionReturn(void);
void DisableGetkeyToMainFunctionReturn(void);
void Bdisp_AllClr_VRAM(void);
void PrintXY(int x, int y, char const *text, int mode, int color);

/* Asks for a key on screen (GetKey shows VRAM while it waits). PrintXY
   skips the first two characters of its text. */
static void ask(char const *line1, char const *line2)
{
    Bdisp_AllClr_VRAM();
    PrintXY(1, 1, "  TimeKeys", 0, 0);
    PrintXY(1, 3, line1, 0, 0);
    PrintXY(1, 4, line2, 0, 0);
}

#define KEY_CTRL_MENU 30003

#define CHECKS 15
static char const *names[CHECKS] = {
    "Timer_Install", "timer runs", "Timer_Stop", "Timer_Deinstall",
    "CMT_Delay_micros", "CMT_Delay_100us", "RTC_Elapsed_ms", "RTC_SetDateTime",
    "RTC_Reset", "timer in GetKey", "PutKeycode code", "PutKeycode matrix",
    "Bkey flags", "MENU return flag", "timer injects key",
};
static int results[CHECKS];
static int values[CHECKS];

static void check(int i, int ok, int value)
{
    results[i] = ok;
    values[i] = value;
}

static volatile int count;
static void tick(void) { count++; }

static int inject_id;
static void inject_z(void)
{
    Keyboard_PutKeycode(-1, -1, 'Z');
    Timer_Stop(inject_id);
    Timer_Deinstall(inject_id);
}

static int run(void)
{
    /* 0: install in a free user slot; that slot is then taken */
    int id = Timer_Install(0, tick, 100);
    int again = Timer_Install(id, tick, 100);
    check(0, id >= 5 && id <= 10 && again == -1, id);

    /* 1: about every 100 ms (in 40 ms ticks) for one second */
    count = 0;
    Timer_Start(id);
    OS_InnerWait_ms(1000);
    int n = count;
    check(1, n >= 7 && n <= 12, n);

    /* 2: stopped, it doesn't run */
    Timer_Stop(id);
    OS_InnerWait_ms(500);
    check(2, count == n, count - n);

    /* 3: removed: a second removal and a start fail with -2 */
    int d1 = Timer_Deinstall(id), d2 = Timer_Deinstall(id), s = Timer_Start(id);
    check(3, d1 == 0 && d2 == -2 && s == -2, d2);

    /* 4, 5: 50 ms delays move the 1/128 s ticks by about 6 */
    int t0 = RTC_GetTicks();
    CMT_Delay_micros(50000);
    int t1 = RTC_GetTicks();
    CMT_Delay_100micros(500);
    int t2 = RTC_GetTicks();
    check(4, t1 - t0 >= 5 && t1 - t0 <= 8, t1 - t0);
    check(5, t2 - t1 >= 5 && t2 - t1 <= 8, t2 - t1);

    /* 6: RTC_Elapsed_ms */
    int start = RTC_GetTicks();
    int e1 = RTC_Elapsed_ms(start, 100);
    CMT_Delay_micros(120000);
    int e2 = RTC_Elapsed_ms(start, 100);
    check(6, e1 == 0 && e2 == 1, e1 * 10 + e2);

    /* 7: set the clock to 2008-03-02 19:47:03 */
    unsigned char when[7] = { 0x20, 0x08, 0x03, 0x02, 0x19, 0x47, 0x03 };
    RTC_SetDateTime(when);
    unsigned h = 99, m = 99, sec = 99, ms = 0;
    RTC_GetTime(&h, &m, &sec, &ms);
    check(7, h == 19 && m == 47 && sec == 3, h * 100 + m);

    /* 8: reset to zero */
    RTC_Reset(1);
    RTC_GetTime(&h, &m, &sec, &ms);
    check(8, h == 0 && m == 0 && sec <= 1, h * 100 + m);

    /* 9: a running timer is called while GetKey waits (key: 7) */
    id = Timer_Install(0, tick, 40);
    count = 0;
    Timer_Start(id);
    ask("  Press 7", "  (a timer runs)");
    int key = 0;
    OS_GetKey(&key);
    Timer_Stop(id);
    Timer_Deinstall(id);
    check(9, key == '7' && count >= 1, count);

    /* 10: a GetKey code put in the buffer */
    Keyboard_PutKeycode(-1, -1, 'Q');
    key = 0;
    OS_GetKey(&key);
    check(10, key == 'Q', key);

    /* 11: a matrix code (EXE: column 3, row 2), seen by SpyMatrixCode
       then read by GetKeyWait_OS */
    Keyboard_PutKeycode(3, 2, 0);
    char sc = 0, sr = 0;
    int waiting = Keyboard_SpyMatrixCode(&sc, &sr);
    int col = 0, row = 0;
    unsigned short kc = 0;
    int rc = GetKeyWait_OS(&col, &row, 0, 0, 0, &kc);
    check(11, waiting == 1 && sc == 3 && sr == 2 && rc == 1 && col == 3 && row == 2, waiting);

    /* 12: Bkey flags */
    Bkey_ClrAllFlags();
    Bkey_SetFlag(0x11);
    short f1 = Bkey_GetAllFlags();
    Bkey_SetAllFlags(0x80);
    short f2 = Bkey_GetAllFlags();
    Bkey_ClrAllFlags();
    check(12, f1 == 0x11 && f2 == 0x80, f1);

    /* 13: with the return to the main menu off, GetKey gives MENU (key) */
    int on = GetGetkeyToMainFunctionReturnFlag();
    DisableGetkeyToMainFunctionReturn();
    int off = GetGetkeyToMainFunctionReturnFlag();
    ask("  Press MENU", "  (PC: Home)");
    key = 0;
    OS_GetKey(&key);
    EnableGetkeyToMainFunctionReturn();
    check(13, on == 1 && off == 0 && key == KEY_CTRL_MENU, key);

    /* 14: a timer handler injects a key while GetKey waits (as gint does
       to reach the main menu): GetKey returns it */
    inject_id = Timer_Install(0, inject_z, 0);
    Timer_Start(inject_id);
    key = 0;
    OS_GetKey(&key);
    check(14, key == 'Z', key);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\TIMETEST.txt";
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
    row_title("TimeKeys");
    for(int k = 0; k < CHECKS; k++) {
        int row = k % 8 + 2, x = 1 + k / 8 * 24;
        if(results[k])
            row_print_color(row, x, C_RGB(0, 20, 0), C_WHITE, "%-17s ok", names[k]);
        else
            row_print_color(row, x, C_RED, C_WHITE, "%-17s %d", names[k], values[k]);
    }
    dupdate();

    while(getkey().key != KEY_EXIT) {}
    return 1;
}
