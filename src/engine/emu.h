#ifndef EMU_EMU_H
#define EMU_EMU_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu.h"
#include "mem.h"
#include "r61524.h"
#include "sh7305.h"
#include "t6k11.h"

/* The calculators we can be. All have the SH7305 (SH4AL-DSP).
 * - Colour (.g3a add-ins, 396x224 R61524 display): they differ in where RAM
 *   is, how much there is, and the clock.
 * - Monochrome (.g1a add-ins, 128x64 T6K11 display, 512 kB of RAM, gint
 *   add-ins only): the fx-9860GII and the newer fx-9750GIII, whose display
 *   takes other commands and whose OS (3.x) gint tells by its version. */
typedef enum {
    MODEL_CG50,     /* fx-CG50 (2017) */
    MODEL_CG20,     /* fx-CG10 / fx-CG20 ("Prizm", 2011) */
    MODEL_FX9860G2, /* fx-9860GII SH4 / Graph 75+ (2011) */
    MODEL_FX9750G3, /* fx-9750GIII / fx-9860GIII / Graph 35+E II (2019) */
} Model;

const char *model_name(Model model);
static inline bool model_mono(Model model)
{
    return model == MODEL_FX9860G2 || model == MODEL_FX9750G3;
}

/* Where things are on a model. Addresses are what the CPU sees. */
typedef enum { ALPHA_OFF, ALPHA_ONCE, ALPHA_LOCK } KeyAlpha;

/* The OS's message languages (System > Language), in its order. */
enum { LANG_EN, LANG_ES, LANG_DE, LANG_FR, LANG_PT, LANG_ZH, LANG_COUNT };
static const char *const LANG_CODES[LANG_COUNT] = {"en", "es", "de", "fr", "pt", "zh"};
static const char *const LANG_NAMES[LANG_COUNT] = {"English", "Espanol", "Deutsch", "Francais", "Portugues", "Chinese"};

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
/* A step of the key script. */
typedef enum { STEP_KEY, STEP_WAIT, STEP_SHOT } StepKind;
typedef struct {
    StepKind kind;
    int key;      /* STEP_KEY: index into KEYS */
    uint32_t ms;  /* STEP_WAIT */
    int path;     /* STEP_SHOT: offset in script_paths */
    int scale;    /* STEP_SHOT */
} ScriptStep;

typedef struct {
    int dx, dy, xalign, xoffset;
    uint32_t bitmap; /* RGB565, dx * dy */
} StatusGlyph;

