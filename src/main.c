#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            "usage: %s [options] addin.g3a\n"
            "  --scale N          window scale (default 2)\n"
            "  --trace            log every syscall\n"
            "  --trace-hw         log exceptions, DMA transfers and unmodelled I/O registers\n"
            "  --keys K1,K2,...   keys to press first (EXE, EXIT, UP, F1, 0-9...)\n"
            "  --headless         no window; input only comes from --keys\n"
            "  --screenshot F     save the last frame to F (.bmp) on exit\n"
            "  --icon F           save the add-in's menu icon to F (.bmp)\n",
            argv0);
}

/* (Re)builds the machine and loads the add-in, ready to run. */
static bool load(Emu *emu, const Options *opt, const char *path, G3aInfo *info, bool first)
{
    if (!first)
        emu_free(emu);
    memset(emu, 0, sizeof *emu);
    if (!emu_init(emu)) {
        fprintf(stderr, "out of memory\n");
        return false;
    }
    emu->trace = opt->trace;
    emu->headless = opt->headless;
    emu->mem.trace_io = opt->trace_hw;
    emu->cpu.trace_exceptions = opt->trace_hw;
    emu->hw.trace = opt->trace_hw;
    if (first && opt->keys && !emu_parse_keys(emu, opt->keys))
        return false;
    if (!g3a_load(emu, path, info))
        return false;
    platform_set_app(info, path);
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
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (!platform_init(scale > 0 ? scale : 2, opt.headless))
        return 1;
    if (!load(&emu, &opt, path, &info, true))
        return 1;
    if (icon && !platform_save_bmp(icon, info.icon, G3A_ICON_W, G3A_ICON_H))
        return 1;

    static char current[1024];
    snprintf(current, sizeof current, "%s", path);
    bool loaded = true;
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
        loaded = load(&emu, &opt, current, &info, false);
        if (!loaded)
            platform_status("Could not load that file");
    }

    if (screenshot && !platform_save_screenshot(screenshot))
        status = 1;
    platform_shutdown();
    emu_free(&emu);
    return status;
}
