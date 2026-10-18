#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "emu.h"
#include "g3a.h"
#include "keys.h"
#include "platform.h"

/* Emulated time between event polls and screen updates. */
#define SLICE_MS 10
/* Headless runs of add-ins that scan the keyboard stop this long after the
 * last scripted key (they never return to a blocking GetKey we could see). */
#define HEADLESS_TAIL_MS 500

typedef struct {
    bool headless, trace, trace_hw;
    const char *keys;
} Options;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options] [addin.g3a]\n"
            "  (without an add-in, the window opens empty: use File > Open)\n"
            "  --scale N          window scale (default 2)\n"
            "  --trace            log every syscall\n"
            "  --trace-hw         log exceptions, DMA transfers and unmodelled I/O registers\n"
            "  --keys K1,K2,...   keys to press first (EXE, EXIT, UP, F1, 0-9...)\n"
            "  --headless         no window; input only comes from --keys\n"
            "  --screenshot F     save the last frame to F (.bmp) on exit\n"
            "  --icon F           save the add-in's menu icon to F (.bmp)\n"
            "  --info             open the add-in information window at start\n"
            "  --model M          calculator to emulate: auto (default, detected from the\n"
            "                     add-in), cg20 (fx-CG10/20) or cg50 (fx-CG50)\n",
            argv0);
}

/* What the screen shows before an add-in is loaded. */
static void show_welcome(void)
{
    static uint8_t vram[VRAM_SIZE];
    static R61524 lcd;
    display_fill(vram, 0xFFFF);
    display_rect(vram, 0, 0, LCD_WIDTH - 1, STATUS_HEIGHT - 1, 0x2A69);
    display_text_mini(vram, 8, 3, "emuCasioFxCG", 0xFFFF, 0x2A69, true, LCD_WIDTH, true);
    display_text_big(vram, 4 * BIG_CELL_W, 3 * BIG_CELL_H, "No add-in", 0x0000, 0xFFFF, true);
    display_text_mini(vram, 4 * BIG_CELL_W, 5 * BIG_CELL_H, "File > Open (Ctrl+O)", 0x0000, 0xFFFF, true,
                      LCD_WIDTH, true);
    display_text_mini(vram, 4 * BIG_CELL_W, 5 * BIG_CELL_H + MINI_CELL_H + 4, "to load a .g3a", 0x0000, 0xFFFF,
                      true, LCD_WIDTH, true);
    r61524_reset(&lcd);
    r61524_blit_vram(&lcd, vram);
    platform_present(lcd.fb);
    platform_status("No add-in");
}

/* The model to emulate for an add-in: the detected one unless the user
 * chose (Emulation > Model, --model). */
static Model pick_model(const G3aInfo *info)
{
    switch (platform_model_choice()) {
    case CHOICE_CG20: return MODEL_CG20;
    case CHOICE_CG50: return MODEL_CG50;
    default: return info->model;
    }
}

/* (Re)builds the machine for the add-in's model and loads it, ready to run.
 * fresh: emu has never been initialized. */
static bool load(Emu *emu, const Options *opt, const char *path, G3aInfo *info, bool fresh)
{
    static G3aFile file;
    if (!g3a_read(path, &file))
        return false;
    Model model = pick_model(&file.info);
    if (!fresh)
        emu_free(emu);
    memset(emu, 0, sizeof *emu);
    bool ok = emu_init(emu, model);
    if (!ok)
        fprintf(stderr, "out of memory\n");
    emu->trace = opt->trace;
    emu->headless = opt->headless;
    emu->mem.trace_io = opt->trace_hw;
    emu->cpu.trace_exceptions = opt->trace_hw;
    emu->hw.trace = opt->trace_hw;
    if (ok && fresh && opt->keys)
        ok = emu_parse_keys(emu, opt->keys);
    if (ok)
        ok = g3a_map(emu, &file);
    *info = file.info;
    g3a_close(&file);
    if (!ok)
        return false;
    printf("loaded %s: %u bytes of code, running as %s (%s%s)\n", path, file.size - G3A_HEADER_SIZE,
           model_name(model), platform_model_choice() == CHOICE_AUTO ? "" : "chosen; detected: ",
           platform_model_choice() == CHOICE_AUTO ? info->model_reason : model_name(info->model));
    platform_set_app(info, path, model);
    emu_start(emu);
    platform_status(NULL);
    return true;
}

/* Runs until the add-in stops or a menu command needs the main loop.
 * Returns true if the window was closed or a command is waiting. */