typedef struct {
    uint32_t ram_phys, ram_size;   /* physical RAM */
    uint32_t uram_phys, uram_size; /* add-in static RAM behind URAM_BASE */
    uint32_t vram;                 /* GetVRAMAddress, at the start of RAM */
    uint32_t vram2;                /* GetSecondaryVRAMAddress (SaveVRAM_1), after it */
    uint32_t stack_top;            /* r15 when the add-in starts */
    uint32_t heap_base, heap_size; /* our sys_malloc arena */
    uint32_t mcs;                  /* MCS_GetMainMemoryStart: unused RAM (no MCS image there) */
    uint32_t frqcr;                /* clock setup the OS leaves (CPG FRQCR) */
    const char *os_version;        /* at 0x80010020 (monochrome models: gint reads it) */
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
    R61524 lcd;           /* colour models' display */
    T6k11 mono;           /* monochrome models' display */
    uint16_t mono_fb[MONO_WIDTH * MONO_HEIGHT]; /* its last render, RGB565 */
    bool mono_shown;      /* mono_fb went to the window once */
    uint32_t entry;       /* where the add-in starts */
    uint8_t *rom, *ram;
    uint8_t *vram;        /* OS VRAM (GetVRAMAddress), in RAM */
    uint8_t *vram2;       /* secondary VRAM (SaveVRAM_1 / LoadVRAM_1), in RAM */
    bool heap_ready;      /* the sys_malloc heap has its first header */
    bool trace;           /* log every syscall to stdout */
    /* Realistic timing: syscalls, uncached memory and DMA transfers take the
     * time they would on the calculator. Off ("fast"): they take none. */
    bool realistic;
    bool headless;

    /* --keys / --script: keys, waits and screenshots, in order, in emulated
     * time. GetKey takes the keys; for add-ins that scan the keyboard
     * themselves (gint), each key is held down for a while. Waits and
     * screenshots don't need the add-in: they happen when their time comes. */
    ScriptStep script[512];
    int script_len, script_pos;
    char script_paths[8192]; /* the screenshots' file names */
    int script_paths_len;
    int shot_scale;       /* the screenshots' scale (script: scale N) */
    bool script_down;     /* a scripted key is held in the matrix */
    uint64_t script_next; /* cycle the next step may start, 0 = not started */
    uint64_t script_done; /* cycle when the script ended */
    bool script_failed;   /* a screenshot couldn't be saved */
    bool script_settling; /* GetKey took the last key: the script waits for it to be back */
    bool polls_keys;      /* the add-in read the key state (PRGM_GetKey...) since the last update */
    bool key_down[256];   /* KEYS currently held (host keyboard + script) */
    /* GetKey's modifiers: SHIFT pressed, ALPHA for one key or locked. */
    bool key_shift;
    KeyAlpha key_alpha;
    int battery_cv;         /* battery voltage, 1/100 V */
    int language;           /* LANG_*: the OS's message language */

    /* OS timers (Timer_Install...), slots 1-10; 5-10 are the add-in's. */
    struct {
        bool installed, running;
        uint32_t handler;
        uint32_t period_ms;  /* in whole 25 Hz ticks */
        uint64_t next;       /* cycle of the next call */
    } timers[11];
    bool in_timer;          /* a handler is running: no nested calls */

    /* Keyboard_PutKeycode: keys injected for the next reads. key is a KEYS
     * index (a matrix code, for every reader), or -1 with a GetKey code. */
    struct {
        int key, code;
    } inject[8];
    int inject_count;
    uint32_t inject_serial;  /* counts injections, to notice new ones */
    uint16_t bkey_flags;     /* Bkey_SetFlag / GetAllFlags... */
    bool menu_return_off;    /* DisableGetkeyToMainFunctionReturn */

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
/* The calculator's clock starts at unix_time (local time) instead of now,
 * then follows the emulated time: runs show the same date and time. */
void emu_set_clock(int64_t unix_time);
void emu_free(Emu *emu);
/* Switches between realistic and fast timing (can change while running). */
void emu_set_realistic(Emu *emu, bool realistic);
/* Resets the CPU to the add-in's entry point, like the OS launching it. */
void emu_start(Emu *emu);

/* Adds the steps of a script: key names (EXE, F1, 7...), "wait N" (ms, or
 * "2s"), "shot FILE" (.png or .bmp, at the scale set by "scale N", 2 by
 * default), separated by spaces, commas or lines; "#" starts a comment and
 * "quotes" keep a file name with spaces together. */
bool emu_parse_script(Emu *emu, const char *text);
/* GetKey: next scripted key, else waits for the host keyboard.
 * Returns a KEYS index, -1 when there is nothing left (headless) or the
 * window was closed, or -3 when an OS timer injected a key meanwhile
 * (Keyboard_PutKeycode: read emu->inject). */
int emu_next_key(Emu *emu);
/* The add-in has ended: the script's screenshots and waits left happen on
 * its last screen (waits make a recording's last frame last). Returns the
 * number of keys left unused. */
int emu_finish_script(Emu *emu);
/* Runs the add-in for cycles of time, with the OS timers due meanwhile. */
void emu_run(Emu *emu, uint64_t cycles);
/* Feeds the key matrix with host-held keys and the key script. */
void emu_update_keys(Emu *emu, const bool *host_down);
/* Headless runs of keyboard-scanning add-ins end this long after the script
 * (its keys, waits and screenshots). */
bool emu_script_finished(const Emu *emu, uint64_t after_ms);

/* Shows the panel if it changed (and tells the window the emulated time). */
void emu_present(Emu *emu);
/* The screen as it is now (RGB565, *w x *h): the colour panel, or the
 * monochrome one rendered. */
const uint16_t *emu_screen(Emu *emu, int *w, int *h);

#endif
