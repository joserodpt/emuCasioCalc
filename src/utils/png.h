#ifndef EMU_PNG_H
#define EMU_PNG_H

#include <stdbool.h>
#include <stdint.h>

/* Saves w x h ARGB8888 pixels (straight alpha) as a PNG file: stored
 * (uncompressed) deflate blocks, enough for icons. */
bool png_save_argb(const char *path, const uint32_t *argb, int w, int h);

#endif
