#ifndef EMU_APP_ICON_H
#define EMU_APP_ICON_H

#include <stdint.h>

/* The emulator's own icon (window, Help > About): a white pixel LCD in a
 * grey border showing "CG" over a wave, drawn in code so there is no image
 * file to ship. */
#define APP_ICON_SIZE 64

/* Fills APP_ICON_SIZE x APP_ICON_SIZE pixels, ARGB8888 (straight alpha). */
void app_icon_pixels(uint32_t *argb);

/* The same at any size (size x size pixels): sharp in docks and menus. */
void app_icon_pixels_size(int size, uint32_t *argb);

#endif
