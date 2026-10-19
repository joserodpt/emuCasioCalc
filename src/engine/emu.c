#include "emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "keys.h"
#include "platform.h"
#include "syscalls.h"

#define KEY_START_MS 300  /* let a keyboard-scanning add-in settle first */
#define KEY_HOLD_MS  100
#define KEY_GAP_MS   100
#define KEY_SETTLE_MS 100 /* after a key GetKey took, before the script goes on without it */

/*
 * fx-CG50: 8 MB of RAM at 0x0C000000, VRAM at its start (0xAC000000). The
 * stack is in P1 above 0x8C000000 (gint checks this to tell the model) and
 * above the 350 kB gint borrows at 0x8C0F0000.
 *
 * fx-CG10/20: 2 MB of RAM at 0x08000000, VRAM at its start (0xA8000000);
 * gint borrows 0x880F0000, the add-in's static RAM is the last 512 kB.
 *
 * On both, the secondary VRAM (SaveVRAM_1) follows VRAM, then the heap.
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

/*
 * fx-9860GII / fx-9750GIII: 512 kB of RAM at 0x08000000. The add-in's
 * static RAM is behind 0x08100000 from physical 0x0801C000, as on the
 * calculator (fxlib maps more of it with Hmem_SetMMU, 1 kB pages from
 * there); 128 kB here. gint keeps its stack, VBR and heap in it (its size
 * read from the MMU). The OS's 1 kB VRAM, our sys_malloc heap and the stack
 * are elsewhere in RAM. The CPU runs at 29.49 MHz (Pphi 14.75 MHz).
 */
static const Layout LAYOUTS[] = {
    [MODEL_CG50] = {
        .ram_phys = 0x0C000000u, .ram_size = 0x00800000u,
        .uram_phys = 0x0C200000u, .uram_size = URAM_SIZE,
        .vram = P2(0x0C000000u),
        .vram2 = P2(0x0C030000u),
        .stack_top = P1(0x0C180000u),
        .heap_base = P1(0x0C400000u), .heap_size = 0x00400000u,
        .mcs = P1(0x0C300000u),
        .frqcr = 0x0F011112u,
    },
    [MODEL_CG20] = {
        .ram_phys = 0x08000000u, .ram_size = 0x00200000u,
        .uram_phys = 0x08180000u, .uram_size = URAM_SIZE,
        .vram = P2(0x08000000u),
        .vram2 = P2(0x08030000u),
        .stack_top = P1(0x08170000u),
        .heap_base = P1(0x08060000u), .heap_size = 0x00090000u,
        .mcs = P1(0x08180000u),
        .frqcr = 0x0F102203u,
    },
    [MODEL_FX9860G2] = {
        .ram_phys = 0x08000000u, .ram_size = 0x00080000u,
        .uram_phys = 0x0801C000u, .uram_size = 0x00020000u,
        .vram = P1(0x08004000u), .vram2 = P1(0x08004400u),
        .stack_top = P1(0x08080000u),
        .heap_base = P1(0x08040000u), .heap_size = 0x00030000u,
        .mcs = P1(0x08070000u),
        .frqcr = 0x0F202203u,
        .os_version = "02.05.2201",
    },
    [MODEL_FX9750G3] = {
        .ram_phys = 0x08000000u, .ram_size = 0x00080000u,
        .uram_phys = 0x0801C000u, .uram_size = 0x00020000u,
        .vram = P1(0x08004000u), .vram2 = P1(0x08004400u),
        .stack_top = P1(0x08080000u),
        .heap_base = P1(0x08040000u), .heap_size = 0x00030000u,
        .mcs = P1(0x08070000u),
        .frqcr = 0x0F202203u,
        .os_version = "03.60.0200",
    },
};

const char *model_name(Model model)
{
    switch (model) {
    case MODEL_CG20: return "fx-CG10/20";
    case MODEL_FX9860G2: return "fx-9860GII";
    case MODEL_FX9750G3: return "fx-9750GIII";
    default: return "fx-CG50";
    }
}

const Layout *model_layout(Model model)
{
    return &LAYOUTS[model];
}

