#include "emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "keys.h"
#include "platform.h"
#include "syscalls.h"

#define KEY_START_MS 300  /* let a keyboard-scanning add-in settle first */
#define KEY_HOLD_MS  100
#define KEY_GAP_MS   100

/*
 * fx-CG50: 8 MB of RAM at 0x0C000000, VRAM at its start (0xAC000000). The
 * stack is in P1 above 0x8C000000 (gint checks this to tell the model) and
 * above the 350 kB gint borrows at 0x8C0F0000.
 *
 * fx-CG10/20: 2 MB of RAM at 0x08000000, VRAM at its start (0xA8000000);
 * gint borrows 0x880F0000, the add-in's static RAM is the last 512 kB.
 *
 * Clocks: the fx-CG50 runs the CPU at 117.96 MHz (Pphi 29.49 MHz); the
 * fx-CG10/20 at half that, 58.98 MHz (Pphi 14.75 MHz), from the same PLL
 * with bigger dividers.
 */
/* Uncached accesses (P2: the OS's VRAM at 0xAC000000 / 0xA8000000) go to
 * the RAM bus, which runs at a fraction of the CPU clock: extra cycles per
 * access. Estimates (no published figures); writes are partly absorbed by
 * the write buffer, reads stall fully. */
#define UNCACHED_READ_CYCLES  12
#define UNCACHED_WRITE_CYCLES 4

static const Layout LAYOUTS[] = {
    [MODEL_CG50] = {
        .ram_phys = 0x0C000000u, .ram_size = 0x00800000u,
        .uram_phys = 0x0C200000u,
        .vram = P2(0x0C000000u),
        .stack_top = P1(0x0C180000u),
        .heap_base = P1(0x0C400000u), .heap_size = 0x00400000u,
        .frqcr = 0x0F011112u,
    },
    [MODEL_CG20] = {
        .ram_phys = 0x08000000u, .ram_size = 0x00200000u,
        .uram_phys = 0x08180000u,
        .vram = P2(0x08000000u),
        .stack_top = P1(0x08170000u),
        .heap_base = P1(0x08030000u), .heap_size = 0x000C0000u,
        .frqcr = 0x0F102203u,
    },
};

const char *model_name(Model model)
{
    return model == MODEL_CG20 ? "fx-CG10/20" : "fx-CG50";
}

const Layout *model_layout(Model model)
{
    return &LAYOUTS[model];
}

