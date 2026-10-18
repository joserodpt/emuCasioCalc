#ifndef EMU_PLATFORM_H
#define EMU_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/* Host side: window, input, timing, font. Implemented with SDL3. */

/* headless: no window (for tests). */
bool platform_init(int scale, bool headless);
void platform_shutdown(void);

/* fb is the panel: PANEL_WIDTH x PANEL_HEIGHT, host-endian RGB565. */
void platform_present(const uint16_t *fb);
bool platform_save_screenshot(const char *path);
/* Saves w x h host-endian RGB565 pixels as a .bmp file. */
bool platform_save_bmp(const char *path, const uint16_t *pixels, int w, int h);

/* Puts the add-in's name in the title bar and its icon (w x h, host-endian
 * RGB565, NULL for none) as the window icon. */
void platform_set_app(const char *name, const uint16_t *icon, int w, int h);

/* Processes window events. down[i] tells whether KEYS[i] is held on the
 * host keyboard. Returns false once the user closed the window. */
bool platform_poll(bool *down);

/* Blocks until a key is pressed; returns its KEYS index, or -1 on quit. */
int platform_wait_key(void);

uint64_t platform_ticks_ms(void);
void platform_delay_ms(uint32_t ms);

/* 8x8 bitmap glyphs for ASCII 0x20..0x7E, one byte per row, bit 7 = left. */
const uint8_t *platform_glyph(char c);

#endif
