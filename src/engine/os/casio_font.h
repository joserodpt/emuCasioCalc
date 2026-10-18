#ifndef EMU_CASIO_FONT_H
#define EMU_CASIO_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Casio's own fonts, read from a dump of the calculator's OS that the user
 * supplies (FirmDump); nothing of them ships with the emulator. The dump's
 * glyph lookup syscalls run on a private CPU to map every character code to
 * its bitmap, so any OS version works.
 *
 * A glyph is h rows of stride bytes, most significant bit first, w pixels
 * wide (its spacing included). Width 0 means the OS draws nothing.
 */
typedef enum {
    CFONT_BIG,       /* 18x24, PrintXY / PrintCXY (syscall 0x01E8) */
    CFONT_MINI,      /* proportional, 18 rows: PrintMini (0x01E9) */
    CFONT_TINY,      /* proportional, 11 rows: PrintMiniMini (0x01EA) */
    CFONT_TINY_BOLD, /* PrintMiniMini's bold (0x01EB) */
    CFONT_COUNT
} CasioFontId;

typedef struct {
    const uint8_t *bits;
    int w, h, stride;
} CasioGlyph;

/* Reads the fonts from an OS dump (a file starting at 0x80000000, as
 * FirmDump's OS or ROM dumps). On failure, says why in err and keeps the
 * fonts loaded before, if any. */
bool casio_fonts_load_file(const char *path, char *err, size_t errlen);
bool casio_fonts_load(const uint8_t *dump, size_t size, char *err, size_t errlen);
bool casio_fonts_loaded(void);
/* The OS version of the dump the fonts come from ("03.80.0010"), or "". */
const char *casio_fonts_os_version(void);

/* The glyph of a character code (a byte, or lead byte << 8 | byte); false
 * if the fonts aren't loaded or the OS has no glyph for it. */
bool casio_glyph(CasioFontId font, uint16_t code, CasioGlyph *out);

#endif