/* --clock: when the calculator's clock starts (Unix time), 0 = now. */
static int64_t clock_start;

void emu_set_clock(int64_t unix_time)
{
    clock_start = unix_time;
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
    /* Past the OS, flash is erased (0xFF) until files and the add-in are
     * written into it. The OS area stays zero: gint takes an all-0xFF
     * product ID (0x8001FFD0) for Casio's PC emulator. */
    memset(emu->rom + OS_ROM_SIZE, 0xFF, ROM_SIZE - OS_ROM_SIZE);
    emu->flash_next = FLASH_FILES;
    emu->battery_cv = BATTERY_CENTIVOLTS;
    emu->status_flags = 0x0303; /* battery, SHIFT/ALPHA, text, glyph */
    emu->status_color[0] = emu->status_color[1] = 7; /* white */
    emu->getkey_status = 1;
    mem_map_phys(mem, ROM_PHYS, ROM_SIZE, emu->rom, false);
    /* U0 below the add-in is scratch memory. Add-ins that read and write
     * through a NULL pointer (Gravity Duck does every time the duck dies)
     * don't crash on the calculator, so they must not here: their stray
     * accesses land in this zeroed area, away from anything that matters. */
    uint8_t *low = mem_alloc(mem, ADDIN_BASE);
    if (!low)
        return false;
    mem_map(mem, 0, ADDIN_BASE, low, true);
    mem_map_phys(mem, l->ram_phys, l->ram_size, emu->ram, true);
    /* Add-in static RAM: a U0 view of part of the physical RAM. */
    mem_map(mem, URAM_BASE, l->uram_size, emu->ram + (l->uram_phys - l->ram_phys), true);
    /* On-chip memories: IL RAM, X/Y RAM, RS memory. */
    if (!mem_add_ram(mem, 0xE5200000u, 0x4000, true) ||
        !mem_add_ram(mem, 0xE5000000u, 0x20000, true) ||
        !mem_add_ram(mem, 0xFD800000u, 0x1000, true))
        return false;

    r61524_reset(&emu->lcd);
    bool mono = model_mono(model);
    if (!sh7305_init(&emu->hw, &emu->cpu, mem, mono ? NULL : &emu->lcd,
                     clock_start ? clock_start : (int64_t)time(NULL), l->frqcr))
        return false;
    if (mono) {
        t6k11_reset(&emu->mono, &emu->cpu.instructions);
        if (!t6k11_map(&emu->mono, mem) || !sh3_keys_map(emu))
            return false;
    }
    if (l->os_version) /* the OS header gint reads (OS 3.x: the newer display) */
        memcpy(emu->rom + 0x10020, l->os_version, strlen(l->os_version));
    sh7305_add_mapping(&emu->hw, URAM_BASE, l->uram_phys, l->uram_size);

    emu->vram = emu->ram + ((l->vram & 0x1FFFFFFFu) - l->ram_phys);
    emu->vram2 = emu->ram + ((l->vram2 & 0x1FFFFFFFu) - l->ram_phys);
    /* Add-ins written for the fx-CG10/20 hard-code VRAM at 0xA8000000; on
     * the fx-CG50, show those writes too by aliasing that address. */
    if (model == MODEL_CG50)
        mem_map_phys(mem, model_layout(MODEL_CG20)->ram_phys, (VRAM_SIZE + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                     emu->vram, true);
    emu->data_dir = "storage"; /* main memory files, next to where we run */
    emu_set_realistic(emu, true);
    emu->battery_type = 1;
    emu->backlight_halfmin = 6; /* 3 minutes */
    emu->power_off_min = 10;
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
    for (size_t i = 0; i < sizeof emu->finds / sizeof emu->finds[0]; i++) {
        for (int k = 0; k < emu->finds[i].count; k++)
            free(emu->finds[i].names[k]);
        free(emu->finds[i].names);
    }
    mem_free(&emu->mem);
}

void emu_start(Emu *emu)
{
    cpu_reset(&emu->cpu, &emu->mem, emu->entry ? emu->entry : ADDIN_BASE, emu->layout->stack_top);
    emu->cpu.r[4] = 1; /* isappli: started from the main menu */
    emu->cpu.r[5] = 0; /* optnum */
}

/*---------------------------------------------------------------------------
 * The key script (--keys, --script)
 *-------------------------------------------------------------------------*/

static uint64_t cycles_per_ms(const Emu *emu)
{
    return sh7305_iphi_hz(&emu->hw) / 1000;
}

/* The script's next word, or "quoted words"; NULL at its end. */
static const char *next_token(const char **text, char *buf, size_t cap)
{
    const char *p = *text;
    for (;;) {
        while (*p && strchr(" \t\r\n,", *p))
            p++;
        if (*p != '#')
            break;
        while (*p && *p != '\n') /* a comment, to the end of the line */
            p++;
    }
    if (!*p) {
        *text = p;
        return NULL;
    }
    size_t n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (n + 1 < cap)
                buf[n++] = *p;
        if (*p)
            p++;
    } else {
        for (; *p && !strchr(" \t\r\n,", *p); p++)
            if (n + 1 < cap)
                buf[n++] = *p;
    }
    buf[n] = '\0';
    *text = p;
    return buf;
}

