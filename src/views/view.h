#ifndef EMU_VIEW_H
#define EMU_VIEW_H

/* Shared by the files in views/ (not part of the platform interface). */

#include <SDL3/SDL.h>

#include "platform.h"

/* window.c: the main window and the state the menus act on */
extern SDL_Window *view_window;
extern SDL_Renderer *view_renderer;
extern bool view_headless;
extern int view_scale;          /* zoom: panel pixels are view_scale x view_scale */
extern Command view_pending;    /* command waiting for the main loop */
extern bool view_paused;
extern G3aInfo view_app;        /* the add-in being run */
extern bool view_have_app;
extern char view_app_path[1024];
extern Model view_model;        /* the model being emulated */
extern ModelChoice view_model_choice;

void view_redraw(void);
void view_set_scale(int scale);
/* Shows text on the right of the menu bar for a few seconds. */
void view_flash_status(const char *msg);

/* Menu text is SDL's 8x8 debug font drawn at 2x, or 1x at zoom 1 (396 px
 * is too narrow for the bar at 2x). Sizes in window pixels. */
#define TEXT_SCALE (view_scale >= 2 ? 2 : 1)
#define CHAR_W     (8 * TEXT_SCALE)
#define GLYPH_H    (8 * TEXT_SCALE)
#define BAR_H      (GLYPH_H + 16)
#define ITEM_H     (GLYPH_H + 18)
#define PAD        (6 + 4 * TEXT_SCALE)

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

/* info_window.c */
void info_window_open(void);
void info_window_close(void);
/* Shows the current add-in if the window is open. */
void info_window_refresh(void);
/* Returns true if the event belonged to the info window. */
bool info_window_event(const SDL_Event *e);

/* font.c */
bool font_init(void);

#endif
