#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "casio_font.h"
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
    const char *keys;   /* --keys: a script on the command line */
    char *script;       /* --script: the file's text */
} Options;

/* The whole of a text file (malloc'd), or NULL. */
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *text = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *p = realloc(text, cap);
            if (!p) {
                free(text);
                fclose(f);
                return NULL;
            }
            text = p;
        }
        got = fread(text + n, 1, 4096, f);
        n += got;
    } while (got > 0);
    fclose(f);
    text[n] = '\0';
    return text;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options] [addin.g3a or .g1a]\n"
            "  (without an add-in, the window opens empty: use File > Open)\n"
            "  (the window remembers its zoom, font, model, speed... between runs;\n"
            "  these options only change them for one run)\n"
            "  --scale N          window scale, 1-6 (default 4, or the last run's)\n"
            "  --trace            log every syscall\n"
            "  --trace-hw         log exceptions, DMA transfers and unmodelled I/O registers\n"
            "  --keys K1,K2,...   keys to press first (EXE, EXIT, UP, F1, 0-9...); takes\n"
            "                     the --script steps too (\"EXE,wait 1s,shot a.png\")\n"
            "  --script F         run the steps in file F: keys, \"wait 500\" / \"wait 2s\",\n"
            "                     \"shot F.png\" (screenshot, 2x), \"scale N\" (of the next\n"
            "                     shots); # starts a comment\n"
            "  --headless         no window; input only comes from --keys / --script\n"
            "  --clock T          the calculator's clock starts at T (\"2026-01-01 12:00\")\n"
            "                     instead of now: runs show the same time\n"
            "  --screenshot F     save the last frame to F (.png or .bmp) on exit\n"
            "  --window-shot F    save the whole window (menus, keyboard) to F (.bmp) on exit\n"
            "  --record F         record the screen to F (an animated .gif) until the window\n"
            "                     closes; headless, until the run ends, in emulated time\n"
            "  --export-icon F    save the emulator's icon to F (.png, 1024 x 1024) and quit\n"
            "  --install-desktop [CMD]  add the icon and a launcher running CMD (default:\n"
            "                     this program) to the desktop's applications, and quit\n"
            "  --icon F           save the add-in's menu icon to F (.bmp)\n"
            "  --info             open the add-in information window at start\n"
            "  --model M          calculator to emulate: auto (default, detected from the\n"
            "                     add-in), cg20 (fx-CG10/20) or cg50 (fx-CG50) for .g3a\n"
            "                     add-ins, fx9860g2 (fx-9860GII) or fx9750g3 (fx-9750GIII,\n"
            "                     Graph 35+E II) for .g1a ones\n"
            "  --speed S          window speed in percent (10-400, default 100) or max\n"
            "  --timing T         realistic (default: OS calls, memory and DMA take\n"
            "                     the time they would on the calculator) or fast\n"
            "  --battery B        battery: full (default), half, low, empty, or volts (4.6)\n"
            "  --font F           font of the OS text: classic (default), spleen, unifont,\n"
            "                     or casio (the OS's own, from an OS dump; the default\n"
            "                     once one is loaded)\n"
            "  --os-dump F        read Casio's fonts from F, a dump of the calculator's OS\n"
            "                     (FirmDump); without it the window uses the one File >\n"
            "                     Load OS dump kept\n"
            "  --keyboard         show the on-screen keyboard (View > Keyboard)\n"
            "  --ui-scale S       size of the menus and texts: 1, 1.5, 2... (default: the\n"
            "                     desktop's scaling)\n"
            "  --language L       the calculator's message language: en (default), es, de,\n"
            "                     fr, pt or zh\n",
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
    display_text_mini(vram, 4 * BIG_CELL_W, 5 * BIG_CELL_H + MINI_CELL_H + 4, "to load a .g3a or .g1a", 0x0000, 0xFFFF,
                      true, LCD_WIDTH, true);
    r61524_reset(&lcd);
    r61524_blit_vram(&lcd, vram);
    platform_present(lcd.fb, PANEL_WIDTH, PANEL_HEIGHT);
    platform_status("No add-in");
}

/* Draws the error over the last screen: a white box with a red border and
 * the error in black, with what the user can do next. */