/* "500", "500ms", "2s", "1.5s" in milliseconds; -1 if it isn't a time. */
static long parse_ms(const char *t)
{
    char *end;
    double v = strtod(t, &end);
    if (end == t || v < 0)
        return -1;
    if (!strcmp(end, "s"))
        v *= 1000;
    else if (*end && strcmp(end, "ms"))
        return -1;
    return v > 3600000 ? -1 : (long)(v + 0.5);
}

static bool add_step(Emu *emu, ScriptStep step)
{
    if (emu->script_len >= (int)(sizeof emu->script / sizeof emu->script[0])) {
        fprintf(stderr, "script: more than %d steps\n", (int)(sizeof emu->script / sizeof emu->script[0]));
        return false;
    }
    emu->script[emu->script_len++] = step;
    return true;
}

bool emu_parse_script(Emu *emu, const char *text)
{
    char tok[1024], arg[1024];
    if (!emu->shot_scale)
        emu->shot_scale = 2;
    while (next_token(&text, tok, sizeof tok)) {
        if (!strcasecmp(tok, "wait")) {
            long ms = next_token(&text, arg, sizeof arg) ? parse_ms(arg) : -1;
            if (ms < 0) {
                fprintf(stderr, "script: wait needs a time (500, 500ms, 2s)\n");
                return false;
            }
            if (!add_step(emu, (ScriptStep){.kind = STEP_WAIT, .ms = (uint32_t)ms}))
                return false;
        } else if (!strcasecmp(tok, "shot")) {
            if (!next_token(&text, arg, sizeof arg)) {
                fprintf(stderr, "script: shot needs a file name\n");
                return false;
            }
            size_t n = strlen(arg) + 1;
            if (emu->script_paths_len + n > sizeof emu->script_paths) {
                fprintf(stderr, "script: too many screenshot names\n");
                return false;
            }
            memcpy(emu->script_paths + emu->script_paths_len, arg, n);
            if (!add_step(emu, (ScriptStep){.kind = STEP_SHOT, .path = emu->script_paths_len,
                                             .scale = emu->shot_scale}))
                return false;
            emu->script_paths_len += (int)n;
        } else if (!strcasecmp(tok, "scale")) {
            int v = next_token(&text, arg, sizeof arg) ? atoi(arg) : 0;
            if (v < 1 || v > 8) {
                fprintf(stderr, "script: scale is 1 to 8\n");
                return false;
            }
            emu->shot_scale = v;
        } else {
            int k = key_find(tok);
            if (k < 0) {
                fprintf(stderr, "script: unknown key or command '%s'\n", tok);
                return false;
            }
            if (!add_step(emu, (ScriptStep){.kind = STEP_KEY, .key = k}))
                return false;
        }
    }
    return true;
}

