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

bool emu_init(Emu *emu)
{
    Memory *mem = &emu->mem;
    if (!mem_init(mem))
        return false;
    mem->lazy_io = true;

    emu->rom = mem_alloc(mem, ROM_SIZE);
    emu->ram = mem_alloc(mem, RAM_SIZE);
    if (!emu->rom || !emu->ram)
        return false;
    mem_map_phys(mem, ROM_PHYS, ROM_SIZE, emu->rom, false);
    mem_map_phys(mem, RAM_PHYS, RAM_SIZE, emu->ram, true);
    /* Add-in static RAM: a U0 view of part of the physical RAM. */
    mem_map(mem, URAM_BASE, URAM_SIZE, emu->ram + (URAM_PHYS - RAM_PHYS), true);
    /* On-chip memories: IL RAM, X/Y RAM, RS memory. */
    if (!mem_add_ram(mem, 0xE5200000u, 0x4000, true) ||
        !mem_add_ram(mem, 0xE5000000u, 0x20000, true) ||
        !mem_add_ram(mem, 0xFD800000u, 0x1000, true))
        return false;

    r61524_reset(&emu->lcd);
    if (!sh7305_init(&emu->hw, &emu->cpu, mem, &emu->lcd, (int64_t)time(NULL)))
        return false;
    sh7305_add_mapping(&emu->hw, URAM_BASE, URAM_PHYS, URAM_SIZE);

    emu->vram = emu->ram + (OS_VRAM & 0x1FFFFFFFu) - RAM_PHYS;
    /* Add-ins written for the fx-CG10/20 hard-code VRAM at 0xA8000000; show
     * those writes too by aliasing that address to the same VRAM. */
    mem_map_phys(mem, PRIZM_VRAM_PHYS, (VRAM_SIZE + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), emu->vram, true);
    emu->heap_next = HEAP_BASE;
    emu->data_dir = "storage"; /* main memory files, next to where we run */
    emu->cpu.event = sh7305_event;
    emu->cpu.event_user = &emu->hw;
    syscalls_install(emu);
    return true;
}

void emu_free(Emu *emu)
{
    while (emu->msgbox_depth > 0)
        free(emu->msgbox[--emu->msgbox_depth]);
    mem_free(&emu->mem);
}

void emu_start(Emu *emu)
{
    cpu_reset(&emu->cpu, &emu->mem, ADDIN_BASE, OS_STACK_TOP);
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
