#ifndef EMU_VIEW_H
#define EMU_VIEW_H

/* Shared by the files in views/ (not part of the platform interface). */

#include <SDL3/SDL.h>

#include "platform.h"

/* window.c: the main window and the state the menus act on */
extern SDL_Window *view_window;
extern SDL_Renderer *view_renderer;
extern bool view_headless;
extern int view_text_scale;     /* menu text size: 2x in wide windows, else 1x */
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
extern int view_language;       /* LANG_* */

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

/* Menu text is SDL's 8x8 debug font drawn at 2x, or 1x in narrow windows
 * (the bar doesn't fit at 2x below 600 px). Sizes in window pixels. */
#define TEXT_SCALE view_text_scale
#define CHAR_W     (8 * TEXT_SCALE)
#define GLYPH_H    (8 * TEXT_SCALE)
#define MENU_H     (GLYPH_H + 16)       /* the menu row */
#define TOOL_H     (GLYPH_H + 14)       /* the speed row under it, when shown */
#define STATUS_Y   (MENU_H + (view_show_speed ? TOOL_H : 0)) /* the status row, last */
#define STATUS_H   (GLYPH_H + 14)
#define BAR_H      (STATUS_Y + STATUS_H) /* everything above the screen */
#define ITEM_H     (GLYPH_H + 18)
#define PAD        (6 + 4 * TEXT_SCALE)

/* The PC key for KEYS[key] ("Enter", "Esc"...), and a short label for the
 * calculator key ("EXE", "AC/ON", "x10^x"...). */
const char *view_pc_key(int key);
const char *view_calc_key(int key);

/* Debug-font text at an integer scale k (stays crisp). */
void view_text(SDL_Renderer *r, float x, float y, const char *s, int k);

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

/* font.c */
bool font_init(void);

#endif
