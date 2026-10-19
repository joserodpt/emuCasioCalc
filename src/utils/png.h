#ifndef EMU_PNG_H
#define EMU_PNG_H

#include <stdbool.h>
#include <stdint.h>

/* PNG files, compressed with a small deflate of our own (LZ77 with the
 * fixed Huffman codes): no zlib needed. */

/* Saves w x h ARGB8888 pixels (straight alpha). */
bool png_save_argb(const char *path, const uint32_t *argb, int w, int h);

/* Saves w x h host-endian RGB565 pixels, each as scale x scale pixels:
 * with a palette when there are 256 colours or fewer, else as RGB. */
bool png_save_rgb565(const char *path, const uint16_t *px, int w, int h, int scale);

#endif