static bool run(Emu *emu, bool headless)
{
    bool down[256] = {0};
    uint64_t host_start = platform_ticks_ms();
    while (emu->cpu.state == CPU_RUNNING) {
        uint32_t hz = sh7305_iphi_hz(&emu->hw);
        Cpu *cpu = &emu->cpu;
        if (platform_paused()) {
            if (!platform_idle(20))
                return true;
            host_start = platform_ticks_ms() - cpu->instructions * 1000 / hz;
            continue;
        }
        cpu_run(cpu, (uint64_t)hz / 1000 * SLICE_MS);
        if (!platform_poll(down))
            return true;
        emu_update_keys(emu, down);
        emu_present(emu);

        if (cpu->state == CPU_RUNNING && cpu->sleeping && cpu->next_event == UINT64_MAX &&
            cpu->irq_level <= (int)((cpu->sr & SR_IMASK) >> 4)) {
            fprintf(stderr, "add-in is sleeping and no interrupt can wake it up\n");
            cpu->state = CPU_ERROR;
        }
        if (headless && emu_script_finished(emu, HEADLESS_TAIL_MS)) {
            printf("headless: key script finished\n");
            return false;
        }
        if (!headless) {
            /* Keep emulated time in step with real time. */
            uint64_t emu_ms = cpu->instructions * 1000 / hz;
            uint64_t host_ms = platform_ticks_ms() - host_start;
            if (emu_ms > host_ms)
                platform_delay_ms((uint32_t)(emu_ms - host_ms < 50 ? emu_ms - host_ms : 50));
            else if (host_ms - emu_ms > 100)
                host_start = platform_ticks_ms() - emu_ms; /* we were blocked or slow: don't race */
        }
    }
    /* A blocking GetKey interrupted by a menu command also stops here. */
    return platform_has_command();
}

int main(int argc, char **argv)
{
    int scale = 2;
    Options opt = {0};
    const char *path = NULL, *screenshot = NULL, *icon = NULL;
    bool show_info = false;
    const char *model = "auto";
    static Emu emu;
    static G3aInfo info;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")) opt.trace = true;
        else if (!strcmp(argv[i], "--trace-hw")) opt.trace_hw = true;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) opt.keys = argv[++i];
        else if (!strcmp(argv[i], "--headless")) opt.headless = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--icon") && i + 1 < argc) icon = argv[++i];
        else if (!strcmp(argv[i], "--info")) show_info = true;
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!path && opt.headless) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(model, "cg20") || !strcmp(model, "cg10")) {
        platform_set_model_choice(CHOICE_CG20);
    } else if (!strcmp(model, "cg50")) {
        platform_set_model_choice(CHOICE_CG50);
    } else if (strcmp(model, "auto")) {
        usage(argv[0]);
        return 2;
    }
    if (!platform_init(scale > 0 ? scale : 2, opt.headless))
        return 1;
    static char current[1024]; /* the add-in Reset reloads, "" if none yet */
    bool loaded = false, first = true;
    if (path) {
        if (!load(&emu, &opt, path, &info, true))
            return 1;
        if (icon && !platform_save_bmp(icon, info.icon, G3A_ICON_W, G3A_ICON_H))
            return 1;
        snprintf(current, sizeof current, "%s", path);
        loaded = true;
        first = false;
    } else {
        show_welcome();
    }
    if (show_info)
        platform_show_info();

    int status = 0;
    for (;;) {
        if (loaded && !run(&emu, opt.headless)) {
            if (emu.cpu.state == CPU_ERROR) {
                fprintf(stderr, "stopped with an error:\n");
                cpu_dump(&emu.cpu);
                status = 1;
            } else {
                printf("add-in exited after %llu instructions\n", (unsigned long long)emu.cpu.instructions);
                status = 0;
            }
            emu_present(&emu);
            if (opt.headless)
                break;
            platform_status(emu.cpu.state == CPU_ERROR ? "Stopped with an error" : "Add-in exited");
        }
        /* Wait for the user: Reset, Open another add-in or Quit. */
        while (platform_idle(100)) {
        }
        Command cmd = platform_take_command();
        if (cmd == CMD_QUIT)
            break;
        if (cmd == CMD_OPEN)
            snprintf(current, sizeof current, "%s", platform_open_path());
        if (!current[0]) { /* Reset with nothing loaded */
            show_welcome();
            continue;
        }
        loaded = load(&emu, &opt, current, &info, first);
        first = false;
        if (!loaded)
            platform_status("Could not load that file");
    }

    if (screenshot && !platform_save_screenshot(screenshot))
        status = 1;
    platform_shutdown();
    emu_free(&emu);
    return status;
}
