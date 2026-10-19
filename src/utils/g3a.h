#ifndef EMU_G3A_H
#define EMU_G3A_H

#include <stdbool.h>
#include <stdint.h>

#include "emu.h"

#define G3A_HEADER_SIZE 0x7000
#define G1A_HEADER_SIZE 0x200   /* .g1a: fx-9860G add-ins (fxSDK's fxgxa/g1a.h) */
#define G1A_ICON_W 30
#define G1A_ICON_H 17
#define G3A_ICON_W 92
#define G3A_ICON_H 64
/* The languages of the header's labels, in order (the last two unused) */
#define G3A_LABELS 8
extern const char *const G3A_LANGS[G3A_LABELS];

/* What the OS knows about an add-in, from the g3a header (offsets from
 * fxSDK's fxgxa/g3a.h), or the g1a one (fxgxa/g1a.h: no language labels,
 * a 30x17 monochrome icon, drawn here 3x in the icon's 92x64). Icons are
 * host-endian RGB565. */
typedef struct {
    bool g1a;               /* a monochrome .g1a add-in */
    char name[17];          /* 0x040 */
    char internal[12];      /* 0x060, "@NAME" */
    /* 0x06B: the name in the main menu for each language (G3A_LANGS),
     * Latin-1 for the UI; characters it can't show (Chinese) are '?' */
    char labels[8][25];
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

/* Reads the file (.g3a or .g1a) and its header, and guesses the model.
 * Checksums are not verified. Release with g3a_close. */
bool g3a_read(const char *path, G3aFile *file);
void g3a_close(G3aFile *file);

/* Puts the add-in into ROM, mapped at ADDIN_BASE (writable: see g3a.c):
 * a g3a's code after its 0x7000-byte header, or a whole g1a, header
 * included (its code starts at 0x00300200). Sets emu->entry. */
bool g3a_map(Emu *emu, const G3aFile *file);
/* The header's size: where the code starts in the file. */
static inline uint32_t g3a_header_size(const G3aInfo *info)
{
    return info->g1a ? G1A_HEADER_SIZE : G3A_HEADER_SIZE;
}

#endif