bool emu_init(Emu *emu, Model model)
{
    Memory *mem = &emu->mem;
    const Layout *l = model_layout(model);
    emu->model = model;
    emu->layout = l;
    if (!mem_init(mem))
        return false;
    mem->lazy_io = true;


    emu->rom = mem_alloc(mem, ROM_SIZE);
    emu->ram = mem_alloc(mem, l->ram_size);
    if (!emu->rom || !emu->ram)
        return false;
    mem_map_phys(mem, ROM_PHYS, ROM_SIZE, emu->rom, false);
    mem_map_phys(mem, l->ram_phys, l->ram_size, emu->ram, true);
    /* Add-in static RAM: a U0 view of part of the physical RAM. */
    mem_map(mem, URAM_BASE, URAM_SIZE, emu->ram + (l->uram_phys - l->ram_phys), true);
    /* On-chip memories: IL RAM, X/Y RAM, RS memory. */
    if (!mem_add_ram(mem, 0xE5200000u, 0x4000, true) ||
        !mem_add_ram(mem, 0xE5000000u, 0x20000, true) ||
        !mem_add_ram(mem, 0xFD800000u, 0x1000, true))
        return false;

    r61524_reset(&emu->lcd);
    if (!sh7305_init(&emu->hw, &emu->cpu, mem, &emu->lcd, (int64_t)time(NULL), l->frqcr))
        return false;
    sh7305_add_mapping(&emu->hw, URAM_BASE, l->uram_phys, URAM_SIZE);

    emu->vram = emu->ram + ((l->vram & 0x1FFFFFFFu) - l->ram_phys);
    /* Add-ins written for the fx-CG10/20 hard-code VRAM at 0xA8000000; on
     * the fx-CG50, show those writes too by aliasing that address. */
    if (model == MODEL_CG50)
        mem_map_phys(mem, model_layout(MODEL_CG20)->ram_phys, (VRAM_SIZE + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                     emu->vram, true);
    emu->data_dir = "storage"; /* main memory files, next to where we run */
    emu_set_realistic(emu, true);
    emu->cpu.event = sh7305_event;
    emu->cpu.event_user = &emu->hw;
    syscalls_install(emu);
    return true;
}

void emu_set_realistic(Emu *emu, bool realistic)
{
    emu->realistic = realistic;
    emu->mem.uncached_read_cycles = realistic ? UNCACHED_READ_CYCLES : 0;
    emu->mem.uncached_write_cycles = realistic ? UNCACHED_WRITE_CYCLES : 0;
    emu->hw.dma_instant = !realistic;
}

void emu_free(Emu *emu)
{
    while (emu->msgbox_depth > 0)
        free(emu->msgbox[--emu->msgbox_depth]);
    for (size_t i = 0; i < sizeof emu->files / sizeof emu->files[0]; i++)
        if (emu->files[i])
            fclose(emu->files[i]);
    free(emu->saved_vram);
    mem_free(&emu->mem);
}

void emu_start(Emu *emu)
{
    cpu_reset(&emu->cpu, &emu->mem, ADDIN_BASE, emu->layout->stack_top);
    emu->cpu.r[4] = 1; /* isappli: started from the main menu */
    emu->cpu.r[5] = 0; /* optnum */
}

bool emu_parse_keys(Emu *emu, const char *list)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int k = key_find(tok);
        if (k < 0 || emu->script_len >= (int)(sizeof emu->script / sizeof emu->script[0])) {
            fprintf(stderr, "unknown key '%s'\n", tok);
            return false;
        }
        emu->script[emu->script_len++] = k;
    }
    return true;
}

int emu_next_key(Emu *emu)
{
    if (emu->script_pos < emu->script_len)
        return emu->script[emu->script_pos++];
    if (emu->headless) {
        fprintf(stderr, "headless: GetKey called with no scripted keys left\n");
        return -1;
    }
    emu_present(emu);
    return platform_wait_key();
}

void emu_update_keys(Emu *emu, const bool *host_down)
{
    uint64_t now = emu->cpu.instructions, ms = sh7305_iphi_hz(&emu->hw) / 1000;
    bool down[256];
    memcpy(down, host_down, (size_t)KEY_COUNT * sizeof *down);

    bool polled = emu->hw.keysc_used || emu->polls_keys;
    if (polled && emu->script_pos < emu->script_len) {
        if (!emu->script_next) {
            emu->script_next = now + KEY_START_MS * ms;
        } else if (now >= emu->script_next) {
            emu->script_down = !emu->script_down;
            emu->script_next = now + (emu->script_down ? KEY_HOLD_MS : KEY_GAP_MS) * ms;
            if (!emu->script_down && ++emu->script_pos == emu->script_len)
                emu->script_done = now;
        }
        if (emu->script_down)
            down[emu->script[emu->script_pos]] = true;
    } else if (polled && !emu->script_done) {
        emu->script_done = now; /* no script at all */
    }
    memcpy(emu->key_down, down, sizeof emu->key_down);
    key_matrix(down, emu->hw.keys);
}

bool emu_script_finished(const Emu *emu, uint64_t after_ms)
{
    uint64_t ms = sh7305_iphi_hz(&emu->hw) / 1000;
    return emu->script_done && emu->cpu.instructions >= emu->script_done + after_ms * ms;
}

void emu_present(Emu *emu)
{
    if (!emu->lcd.dirty)
        return;
    platform_present(emu->lcd.fb);
    emu->lcd.dirty = false;
}
