#ifndef EMU_VIEW_H
#define EMU_VIEW_H

/* Shared by the files in views/ (not part of the platform interface). */

#include <SDL3/SDL.h>

#include "platform.h"

/* window.c: the main window and the state the menus act on */
extern SDL_Window *view_window;
extern SDL_Renderer *view_renderer;
extern bool view_headless;
extern int view_text_scale;     /* UI text size, see TEXT_SCALE */
extern float view_ui_scale;     /* the desktop's scaling (2 at 200%), from SDL */
extern SDL_FRect view_screen;   /* where the calculator screen is drawn, in window pixels */
extern Command view_pending;    /* command waiting for the main loop */
extern bool view_paused;
extern G3aInfo view_app;        /* the add-in being run */
extern bool view_have_app;
extern char view_app_path[1024];
extern Model view_model;        /* the model being emulated */
extern ModelChoice view_model_choice;
extern bool view_realistic;
extern int view_speed;          /* index into the speed steps, see menubar.c */
extern bool view_show_speed;    /* Emulation > Speed: custom shows the slider row */
extern bool view_key_shift;     /* SHIFT is on (for the next key) */
extern int view_key_alpha;      /* ALPHA: 0 off, 1 for the next key, 2 locked */
extern int view_battery_cv;     /* battery voltage, 1/100 V */
extern char view_os_status[64]; /* the add-in's status message */
extern bool view_status_area_shown; /* the screen shows the OS status area */
extern int view_language;       /* LANG_* */
extern bool view_auto_reload;   /* File > Reload when the file changes */
extern bool view_show_keypad;   /* View > Keyboard: the on-screen keys */
extern int view_font;           /* View > Font: FONT_STYLE_* */
/* Copies an OS dump's OS part where the next runs load it from
 * (platform_kept_os_dump). */
bool view_keep_os_dump(const char *src);

void view_redraw(void);
/* View > Zoom: sizes the window for the screen at exactly scale x. */
void view_set_scale(int scale);
/* Shows or hides the speed row, growing or shrinking the window by its
 * height so the screen keeps its size. */
void view_show_speed_row(bool show);
/* The zoom the screen is exactly at, or 0 if in between. */
int view_zoom(void);
/* Shows text on the right of the menu bar for a few seconds. */
void view_flash_status(const char *msg);

/* UI sizes, in window pixels. TEXT_SCALE k: text 8k pixels high (8 px
 * units, from the 8x8 font the UI once used), chosen from the desktop's
 * scaling and the window's width (window.c); UI(n): n pixels at 100%. */
#define TEXT_SCALE view_text_scale
#define UI(n)      ((int)((n) * view_ui_scale + 0.5f))
#define CHAR_W     (8 * TEXT_SCALE)
#define GLYPH_H    (8 * TEXT_SCALE)
#define MENU_H     (GLYPH_H + UI(16))   /* the menu row */
#define TOOL_H     (GLYPH_H + UI(14))   /* the speed row under it, when shown */
#define STATUS_Y   (MENU_H + (view_show_speed ? TOOL_H : 0)) /* the status row, last */
#define STATUS_H   (GLYPH_H + UI(14))
#define BAR_H      (STATUS_Y + STATUS_H) /* everything above the screen */
#define ITEM_H     (GLYPH_H + UI(18))
#define PAD        (UI(6) + 4 * TEXT_SCALE)

/* The PC key for KEYS[key] ("Enter", "Esc"...), and a short label for the
 * calculator key ("EXE", "AC/ON", "x10^x"...). */
const char *view_pc_key(int key);
const char *view_calc_key(int key);

/* UI text (ui_font.c, Inter) at scale k: in a box 8k pixels high, capitals
 * centred, in the renderer's draw colour. view_text_w: its width. */
void view_text(SDL_Renderer *r, float x, float y, const char *s, int k);
float view_text_w(const char *s, int k);
/* The same at a font size in pixels, capitals centred on cy. */
void view_text_px(SDL_Renderer *r, float x, float cy, const char *s, float px);
float view_text_w_px(const char *s, float px);
/* Before a renderer is destroyed: drop its glyph atlases. */
void view_text_forget(SDL_Renderer *r);

/* menubar.c */
void menubar_draw(int width, const char *status);
/* Handles mouse events on the bar, Ctrl shortcuts, and keys while a menu
 * is open. Returns true if the event was used. */
bool menubar_event(const SDL_Event *e);
/* Turns a finished File > Open dialog into CMD_OPEN. */
void menubar_poll(void);
const char *menubar_open_path(void);
/* The slider's speed: emulated time / real time, 0 = as fast as possible. */
double menubar_speed(void);
/* Moves the slider to the step nearest to speed (0 = Max). */
void menubar_set_speed(double speed);

/* about_window.c: Help > About */
void about_window_open(void);
void about_window_close(void);
/* Returns true if the event belonged to the About window. */
bool about_window_event(const SDL_Event *e);

/* info_window.c */
void info_window_open(void);
void info_window_close(void);
/* Shows the current add-in if the window is open. */
void info_window_refresh(void);
/* Returns true if the event belonged to the info window. */
bool info_window_event(const SDL_Event *e);

/* send_addin.c: File > Copy add-in to calculator */
void send_addin_start(void);
/* Finishes the copy once the folder dialog has answered. */
void send_addin_poll(void);

/* keypad.c: the on-screen keyboard, right of the screen, KEYPAD_RATIO of
 * its height wide */
#define KEYPAD_RATIO 0.56f
void keypad_set_area(SDL_FRect r);
void keypad_draw(void);
/* Mouse events: a KEYS index pressed, -2 if used otherwise, -1 if not its. */
int keypad_event(const SDL_Event *e);
/* The key held with the mouse, or -1. */
int keypad_mouse_key(void);
/* Keys held as the emulator sees them, to light them up. */
void keypad_show_down(const bool *down);
/* window.c: whether a PC key mapped to KEYS[k] is held. */
bool view_key_held(int k);
/* Shows or hides the keypad, growing or shrinking the window so the screen
 * keeps its size. */
void view_show_keypad_panel(bool show);

/* recorder.c: File > Record GIF */
/* Starts recording to path, from the frame fb (NULL: from the next frame
 * shown). */
bool recorder_start(const char *path, const uint16_t *fb);
/* Writes the file; its path and the recording's length. False if it
 * wasn't recording, or the file couldn't be written. */
bool recorder_stop(char *path, size_t cap, uint64_t *ms);
bool recorder_active(void);
uint64_t recorder_elapsed_ms(void);
/* A frame the window shows. */
void recorder_frame(const uint16_t *fb);
/* The emulated time: without a window, recordings are timed by it. */
void recorder_clock(uint64_t emu_ms);
/* Regularly: keeps the elapsed time on the menu bar current. */
void recorder_tick(void);
/* window.c: the screen as last shown (PANEL_WIDTH x PANEL_HEIGHT). */
const uint16_t *view_last_frame(void);

/* settings.c: what the window remembers between runs */
/* The setting saved for key, or fallback if there is none. */
int settings_int(const char *key, int fallback);
/* The folder File > Open starts in: the last add-in's, "" if unknown. */
const char *settings_folder(void);
/* Notes the values the run starts with (after the command line). */
void settings_started(void);
/* Writes them (when the window closes). */
void settings_save(void);

/* font.c */
bool font_init(void);

#endif
