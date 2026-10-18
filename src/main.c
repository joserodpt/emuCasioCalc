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

int main(int argc, char **argv)
{
    int scale = 2;
    bool headless = false, trace = false, trace_hw = false;
    const char *path = NULL, *keys = NULL, *screenshot = NULL, *icon = NULL;
    static Emu emu;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--trace-hw")) trace_hw = true;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--icon") && i + 1 < argc) icon = argv[++i];
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!path) {
        usage(argv[0]);
        return 2;
    }

    if (!emu_init(&emu)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    emu.trace = trace;
    emu.headless = headless;
    emu.mem.trace_io = trace_hw;
    emu.cpu.trace_exceptions = trace_hw;
    emu.hw.trace = trace_hw;
    if (!platform_init(scale > 0 ? scale : 2, headless))
        return 1;
    if (keys && !emu_parse_keys(&emu, keys))
        return 2;
    static G3aInfo info;
    if (!g3a_load(&emu, path, &info))
        return 1;
    platform_set_app(info.name[0] ? info.name : path, info.has_icon ? info.icon : NULL, G3A_ICON_W, G3A_ICON_H);
    if (icon && !platform_save_bmp(icon, info.icon, G3A_ICON_W, G3A_ICON_H))
        return 1;
    emu_start(&emu);

    bool down[256] = {0}, closed = false;
    uint64_t host_start = platform_ticks_ms();
    while (emu.cpu.state == CPU_RUNNING) {
        uint32_t hz = sh7305_iphi_hz(&emu.hw);
        cpu_run(&emu.cpu, (uint64_t)hz / 1000 * SLICE_MS);
        if (!platform_poll(down)) {
            closed = true;
            break;
        }
        emu_update_keys(&emu, down);
        emu_present(&emu);

        Cpu *cpu = &emu.cpu;
        if (cpu->state == CPU_RUNNING && cpu->sleeping && cpu->next_event == UINT64_MAX &&
            cpu->irq_level <= (int)((cpu->sr & SR_IMASK) >> 4)) {
            fprintf(stderr, "add-in is sleeping and no interrupt can wake it up\n");
            cpu->state = CPU_ERROR;
        }
        if (headless && emu_script_finished(&emu, HEADLESS_TAIL_MS)) {
            printf("headless: key script finished\n");
            break;
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

    int status = 0;
    if (emu.cpu.state == CPU_ERROR) {
        fprintf(stderr, "stopped with an error:\n");
        cpu_dump(&emu.cpu);
        status = 1;
    } else {
        printf("%s after %llu instructions\n", closed ? "window closed" : "add-in exited",
               (unsigned long long)emu.cpu.instructions);
    }
    emu_present(&emu);
    if (screenshot && !platform_save_screenshot(screenshot))
        status = 1;
    platform_shutdown();
    emu_free(&emu);
    return status;
}
