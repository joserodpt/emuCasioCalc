#ifndef EMU_PLATFORM_H
#define EMU_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "g3a.h"

/* Host side: window with a menu bar, input, timing, font. Implemented with
 * SDL3. Menu actions that only concern the window (zoom, screenshots,
 * pause, help) are handled here; the ones that restart the emulator come
 * back to the main loop as commands. */

typedef enum {
    CMD_NONE,
    CMD_OPEN,   /* load platform_open_path() */
    CMD_RESET,  /* restart the current add-in */
    CMD_QUIT,   /* File > Quit or the window was closed */
} Command;

/* The app's identifier: window class, launcher and icon file names. */
#define APP_ID "emucasiofxcg"

/* desktop.c: --export-icon writes the icon as a size x size PNG;
 * --install-desktop adds the icon to the user's icon theme and a launcher
 * running `run` to the applications menu (Linux). */
bool desktop_export_icon(const char *path, int size);
bool desktop_install(const char *run);

/* headless: no window (for tests). */
bool platform_init(int scale, bool headless);
void platform_shutdown(void);

/* fb is the panel: PANEL_WIDTH x PANEL_HEIGHT, host-endian RGB565. */
void platform_present(const uint16_t *fb);
bool platform_save_screenshot(const char *path);
/* The whole window as drawn (menu bar, screen, keyboard), as a .bmp. */
bool platform_save_window(const char *path);
/* Saves w x h host-endian RGB565 pixels as a .bmp file. */
bool platform_save_bmp(const char *path, const uint16_t *pixels, int w, int h);

/* The add-in being run, and the model it runs on: they go in the title
 * bar, and Add-in info shows the header, icons and model detection
 * (File > Save icon saves the icon). */
void platform_set_app(const G3aInfo *info, const char *path, Model model);

/* Emulation > Model: be the model detected for each add-in, or force one.
 * Changing it reloads the add-in (CMD_RESET). */
typedef enum { CHOICE_AUTO, CHOICE_CG20, CHOICE_CG50 } ModelChoice;
ModelChoice platform_model_choice(void);

/* The toolbar's speed slider: emulated time / real time (1 = real time),
 * 0 = as fast as possible. */
double platform_speed(void);
void platform_set_speed(double speed);

/* Emulation > Timing: realistic (default) or fast. */
bool platform_realistic(void);
void platform_set_realistic(bool realistic);
void platform_set_model_choice(ModelChoice choice);

/* Opens the Add-in information window (as the Info button does). */
void platform_show_info(void);

/* The status row under the menu bar, like the calculator's status bar.
 * GetKey's SHIFT / ALPHA state (alpha: 0 off, 1 next key, 2 locked); with
 * ALPHA on, PC letters type the calculator's alpha letters. */
void platform_set_key_mode(bool shift, int alpha);
/* The add-in's status message (DefineStatusMessage). */
void platform_set_os_status(const char *msg);
/* The OS's message language (LANG_*): Language menu, --language, or the
 * add-in through APP_SYSTEM_LANGUAGE. */
int platform_language(void);
void platform_set_language(int lang);

/* Battery voltage in 1/100 V: chosen in Emulation > Battery or by
 * clicking the battery (--battery sets it at start). */
int platform_battery(void);
void platform_set_battery(int centivolts);

/* View > Font: FONT_STYLE_* (display.h) for the OS text. */
int platform_font(void);
void platform_set_font(int style);
/* The OS dump File > Load OS dump keeps for the next runs (its first 8 MB,
 * the OS, in the emulator's data folder): its path, if there is one. */
bool platform_kept_os_dump(char *path, size_t cap);
/* The UI's scaling (--ui-scale): 2 for a desktop at 200%; by default the
 * desktop's, as SDL reports it. */
void platform_set_ui_scale(float scale);

/* Shows the on-screen keyboard from the start (--keyboard). */
void platform_set_keyboard(bool show);

/* Keys held as the emulator sees them (host keys and the key script): the
 * on-screen keyboard lights them up. */
void platform_show_keys(const bool *down);

/* A message on the menu bar for a few seconds. */
void platform_flash(const char *msg);

/* Text on the right of the menu bar until replaced (NULL clears it). */
void platform_status(const char *text);

/* Processes window events. down[i] tells whether KEYS[i] is held on the
 * host keyboard. Returns false when a command is waiting. */
bool platform_poll(bool *down);

/* Blocks until a calculator key is pressed; returns its KEYS index, or -1
 * when a command is waiting. */
int platform_wait_key(void);
/* The same, giving up after ms: -2 then. */
int platform_wait_key_timeout(uint32_t ms);

/* Waits up to ms for window events (while paused or after the add-in
 * exited). Returns false when a command is waiting. */
bool platform_idle(uint32_t ms);

/* The command the user chose, cleared by reading it. */
Command platform_take_command(void);
bool platform_has_command(void);
/* The file picked with File > Open (valid after CMD_OPEN). */
const char *platform_open_path(void);
/* Emulation > Pause. */
bool platform_paused(void);

uint64_t platform_ticks_ms(void);
void platform_delay_ms(uint32_t ms);

/* 8x8 bitmap glyphs for ASCII 0x20..0x7E, one byte per row, bit 7 = left. */
const uint8_t *platform_glyph(char c);

#endif
