#ifndef EMU_APP_ICON_H
#define EMU_APP_ICON_H

#include <stdint.h>

/* The emulator's own window icon: a small calculator, drawn in code so
 * there is no image file to ship. */
#define APP_ICON_SIZE 64

/* Fills APP_ICON_SIZE x APP_ICON_SIZE pixels, ARGB8888. */
void app_icon_pixels(uint32_t *argb);

#endif
