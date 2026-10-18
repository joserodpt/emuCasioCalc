#ifndef EMU_DISPLAY_H
#define EMU_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Drawing into emulated VRAM (big-endian RGB565, LCD_WIDTH x LCD_HEIGHT). */

#define TEXT_MODE_NORMAL                 0x00
#define TEXT_MODE_INVERT                 0x01
#define TEXT_MODE_TRANSPARENT_BACKGROUND 0x20

#define STATUS_HEIGHT 24  /* the status bar at the top of VRAM */

/* PrintXY / PrintCXY font: 18x24 cells. PrintMini font: 18 px high, about
 * 10 px wide. The real Casio fonts are approximated by an 8x8 font, scaled. */
#define BIG_CELL_W  18
#define BIG_CELL_H  24
#define MINI_CELL_W 10
#define MINI_CELL_H 18

void display_fill(uint8_t *vram, uint16_t color);
void display_set_pixel(uint8_t *vram, int x, int y, uint16_t color);
uint16_t display_get_pixel(const uint8_t *vram, int x, int y);
void display_rect(uint8_t *vram, int x0, int y0, int x1, int y1, uint16_t color);

/* RGB565 value of a TEXT_COLOR_* index (0 black .. 7 white). */
uint16_t display_text_color(int index);

/* PrintXY semantics: x in 1..21, y in 1..8 (row 1 sits under the 24 px status
 * bar), 18x24 character cells. color is a TEXT_COLOR_* index. */
void display_print_xy(uint8_t *vram, int x, int y, const char *text, int mode, int color);

/* Big font at pixel coordinates. */
void display_text_big(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg, bool opaque);

/* Mini font at pixel coordinates; stops before xlimit. Draws only if draw is
 * set. Returns the x after the text. */
int display_text_mini(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg,
                      bool opaque, int xlimit, bool draw);

#endif
