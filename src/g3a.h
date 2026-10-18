#ifndef EMU_G3A_H
#define EMU_G3A_H

#include <stdbool.h>
#include <stdint.h>

#include "emu.h"

#define G3A_HEADER_SIZE 0x7000
#define G3A_ICON_W 92
#define G3A_ICON_H 64

/* What the main menu shows for an add-in (header offsets from fxSDK's
 * fxgxa/g3a.h: name at 0x40, unselected icon at 0x1000, big-endian RGB565). */
typedef struct {
    char name[17];
    uint16_t icon[G3A_ICON_W * G3A_ICON_H]; /* host-endian RGB565 */
    bool has_icon;                          /* false if the icon is blank */
} G3aInfo;

/*
 * Loads an fx-CG add-in: the code after the 0x7000-byte header goes into
 * ROM and is mapped read-only at ADDIN_BASE. If info is not NULL, it gets
 * the add-in's name and icon. Checksums are not verified.
 */
bool g3a_load(Emu *emu, const char *path, G3aInfo *info);

#endif