static void show_error(Emu *emu)
{
    static uint8_t vram[VRAM_SIZE];
    enum { BOX_X = 10, BOX_W = LCD_WIDTH - 20, MARGIN = 10, BORDER = 3 };
    const int width = (BOX_W - 2 * MARGIN) / SMALL_CELL_W; /* characters per line */
    char lines[16][64];
    int n = 0;

    /* The error lines, wrapped */
    const char *s = emu->cpu.error[0] ? emu->cpu.error : "unknown error";
    while (*s && n < 10) {
        int len = (int)strcspn(s, "\n");
        int take = len < width ? len : width;
        if (take == width) /* break at a space if there is one */
            for (int i = take; i > width / 2; i--)
                if (s[i] == ' ') {
                    take = i;
                    break;
                }
        snprintf(lines[n++], sizeof lines[0], "%.*s", take, s);
        s += take;
        while (*s == ' ' || *s == '\n')
            s++;
    }
    snprintf(lines[n++], sizeof lines[0], "%s", "");
    snprintf(lines[n++], sizeof lines[0], "PC %08X  PR %08X  SR %08X", emu->cpu.pc, emu->cpu.pr, emu->cpu.sr);
    snprintf(lines[n++], sizeof lines[0], "last instruction at %08X", emu->cpu.op_pc);

    int title_h = MINI_CELL_H + 6, footer_h = SMALL_CELL_H + 8;
    int box_h = 2 * MARGIN + title_h + n * SMALL_CELL_H + footer_h;
    int y0 = (LCD_HEIGHT - box_h) / 2;

    r61524_read_vram(&emu->lcd, vram);
    display_rect(vram, BOX_X, y0, BOX_X + BOX_W - 1, y0 + box_h - 1, 0xF800);
    display_rect(vram, BOX_X + BORDER, y0 + BORDER, BOX_X + BOX_W - 1 - BORDER, y0 + box_h - 1 - BORDER, 0xFFFF);
    int x = BOX_X + MARGIN, y = y0 + MARGIN;
    display_text_mini(vram, x, y, "The add-in stopped with an error", 0x0000, 0xFFFF, false, LCD_WIDTH, true);
    y += title_h;
    for (int i = 0; i < n; i++, y += SMALL_CELL_H)
        display_text_small(vram, x, y, lines[i], 0x0000);
    display_text_small(vram, x, y + 8, "Emulation > Reset, or File > Open", 0x7BEF);
    r61524_blit_vram(&emu->lcd, vram);
    if (model_mono(emu->model)) /* shown on the colour screen: it has room for the text */
        platform_present(emu->lcd.fb, PANEL_WIDTH, PANEL_HEIGHT);
    else
        emu_present(emu);
}

/* The model to emulate for an add-in: the detected one unless the user
 * chose (Emulation > Model, --model). */
