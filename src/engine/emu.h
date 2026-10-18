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
typedef struct {
    uint32_t ram_phys, ram_size;   /* physical RAM */
    uint32_t uram_phys;            /* add-in static RAM behind URAM_BASE */
    uint32_t vram;                 /* GetVRAMAddress, at the start of RAM */
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
    uint32_t heap_next;
    bool trace;           /* log every syscall to stdout */
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

    /* OS state for the HLE syscalls. */
    uint8_t setup[256];   /* GetSetupSetting / SetSetupSetting */
    uint8_t *msgbox[8];   /* VRAM saved by MsgBoxPush */
    int msgbox_depth;
    char mcs_path[600];   /* item selected by MCSGetDlen2, for MCSGetData1 */
    const char *data_dir; /* host folder for saved data: ./storage (mcs/, fls0/) */
    FILE *files[16];      /* open Bfile handles */
    uint8_t *saved_vram;  /* SaveVRAM_1 / LoadVRAM_1 */
} Emu;

/* Builds the memory map and peripherals of a model. */
bool emu_init(Emu *emu, Model model);
void emu_free(Emu *emu);
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
