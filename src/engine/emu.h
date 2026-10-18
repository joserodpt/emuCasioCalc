#ifndef EMU_EMU_H
#define EMU_EMU_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"
#include "mem.h"
#include "r61524.h"
#include "sh7305.h"

/* The calculators we can be. Same CPU, display and add-in format; they
 * differ in where RAM is, how much there is, and the clock. */
typedef enum {
    MODEL_CG50,  /* fx-CG50 (2017) */
    MODEL_CG20,  /* fx-CG10 / fx-CG20 ("Prizm", 2011) */
} Model;

const char *model_name(Model model);

/* Where things are on a model. Addresses are what the CPU sees. */
typedef enum { ALPHA_OFF, ALPHA_ONCE, ALPHA_LOCK } KeyAlpha;

/* The main battery (GetMainBatteryVoltage), in 1/100 V: four AAA cells.
 * Emulation > Battery and --battery choose one of these levels. The
 * thresholds of the status bar's 3 bars are our guess. */
#define BATTERY_CENTIVOLTS 520
#define BATTERY_LEVELS 4
static const int BATTERY_LEVEL_CV[BATTERY_LEVELS] = {520, 480, 440, 400}; /* full, half, low, empty */
static inline int battery_bars(int cv)
{
    return cv >= 500 ? 3 : cv >= 460 ? 2 : cv >= 420 ? 1 : 0;
}

/* A glyph of the status area (DefineStatusGlyph). */
typedef struct {
    int dx, dy, xalign, xoffset;
    uint32_t bitmap; /* RGB565, dx * dy */
} StatusGlyph;

typedef struct {
    uint32_t ram_phys, ram_size;   /* physical RAM */
    uint32_t uram_phys;            /* add-in static RAM behind URAM_BASE */
    uint32_t vram;                 /* GetVRAMAddress, at the start of RAM */
    uint32_t vram2;                /* GetSecondaryVRAMAddress (SaveVRAM_1), after it */
    uint32_t stack_top;            /* r15 when the add-in starts */
    uint32_t heap_base, heap_size; /* our sys_malloc arena */
    uint32_t frqcr;                /* clock setup the OS leaves (CPG FRQCR) */
} Layout;

const Layout *model_layout(Model model);

/* The whole calculator: CPU, memory, peripherals, display, plus the state
 * of our high-level OS (heap) and of the scripted keys. */
typedef struct {
    Model model;
    const Layout *layout;
    Cpu cpu;
    Memory mem;
    Sh7305 hw;
    R61524 lcd;
    uint8_t *rom, *ram;
    uint8_t *vram;        /* OS VRAM (GetVRAMAddress), in RAM */
    uint8_t *vram2;       /* secondary VRAM (SaveVRAM_1 / LoadVRAM_1), in RAM */
    bool heap_ready;      /* the sys_malloc heap has its first header */
    bool trace;           /* log every syscall to stdout */
    /* Realistic timing: syscalls, uncached memory and DMA transfers take the
     * time they would on the calculator. Off ("fast"): they take none. */
    bool realistic;
    bool headless;

    /* --keys: GetKey pops them; for add-ins that scan the keyboard
     * themselves (gint), each key is held down for a while in emulated time. */
    int script[256];      /* indices into KEYS */
    int script_len, script_pos;
    bool script_down;
    uint64_t script_next; /* cycle of the next press/release, 0 = not started */
    uint64_t script_done; /* cycle when the last scripted key was released */
    bool polls_keys;      /* the add-in reads the key state (PRGM_GetKey...) */
    bool key_down[256];   /* KEYS currently held (host keyboard + script) */
    /* GetKey's modifiers: SHIFT pressed, ALPHA for one key or locked. */
    bool key_shift;
    KeyAlpha key_alpha;
    int battery_cv;         /* battery voltage, 1/100 V */

    /* The OS status area, the top 24 rows of VRAM (syscalls_status.c). */
    bool status_off;        /* EnableStatusArea(3) */
    uint32_t status_flags;  /* SAF_* */
    uint8_t status_color[2]; /* header colours, PrintXY indices */
    char status_msg[64];    /* DefineStatusMessage */
    uint8_t status_msg_color;
    StatusGlyph status_glyph[4];
    uint32_t getkey_status; /* EnableDisplayHeader: GetKey draws the area */

    /* OS state for the HLE syscalls. */
    uint8_t setup[256];   /* GetSetupSetting / SetSetupSetting */
    uint8_t *msgbox[8];   /* VRAM saved by MsgBoxPush */
    int msgbox_depth;
    char mcs_path[600];   /* item selected by MCSGetDlen2, for MCSGetData1 */
    const char *data_dir; /* host folder for saved data: ./storage (mcs/, fls0/) */
    FILE *files[16];      /* open Bfile handles */
    int file_flash[16];   /* their flash_files index, -1 if not copied */
    /* Bfile_FindFirst searches: the matching names, sorted, folders first. */
    struct {
        bool used;
        char dir[600];    /* host folder searched */
        char **names;
        int count, pos;
    } finds[4];
    bool serial_open;     /* Serial_Open was called (no cable: nothing arrives) */
    int cursor_x, cursor_y; /* locate_OS: PrintLine's position, in PrintXY cells */
    char app_name[12];      /* GetAppName: the add-in's internal name (@NAME) */
    /* System settings add-ins can read and change (calculator defaults). */
    int battery_type;       /* 1 alkaline, 2 Ni-MH */
    int backlight_halfmin;  /* backlight duration, half-minutes */
    int power_off_min;      /* auto power off, minutes */

    /* Storage files copied into the flash (ROM), as on the calculator where
     * they live there: some add-ins (CGDoom) read them from flash directly. */
    struct {
        char path[700];
        uint32_t offset, size;
        int64_t mtime;
    } flash_files[16];
    int flash_file_count;
    uint32_t flash_next;  /* next free ROM offset for a file */
} Emu;

/* Builds the memory map and peripherals of a model. */
bool emu_init(Emu *emu, Model model);
void emu_free(Emu *emu);
/* Switches between realistic and fast timing (can change while running). */
void emu_set_realistic(Emu *emu, bool realistic);
/* Resets the CPU to the add-in's entry point, like the OS launching it. */
void emu_start(Emu *emu);

bool emu_parse_keys(Emu *emu, const char *list);
/* GetKey: next scripted key, else waits for the host keyboard.
 * Returns a KEYS index, or -1 when there is nothing left (headless) or the
 * window was closed. */
int emu_next_key(Emu *emu);
/* Feeds the key matrix with host-held keys and the key script. */
void emu_update_keys(Emu *emu, const bool *host_down);
/* Headless runs of keyboard-scanning add-ins end this long after the script. */
bool emu_script_finished(const Emu *emu, uint64_t after_ms);

/* Shows the panel if it changed. */
void emu_present(Emu *emu);

#endif
