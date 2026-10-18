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

    /* Which calculator the add-in was made for, guessed from its contents
     * (g3a files have no field for it), and why. */
    Model model;
    char model_reason[64];

    /* Keys the add-in looks for, as bits over KEYS indices: found as the
     * OS GetKey codes for control keys (30000 and up) in its code. Add-ins
     * that read the keyboard hardware directly leave this empty. */
    uint64_t keys_used;
} G3aInfo;

/* A g3a read into memory. */
typedef struct {
    unsigned char *data;
    uint32_t size;
    G3aInfo info;
} G3aFile;

/* Reads the file and its header, and guesses the model. Checksums are not
 * verified. Release with g3a_close. */
bool g3a_read(const char *path, G3aFile *file);
void g3a_close(G3aFile *file);

/* Puts the code after the 0x7000-byte header into ROM, mapped read-only at
 * ADDIN_BASE. */
bool g3a_map(Emu *emu, const G3aFile *file);

#endif
