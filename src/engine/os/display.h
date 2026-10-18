#ifndef EMU_DISPLAY_H
#define EMU_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Drawing into emulated VRAM (big-endian RGB565, LCD_WIDTH x LCD_HEIGHT). */

#define TEXT_MODE_NORMAL                 0x00
#define TEXT_MODE_INVERT                 0x01
#define TEXT_MODE_TRANSPARENT_BACKGROUND 0x20

#define STATUS_HEIGHT 24  /* the status bar at the top of VRAM */

/* PrintXY / PrintCXY: 18x24 cells (Spleen 12x24 centred). PrintMini: lines
 * of 18 px (Spleen 8x16, proportional). PrintMiniMini: lines of 10 px
 * (Spleen 5x8). See font.h. */
#define BIG_CELL_W  18
#define BIG_CELL_H  24
#define MINI_CELL_W 10
#define MINI_CELL_H 18
#define SMALL_CELL_W 8   /* the emulator's own 8x8 labels, 10 px lines */
#define SMALL_CELL_H 10

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

/* 8x8 font at pixel coordinates, transparent background. */
void display_text_small(uint8_t *vram, int x, int y, const char *text, uint16_t fg);

/* Mini font (proportional, 18 px lines) at pixel coordinates; stops before
 * xlimit. Draws only if draw is set. Returns the x after the text. */
int display_text_mini(uint8_t *vram, int x, int y, const char *text, uint16_t fg, uint16_t bg,
                      bool opaque, int xlimit, bool draw);

/* Proportional text in any font (font.h): glyphs at y + oy, lines of
 * line_h pixels (the background), bold drawn 1 px wider. */
struct Font;
int display_text_prop(uint8_t *vram, const struct Font *font, int x, int y, int line_h, int oy,
                      const char *text, uint16_t fg, uint16_t bg, bool opaque, bool bold, int xlimit,
                      bool draw);

/* Casio strings (bytes of the OS character set): the length in bytes of
 * the character at text (0 at the end), the number of characters, whether
 * a byte starts a 2-byte character, and the Unicode code point of the
 * character at text (len: its bytes). GB 18030 (Chinese) decoding is on or
 * off for all text (EnableGB18030...). */
int text_char_len(const char *text);
int text_length(const char *text);
bool text_is_lead(uint8_t c);
uint32_t text_decode(const char *text, int *len);
void display_set_gb18030(bool on);
bool display_gb18030(void);

/* View > Font: Classic (the 8x8 font scaled, the default), Spleen (bitmap
 * fonts of the OS's sizes) or Unifont (its 8x16 glyphs, scaled). */
enum { FONT_STYLE_CLASSIC, FONT_STYLE_SPLEEN, FONT_STYLE_UNIFONT, FONT_STYLE_COUNT };
void display_set_font_style(int style);

#endif
