/* OsSyscalls: tests OS system-info and dialog syscalls (emuCasioFxCG).

   Each syscall is called from the OS world (gint_world_switch), like a
   libfxcg add-in would. Results are shown on screen; if every check passes,
   \\fls0\OSTEST.txt is written with "ok", else with the first failing check.

   The dialogs need keys, in this order (emuCasioFxCG: --keys ...):
     DOWN,DOWN,EXE      ColorIndexDialog1 -> 2 (3rd colour)
     DOWN,EXE           same, colour 2 disabled -> skips to 2 again
     EXIT               same, cancelled -> 0xFF
     EXIT               DisplayMessageBox closes */

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

SYSCALL(GlibGetOSVersionInfo, 0x002B);
SYSCALL(GetAppName, 0x1E9F);
SYSCALL(GetMainBatteryVoltage, 0x1186);
SYSCALL(SetBatteryType, 0x12D4);
SYSCALL(GetBatteryType, 0x12D5);
SYSCALL(SetBacklightDuration, 0x12D8);
SYSCALL(GetBacklightDuration, 0x12D9);
SYSCALL(SetAutoPowerOffTime, 0x1E90);
SYSCALL(GetAutoPowerOffTime, 0x1E91);
SYSCALL(GetStackPtr, 0x1A2C);
SYSCALL(App_Optimize, 0x1630);
SYSCALL(HourGlass, 0x02C7);
SYSCALL(ColorIndexDialog1, 0x1815);
SYSCALL(DisplayMessageBox, 0x1E4B);
SYSCALL(ProgressBar2, 0x1809);
SYSCALL(MsgBoxPop, 0x17F9);
SYSCALL(BoxYLimits, 0x17FB);
SYSCALL(BoxInnerClear, 0x17FD);
SYSCALL(MsgBoxPush, 0x17F7);
SYSCALL(Scrollbar, 0x1A0A);

int GlibGetOSVersionInfo(char *major, char *minor, short *c, short *d);
char *GetAppName(char *name);
int GetMainBatteryVoltage(int one);
void SetBatteryType(int type);
int GetBatteryType(void);
void SetBacklightDuration(char half_minutes);
char GetBacklightDuration(void);
void SetAutoPowerOffTime(int minutes);
int GetAutoPowerOffTime(void);
void *GetStackPtr(void);
void App_Optimize(void);
void HourGlass(void);
unsigned char ColorIndexDialog1(unsigned char initial, unsigned short disable_mask);
void DisplayMessageBox(unsigned char *message);
void ProgressBar2(unsigned char *heading, int current, int max);
void MsgBoxPop(void);
void BoxYLimits(int lines, int *top, int *bottom);
void BoxInnerClear(int lines);
void MsgBoxPush(int lines);

struct scrollbar {
    unsigned int I1, indicatormaximum, indicatorheight, indicatorpos, I5;
    unsigned short barleft, bartop, barheight, barwidth;
};
void Scrollbar(struct scrollbar *scrollbar);

#define CHECKS 14
static char const *names[CHECKS] = {
    "OS version", "app name", "battery voltage", "battery type",
    "backlight", "auto power off", "stack pointer", "colour dialog",
    "colour, disabled", "colour, EXIT", "message box", "progress bar",
    "box limits", "scrollbar",
};
static int results[CHECKS];
static int values[CHECKS];
static char app_name[16];

static void check(int i, int ok, int value)
{
    results[i] = ok;
    values[i] = value;
}

static int run(void)
{
    char major = 0, minor = 0;
    short c = 0, d = 0;

    GlibGetOSVersionInfo(&major, &minor, &c, &d);
    check(0, major == 3 || major == 2, major);

    char *p = GetAppName(app_name);
    check(1, p == app_name && app_name[0] == '@', app_name[1]);

    int mv = GetMainBatteryVoltage(1);
    check(2, mv > 300 && mv < 700, mv);

    SetBatteryType(2);
    int type = GetBatteryType();
    check(3, type == 2, type);

    SetBacklightDuration(10);
    int light = GetBacklightDuration();
    check(4, light == 10, light);

    SetAutoPowerOffTime(30);
    int off = GetAutoPowerOffTime();
    check(5, off == 30, off);

    unsigned int sp = (unsigned int)GetStackPtr();
    check(6, sp >= 0x88000000 && sp < 0x8d000000, (int)(sp >> 16));

    App_Optimize();
    HourGlass();

    int col = ColorIndexDialog1(0, 0);
    check(7, col == 2, col);
    col = ColorIndexDialog1(0, 0x0002);
    check(8, col == 2, col);
    col = ColorIndexDialog1(0, 0);
    check(9, col == 0xFF, col);

    DisplayMessageBox((unsigned char *)"Test message\nsecond line");
    check(10, 1, 0); /* it returned */

    ProgressBar2((unsigned char *)"Loading", 0, 10);
    ProgressBar2((unsigned char *)"Loading", 5, 10);
    ProgressBar2((unsigned char *)"Loading", 10, 10);
    MsgBoxPop();
    check(11, 1, 0);

    int top = 0, bottom = 0;
    BoxYLimits(3, &top, &bottom);
    MsgBoxPush(3);
    BoxInnerClear(3);
    MsgBoxPop();
    check(12, top > 0 && bottom > top, bottom - top);

    struct scrollbar sb = { 0, 100, 20, 40, 0, 370, 10, 150, 6 };
    Scrollbar(&sb);
    check(13, 1, 0);

    /* Marker for automated runs */
    int failed = -1;
    for(int k = 0; k < CHECKS; k++)
        if(!results[k] && failed < 0) failed = k;
    static uint16_t const path[] = u"\\\\fls0\\OSTEST.txt";
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
    row_title("OsSyscalls");
    for(int k = 0; k < CHECKS; k++) {
        row_print_color(k + 2, 1, results[k] ? C_RGB(0, 20, 0) : C_RED, C_WHITE,
            "%-17s %s  %d", names[k], results[k] ? "ok" : "FAIL", values[k]);
    }
    dupdate();

    while(getkey().key != KEY_EXIT) {}
    return 1;
}