static Model pick_model(const G3aInfo *info)
{
    switch (platform_model_choice()) {
    case CHOICE_CG20: return info->g1a ? info->model : MODEL_CG20;
    case CHOICE_CG50: return info->g1a ? info->model : MODEL_CG50;
    case CHOICE_FX9860G2: return info->g1a ? MODEL_FX9860G2 : info->model;
    case CHOICE_FX9750G3: return info->g1a ? MODEL_FX9750G3 : info->model;
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
        ok = emu_parse_script(emu, opt->keys);
    if (ok && fresh && opt->script)
        ok = emu_parse_script(emu, opt->script);
    if (ok)
        ok = g3a_map(emu, &file);
    *info = file.info;
    g3a_close(&file);
    if (!ok)
        return false;
    bool chosen = model != info->model;
    printf("loaded %s: %u bytes of code, running as %s (%s%s)\n", path, file.size - g3a_header_size(info),
           model_name(model), chosen ? "chosen; detected: " : "",
           chosen ? model_name(info->model) : info->model_reason);
    platform_set_app(info, path, model);
    emu->battery_cv = platform_battery();
    emu->language = platform_language();
    display_set_font_style(platform_font());
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
    double last_speed = -1;
    while (emu->cpu.state == CPU_RUNNING) {
        uint32_t hz = sh7305_iphi_hz(&emu->hw);
        Cpu *cpu = &emu->cpu;
        if (platform_paused()) {
            if (!platform_idle(20))
                return true;
            last_speed = -1; /* resynchronise after the pause */
            continue;
        }
        if (emu->realistic != platform_realistic())
            emu_set_realistic(emu, platform_realistic());
        emu->battery_cv = platform_battery();
        emu->language = platform_language();
        display_set_font_style(platform_font());
        emu_run(emu, (uint64_t)hz / 1000 * SLICE_MS);
        if (!platform_poll(down))
            return true;
        emu_update_keys(emu, down);
        platform_show_keys(emu->key_down);
        emu_present(emu);

        if (cpu->state == CPU_RUNNING && cpu->sleeping && cpu->next_event == UINT64_MAX &&
            cpu->irq_level <= (int)((cpu->sr & SR_IMASK) >> 4)) {
            cpu_report(cpu, "add-in is sleeping and no interrupt can wake it up");
            cpu->state = CPU_ERROR;
        }
        if (headless && emu_script_finished(emu, HEADLESS_TAIL_MS)) {
            printf("headless: key script finished\n");
            return false;
        }
        if (!headless) {
            /* Keep emulated time in step with real time, times the speed
             * slider (0 = no limit). */
            double speed = platform_speed();
            uint64_t emu_ms = cpu->instructions * 1000 / hz;
            uint64_t now = platform_ticks_ms();
            if (speed != last_speed) { /* restart the reference at the new pace */
                last_speed = speed;
                host_start = now - (uint64_t)(speed > 0 ? emu_ms / speed : 0);
            }
            if (speed > 0) {
                uint64_t target = host_start + (uint64_t)(emu_ms / speed); /* when we should be here */
                if (target > now)
                    platform_delay_ms((uint32_t)(target - now < 50 ? target - now : 50));
                else if (now - target > 100)
                    host_start = now - (uint64_t)(emu_ms / speed); /* blocked or too slow: don't race */
            }
        }
    }
    /* A blocking GetKey interrupted by a menu command also stops here. */
    return platform_has_command();
}

int main(int argc, char **argv)
{
    int scale = 0; /* the zoom remembered (4 the first time) */
    Options opt = {0};
    const char *path = NULL, *screenshot = NULL, *window_shot = NULL, *icon = NULL;
    bool show_info = false;
    const char *model = NULL, *timing = NULL, *speed = NULL, *battery = NULL, *language = NULL, *font = NULL;
    const char *os_dump = NULL, *record = NULL;
    bool keyboard = false;
    static Emu emu;
    static G3aInfo info;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--export-icon") && i + 1 < argc)
            return desktop_export_icon(argv[i + 1], 1024) ? 0 : 1;
        if (!strcmp(argv[i], "--install-desktop")) {
            char self[1024];
            const char *run = i + 1 < argc && argv[i + 1][0] != '-' ? argv[i + 1] : NULL;
            if (!run) {
                ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
                if (n <= 0) {
                    fprintf(stderr, "--install-desktop: give the command to run\n");
                    return 2;
                }
                self[n] = '\0';
                run = self;
            }
            return desktop_install(run) ? 0 : 1;
        }
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")) opt.trace = true;
        else if (!strcmp(argv[i], "--trace-hw")) opt.trace_hw = true;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) opt.keys = argv[++i];
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) {
            /* "2026-01-01 12:00[:00]" or with a T, local time */
            struct tm tm = {.tm_isdst = -1};
            char sep;
            int n = sscanf(argv[++i], "%d-%d-%d%c%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &sep,
                           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            time_t t = n >= 6 && (sep == ' ' || sep == 'T') ? mktime(&tm) : (time_t)-1;
            if (t == (time_t)-1) {
                fprintf(stderr, "--clock: give a date and time, like \"2026-01-01 12:00\"\n");
                return 2;
            }
            emu_set_clock((int64_t)t);
        }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            const char *f = argv[++i];
            if (!(opt.script = read_text(f))) {
                fprintf(stderr, "--script: cannot read %s\n", f);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--headless")) opt.headless = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--window-shot") && i + 1 < argc) window_shot = argv[++i];
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
        else if (!strcmp(argv[i], "--icon") && i + 1 < argc) icon = argv[++i];
        else if (!strcmp(argv[i], "--info")) show_info = true;
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--timing") && i + 1 < argc) timing = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = argv[++i];
        else if (!strcmp(argv[i], "--battery") && i + 1 < argc) battery = argv[++i];
        else if (!strcmp(argv[i], "--language") && i + 1 < argc) language = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc) font = argv[++i];
        else if (!strcmp(argv[i], "--os-dump") && i + 1 < argc) os_dump = argv[++i];
        else if (!strcmp(argv[i], "--keyboard")) keyboard = true;
        else if (!strcmp(argv[i], "--ui-scale") && i + 1 < argc) platform_set_ui_scale((float)atof(argv[++i]));
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!path && opt.headless) {
        usage(argv[0]);
        return 2;
    }
    /* What the window remembered; the options below override it for this
     * run. Headless runs (tests) start from the defaults. */
    bool remembered = !opt.headless && platform_load_settings();
    if (model) {
        if (!strcmp(model, "auto")) {
            platform_set_model_choice(CHOICE_AUTO);
        } else if (!strcmp(model, "cg20") || !strcmp(model, "cg10")) {
            platform_set_model_choice(CHOICE_CG20);
        } else if (!strcmp(model, "cg50")) {
            platform_set_model_choice(CHOICE_CG50);
        } else if (!strcmp(model, "fx9860g2")) {
            platform_set_model_choice(CHOICE_FX9860G2);
        } else if (!strcmp(model, "fx9750g3") || !strcmp(model, "g35pe2")) {
            platform_set_model_choice(CHOICE_FX9750G3);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (speed)
        platform_set_speed(!strcmp(speed, "max") ? 0 : atof(speed) / 100);
    if (timing) {
        if (strcmp(timing, "fast") && strcmp(timing, "realistic")) {
            usage(argv[0]);
            return 2;
        }
        platform_set_realistic(!strcmp(timing, "realistic"));
    }
    if (font) {
        static const char *const FONTS[FONT_STYLE_COUNT] = {"classic", "spleen", "unifont", "casio"};
        int style = -1;
        for (int i = 0; i < FONT_STYLE_COUNT; i++)
            if (!strcmp(font, FONTS[i]))
                style = i;
        if (style < 0) {
            usage(argv[0]);
            return 2;
        }
        platform_set_font(style);
    }
    /* Casio's fonts: from --os-dump, else (in the window) the dump File >
     * Load OS dump kept. Headless runs only use --os-dump, so tests don't
     * depend on what the user loaded. */
    char kept[1024];
    if (!os_dump && !opt.headless && platform_kept_os_dump(kept, sizeof kept))
        os_dump = kept;
    if (os_dump) {
        char err[256];
        if (casio_fonts_load_file(os_dump, err, sizeof err)) {
            printf("Casio fonts from OS %s (%s)\n", casio_fonts_os_version(), os_dump);
            /* switch to them: always for --os-dump, else the first time
             * (later, the font remembered stays) */
            if (!font && (os_dump != kept || !remembered))
                platform_set_font(FONT_STYLE_CASIO);
        } else {
            fprintf(stderr, "OS dump %s: %s\n", os_dump, err);
            if (os_dump != kept)
                return 1;
        }
    }
    if (platform_font() == FONT_STYLE_CASIO && !casio_fonts_loaded())
        platform_set_font(FONT_STYLE_CLASSIC); /* remembered, but the dump is gone */
    if (keyboard)
        platform_set_keyboard(true);
    if (language) {
        int lang = -1;
        for (int i = 0; i < LANG_COUNT; i++)
            if (!strcmp(language, LANG_CODES[i]))
                lang = i;
        if (lang < 0) {
            usage(argv[0]);
            return 2;
        }
        platform_set_language(lang);
    }
    if (battery) {
        static const char *const LEVELS[BATTERY_LEVELS] = {"full", "half", "low", "empty"};
        int cv = (int)(atof(battery) * 100 + 0.5);
        for (int i = 0; i < BATTERY_LEVELS; i++)
            if (!strcmp(battery, LEVELS[i]))
                cv = BATTERY_LEVEL_CV[i];
        if (cv < 300 || cv > 600) {
            usage(argv[0]);
            return 2;
        }
        platform_set_battery(cv);
    }
    if (!platform_init(scale > 0 ? scale : opt.headless ? 4 : 0, opt.headless))
        return 1;
    if (record && !platform_record(record)) {
        fprintf(stderr, "could not create %s\n", record);
        return 1;
    }
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
                show_error(&emu);
                status = 1;
            } else {
                printf("add-in exited after %llu instructions\n", (unsigned long long)emu.cpu.instructions);
                status = 0;
            }
            emu_present(&emu);
            if (opt.headless) {
                int unused = emu_finish_script(&emu);
                if (unused)
                    fprintf(stderr, "script: %d key%s left unused (the add-in ended first)\n", unused,
                            unused == 1 ? "" : "s");
                break;
            }
            platform_status(emu.cpu.state == CPU_ERROR ? "Stopped with an error" : "Add-in exited");
        }
        /* Wait for the user: Reset, Open another add-in or Quit. */
        while (platform_idle(100)) {
        }
        Command cmd = platform_take_command();
        if (cmd == CMD_QUIT) {
            printf("window closed after %llu instructions\n", (unsigned long long)emu.cpu.instructions);
            break;
        }
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

    if (window_shot && !platform_save_window(window_shot))
        fprintf(stderr, "could not save %s\n", window_shot);
    if (screenshot && !platform_save_screenshot(screenshot))
        status = 1;
    if (emu.script_failed)
        status = 1;
    platform_shutdown();
    emu_free(&emu);
    return status;
}