static void take_shot(Emu *emu, const ScriptStep *step)
{
    const char *path = emu->script_paths + step->path;
    int w, h;
    const uint16_t *fb = emu_screen(emu, &w, &h);
    if (platform_save_image(path, fb, w, h, step->scale)) {
        printf("script: saved %s\n", path);
    } else {
        fprintf(stderr, "script: could not save %s\n", path);
        emu->script_failed = true;
    }
}

/* Runs the waits and screenshots whose time has come, up to the next key.
 * Returns true if that key can be pressed now. */
static bool script_ready(Emu *emu)
{
    uint64_t now = emu->cpu.instructions;
    while (emu->script_pos < emu->script_len && emu->script_next <= now) {
        const ScriptStep *step = &emu->script[emu->script_pos];
        if (step->kind == STEP_KEY)
            return true;
        if (step->kind == STEP_WAIT)
            emu->script_next = now + step->ms * cycles_per_ms(emu);
        else
            take_shot(emu, step);
        emu->script_pos++;
    }
    return false;
}

/* The add-in is blocked in GetKey until cycle `until` (a scripted wait):
 * time goes on (in real time with a window), OS timers run and the screen
 * follows. Returns -2 once there, else what emu_next_key returns: a key
 * pressed meanwhile, -1, or -3. */
static int wait_blocked(Emu *emu, uint64_t until)
{
    uint64_t per_ms = cycles_per_ms(emu);
    uint32_t serial = emu->inject_serial;
    emu_present(emu); /* what GetKey shows, from now */
    while (emu->cpu.instructions < until && emu->cpu.state == CPU_RUNNING) {
        uint64_t now = emu->cpu.instructions, due = timers_next_due(emu);
        uint64_t left = (due < until ? due : until) - now;
        if (due <= now)
            left = 0;
        if (!emu->headless) { /* in whole ms, rounded up, at most 1 s at a time */
            uint64_t ms = (left + per_ms - 1) / per_ms;
            if (ms > 1000) {
                ms = 1000;
                left = 1000 * per_ms;
            }
            int key = platform_wait_key_timeout((uint32_t)ms);
            if (key != -2)
                return key;
        }
        time_advance(emu, left);
        emu_present(emu);
        if (emu->inject_serial != serial)
            return -3;
    }
    return emu->cpu.state == CPU_RUNNING ? -2 : -1;
}

int emu_next_key(Emu *emu)
{
    /* A timer about to run (within 50 ms) runs first: it may inject a key,
     * as gint does to go back to the main menu. */
    uint64_t per_ms = cycles_per_ms(emu), due = timers_next_due(emu);
    uint32_t serial = emu->inject_serial;
    if (due != UINT64_MAX && due <= emu->cpu.instructions + 50 * per_ms) {
        time_advance(emu, due > emu->cpu.instructions ? due - emu->cpu.instructions : 0);
        if (emu->inject_serial != serial)
            return -3;
    }
    /* The script: back in GetKey, the add-in has drawn what the last key
     * did, so what follows can go on now. */
    if (emu->script_settling) {
        emu->script_settling = false;
        emu->script_next = emu->cpu.instructions;
    }
    while (emu->script_pos < emu->script_len) {
        if (script_ready(emu)) {
            int key = emu->script[emu->script_pos++].key;
            emu->script_next = emu->cpu.instructions + KEY_SETTLE_MS * per_ms;
            emu->script_settling = true;
            return key;
        }
        if (emu->script_pos < emu->script_len) {
            int key = wait_blocked(emu, emu->script_next);
            if (key != -2)
                return key;
        }
    }
    if (emu->headless) {
        fprintf(stderr, "headless: GetKey called with no scripted keys left\n");
        return -1;
    }
    emu_present(emu);
    /* With OS timers running, time goes on while waiting: their handlers
     * run when due (in real time), and the screen follows. */
    while (timers_next_due(emu) != UINT64_MAX && emu->cpu.state == CPU_RUNNING) {
        due = timers_next_due(emu);
        uint64_t left = due > emu->cpu.instructions ? due - emu->cpu.instructions : 0;
        /* wait in whole ms, rounded up, at most 1 s at a time */
        uint64_t ms = (left + per_ms - 1) / per_ms;
        if (ms > 1000) {
            ms = 1000;
            left = 1000 * per_ms;
        }
        int key = platform_wait_key_timeout((uint32_t)ms);
        if (key != -2)
            return key;
        time_advance(emu, left); /* exactly to the timer (or 1 s on) */
        emu_present(emu);
        if (emu->inject_serial != serial)
            return -3;
    }
    if (emu->cpu.state != CPU_RUNNING)
        return -1;
    return platform_wait_key();
}

