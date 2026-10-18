#ifndef EMU_G3A_H
#define EMU_G3A_H

#include <stdbool.h>
#include <stdint.h>

#include "emu.h"

#define G3A_HEADER_SIZE 0x7000
#define G3A_ICON_W 92
#define G3A_ICON_H 64

/* What the OS knows about an add-in, from the g3a header (offsets from
 * fxSDK's fxgxa/g3a.h). Icons are host-endian RGB565. */
typedef struct {
    char name[17];          /* 0x040 */
    char internal[12];      /* 0x060, "@NAME" */
    char version[11];       /* 0x130, "MM.mm.pppp" */
    char date[15];          /* 0x13C, "yyyy.MMdd.hhmm" */
    char filename[64];      /* 0xEBC */
    uint32_t size;          /* whole file, bytes */
    uint16_t icon[G3A_ICON_W * G3A_ICON_H];     /* 0x1000, unselected */
    uint16_t icon_sel[G3A_ICON_W * G3A_ICON_H]; /* 0x4000, selected in the menu */
    bool has_icon;          /* false if both icons are blank */
} G3aInfo;

/*
 * Loads an fx-CG add-in: the code after the 0x7000-byte header goes into
 * ROM and is mapped read-only at ADDIN_BASE. If info is not NULL, it gets
 * the add-in's name and icon. Checksums are not verified.
 */
bool g3a_load(Emu *emu, const char *path, G3aInfo *info);

#endif
