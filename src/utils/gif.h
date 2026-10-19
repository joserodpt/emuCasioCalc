#ifndef EMU_GIF_H
#define EMU_GIF_H

#include <stdbool.h>
#include <stdint.h>

/* Animated GIF files from RGB565 frames (the screen recorder). Each frame
 * after the first only stores the rectangle that changed, with the pixels
 * that stayed the same transparent; its colours are exact up to 255 and
 * reduced by median cut beyond. The animation loops. */
typedef struct GifWriter GifWriter;

/* A file of w x h frames, each pixel drawn as scale x scale pixels. NULL if
 * the file can't be created. */
GifWriter *gif_open(const char *path, int w, int h, int scale);
/* Adds w x h host-endian RGB565 pixels, shown for delay_cs hundredths of a
 * second (1-65535). */
bool gif_frame(GifWriter *g, const uint16_t *pixels, int delay_cs);
/* Ends the file; false if anything failed to be written. */
bool gif_close(GifWriter *g);

#endif