int emu_finish_script(Emu *emu)
{
    int unused = 0;
    uint64_t per_ms = cycles_per_ms(emu);
    if (emu->script_next > emu->cpu.instructions && !emu->script_settling)
        emu->cpu.instructions = emu->script_next; /* the wait under way */
    for (; emu->script_pos < emu->script_len; emu->script_pos++) {
        const ScriptStep *step = &emu->script[emu->script_pos];
        if (step->kind == STEP_KEY)
            unused++;
        else if (step->kind == STEP_WAIT)
            emu->cpu.instructions += step->ms * per_ms;
        else
            take_shot(emu, step);
    }
    emu_present(emu); /* the clock, for a recording's last frame */
    return unused;
}

void emu_run(Emu *emu, uint64_t cycles)
{
    Cpu *cpu = &emu->cpu;
    uint64_t end = cpu->instructions + cycles;
    while (cpu->state == CPU_RUNNING && cpu->instructions < end) {
        uint64_t due = timers_next_due(emu);
        uint64_t stop = due < end ? due : end;
        if (stop > cpu->instructions)
            cpu_run(cpu, stop - cpu->instructions);
        if (cpu->instructions >= due)
            timers_fire_due(emu);
    }
}

void emu_update_keys(Emu *emu, const bool *host_down)
{
    uint64_t now = emu->cpu.instructions, ms = sh7305_iphi_hz(&emu->hw) / 1000;
    bool down[256];
    memcpy(down, host_down, (size_t)KEY_COUNT * sizeof *down);

    /* Waits and screenshots go on whatever the add-in does. Keys go in the
     * matrix only while the add-in reads it: a gint add-in in the OS world
     * (gint_world_switch) doesn't, and its OS GetKey calls take the keys
     * instead. Each is held, then released for a gap. */
    bool polled = emu->hw.keysc_read || emu->polls_keys;
    emu->hw.keysc_read = false;
    emu->polls_keys = false;
    if (emu->script_down) {
        if (now >= emu->script_next) {
            emu->script_down = false;
            emu->script_pos++;
            emu->script_next = now + KEY_GAP_MS * ms;
        }
    } else if (script_ready(emu) && polled) {
        if (!emu->script_next) {
            emu->script_next = now + KEY_START_MS * ms; /* let the add-in settle first */
        } else {
            emu->script_down = true;
            emu->script_next = now + KEY_HOLD_MS * ms;
        }
    }
    if (emu->script_down)
        down[emu->script[emu->script_pos].key] = true;
    else if (polled && !emu->script_done && emu->script_pos >= emu->script_len)
        emu->script_done = now; /* the script is over, or there is none */
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
    platform_clock(emu->cpu.instructions / cycles_per_ms(emu));
    if (model_mono(emu->model)) {
        if (!emu->mono.dirty)
            return;
        t6k11_render(&emu->mono, emu->mono_fb);
        platform_present(emu->mono_fb, MONO_WIDTH, MONO_HEIGHT);
        return;
    }
    if (!emu->lcd.dirty)
        return;
    platform_present(emu->lcd.fb, PANEL_WIDTH, PANEL_HEIGHT);
    emu->lcd.dirty = false;
}

const uint16_t *emu_screen(Emu *emu, int *w, int *h)
{
    if (model_mono(emu->model)) {
        if (emu->mono.dirty)
            t6k11_render(&emu->mono, emu->mono_fb);
        *w = MONO_WIDTH;
        *h = MONO_HEIGHT;
        return emu->mono_fb;
    }
    *w = PANEL_WIDTH;
    *h = PANEL_HEIGHT;
    return emu->lcd.fb;
}
