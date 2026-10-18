#ifndef EMU_EMU_H
#define EMU_EMU_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "mem.h"
#include "r61524.h"
#include "sh7305.h"

/* The whole calculator: CPU, memory, peripherals, display, plus the state
 * of our high-level OS (heap) and of the scripted keys. */
typedef struct {
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
    const char *data_dir; /* host folder for main memory (MCS) files: ./storage */
} Emu;

/* Builds the memory map and peripherals. */
bool emu_init(Emu *emu);
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
