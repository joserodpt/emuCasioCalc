#include "g3a.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keys.h"

/* The header starts with "USBPower" with every byte inverted. */
static const unsigned char MAGIC[8] = {0xAA, 0xAC, 0xBD, 0xAF, 0x90, 0x88, 0x9A, 0x8D};

/* Copies a fixed-size header string, stopping at NUL or a non-printable byte. */
static void header_string(char *out, size_t cap, const unsigned char *src, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1 < cap && src[n] >= 0x20 && src[n] < 0x7F) {
        out[n] = (char)src[n];
        n++;
    }
    out[n] = '\0';
}

const char *const G3A_LANGS[G3A_LABELS] = {"English", "Spanish", "German", "French",
                                           "Portuguese", "Chinese", "Slot 7", "Slot 8"};

/* A language label: ASCII as is, UTF-8 letters of Latin-1 as Latin-1 (what
 * the UI font draws), any other character (GB 18030 in the Chinese one,
 * Casio codes) as '?', a 2-byte character as one. */
static void header_label(char *out, size_t cap, const unsigned char *src, size_t len)
{
    size_t n = 0, i = 0;
    while (i < len && src[i] && n + 1 < cap) {
        unsigned char c = src[i];
        if (c >= 0x20 && c < 0x7F) {
            out[n++] = (char)c;
            i++;
        } else if ((c == 0xC2 || c == 0xC3) && i + 1 < len && (src[i + 1] & 0xC0) == 0x80) {
            out[n++] = (char)((c & 0x03) << 6 | (src[i + 1] & 0x3F));
            i += 2;
        } else {
            out[n++] = '?';
            i += c >= 0x80 && i + 1 < len && src[i + 1] >= 0x40 ? 2 : 1;
        }
    }
    out[n] = '\0';
}

static void read_icon(uint16_t *icon, const unsigned char *src, bool *any)
{
    for (int i = 0; i < G3A_ICON_W * G3A_ICON_H; i++) {
        icon[i] = (uint16_t)(src[2 * i] << 8 | src[2 * i + 1]);
        if (icon[i])
            *any = true;
    }
}

static void read_info(const unsigned char *buf, long size, G3aInfo *info)
{
    header_string(info->name, sizeof info->name, buf + 0x040, 16);
    header_string(info->internal, sizeof info->internal, buf + 0x060, 11);
    for (int i = 0; i < G3A_LABELS; i++)
        header_label(info->labels[i], sizeof info->labels[i], buf + 0x06B + 24 * i, 24);
    header_string(info->version, sizeof info->version, buf + 0x130, 10);
    header_string(info->date, sizeof info->date, buf + 0x13C, 14);
    header_string(info->filename, sizeof info->filename, buf + 0xEBC, 324);
    info->size = (uint32_t)size;
    info->has_icon = false;
    read_icon(info->icon, buf + 0x1000, &info->has_icon);
    read_icon(info->icon_sel, buf + 0x4000, &info->has_icon);
}

/*
 * Guesses the model an add-in was made for. There is no field for it, so:
 * 1. the build date in the header decides when there is one: before 2017
 *    (when the fx-CG50 came out) means fx-CG10/20, later means fx-CG50;
 * 2. without a date, code that refers to fx-CG50 RAM (0x8C000000..,
 *    0xAC000000..) means fx-CG50, code that draws straight to the
 *    fx-CG10/20 VRAM (0xA8000000) means fx-CG10/20;
 * 3. else fx-CG50, the current model.
 * Addresses are looked for as 32-bit constants (literal pools are 2-byte
 * aligned at least).
 */
static void detect_model(const unsigned char *code, uint32_t size, const char *date, G3aInfo *info)
{
    int year = 0;
    if (sscanf(date, "%4d", &year) != 1 || year < 2000)
        year = 0;
    if (year) {
        info->model = year < 2017 ? MODEL_CG20 : MODEL_CG50;
        snprintf(info->model_reason, sizeof info->model_reason, "built in %d, %s the fx-CG50", year,
                 year < 2017 ? "before" : "after");
        return;
    }

    int cg50_refs = 0, cg20_vram_refs = 0;
    for (uint32_t i = 0; i + 4 <= size; i += 2) {
        uint32_t v = (uint32_t)code[i] << 24 | (uint32_t)code[i + 1] << 16 | (uint32_t)code[i + 2] << 8 | code[i + 3];
        if ((v >= 0x8C000000u && v < 0x8C800000u) || (v >= 0xAC000000u && v < 0xAC800000u))
            cg50_refs++;
        if (v >= 0xA8000000u && v < 0xA8000000u + VRAM_SIZE)
            cg20_vram_refs++;
    }
    if (cg50_refs) {
        info->model = MODEL_CG50;
        snprintf(info->model_reason, sizeof info->model_reason, "no build date; uses fx-CG50 memory");
    } else if (cg20_vram_refs) {
        info->model = MODEL_CG20;
        snprintf(info->model_reason, sizeof info->model_reason, "no build date; uses the fx-CG10/20 VRAM");
    } else {
        info->model = MODEL_CG50;
        snprintf(info->model_reason, sizeof info->model_reason, "no build date or older-model sign");
    }
}

/* A g1a's header: the names, version and date, and the icon (rows of 4
 * bytes, bit 7 leftmost, 1 = dark), drawn 3x; selected, it is inverted as
 * in the main menu. */
static void read_info_g1a(const unsigned char *buf, long size, G3aInfo *info)
{
    header_string(info->name, sizeof info->name, buf + 0x1D4, 8);
    header_string(info->internal, sizeof info->internal, buf + 0x020, 8);
    for (int i = 0; i < G3A_LABELS; i++)
        snprintf(info->labels[i], sizeof info->labels[i], "%s", info->name);
    header_string(info->version, sizeof info->version, buf + 0x030, 10);
    header_string(info->date, sizeof info->date, buf + 0x03C, 14);
    info->size = (uint32_t)size;
    info->has_icon = false;
    enum { K = 3, X0 = (G3A_ICON_W - G1A_ICON_W * K) / 2, Y0 = (G3A_ICON_H - G1A_ICON_H * K) / 2 };
    for (int i = 0; i < G3A_ICON_W * G3A_ICON_H; i++) {
        info->icon[i] = 0xFFFF;
        info->icon_sel[i] = 0x0000;
    }
    for (int y = 0; y < G1A_ICON_H; y++)
        for (int x = 0; x < G1A_ICON_W; x++) {
            bool dark = buf[0x04C + 4 * y + x / 8] & (0x80 >> (x & 7));
            if (dark)
                info->has_icon = true;
            for (int dy = 0; dy < K; dy++)
                for (int dx = 0; dx < K; dx++) {
                    int p = (Y0 + y * K + dy) * G3A_ICON_W + X0 + x * K + dx;
                    info->icon[p] = dark ? 0x0000 : 0xFFFF;
                    info->icon_sel[p] = dark ? 0xFFFF : 0x0000;
                }
        }
}

/* Looks for GetKey control-key codes (KEY_CTRL_*, 30000 and up) as 16-bit
 * constants. Character codes are small numbers that would match anything,
 * so they are not looked for. */
static uint64_t detect_keys(const unsigned char *code, uint32_t size)
{
    uint64_t used = 0;
    for (uint32_t i = 0; i + 2 <= size; i += 2) {
        int v = code[i] << 8 | code[i + 1];
        if (v < 30000 || v > 30100)
            continue;
        for (int k = 0; k < KEY_COUNT && k < 64; k++)
            if (KEYS[k].getkey == v)
                used |= (uint64_t)1 << k;
    }
    return used;
}

/* g1a files have the add-in type 0xF3 after the magic (inverted too). */
static bool is_g1a(const unsigned char *buf, long size)
{
    return size > 9 && buf[8] == (unsigned char)~0xF3u;
}

bool g3a_read(const char *path, G3aFile *file)
{
    memset(file, 0, sizeof *file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= G1A_HEADER_SIZE || size > G3A_HEADER_SIZE + (long)ADDIN_MAX) {
        fprintf(stderr, "%s: not an add-in we can load (%ld bytes)\n", path, size);
        fclose(f);
        return false;
    }
    unsigned char *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: read failed\n", path);
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    if (memcmp(buf, MAGIC, sizeof MAGIC) != 0)
        fprintf(stderr, "%s: warning: g3a/g1a magic not found, loading anyway\n", path);
    file->data = buf;
    file->size = (uint32_t)size;
    if (is_g1a(buf, size)) {
        file->info.g1a = true;
        read_info_g1a(buf, size, &file->info);
        /* Nothing in a g1a tells the two apart; the older display is the
         * common case (Emulation > Model chooses the other). */
        file->info.model = MODEL_FX9860G2;
        snprintf(file->info.model_reason, sizeof file->info.model_reason, "a monochrome .g1a add-in");
    } else if (size <= G3A_HEADER_SIZE) {
        fprintf(stderr, "%s: not an add-in we can load (%ld bytes)\n", path, size);
        free(buf);
        file->data = NULL;
        return false;
    } else {
        read_info(buf, size, &file->info);
        detect_model(buf + G3A_HEADER_SIZE, file->size - G3A_HEADER_SIZE, file->info.date, &file->info);
    }
    uint32_t head = g3a_header_size(&file->info);
    file->info.keys_used = detect_keys(buf + head, file->size - head);
    return true;
}

void g3a_close(G3aFile *file)
{
    free(file->data);
    file->data = NULL;
}

bool g3a_map(Emu *emu, const G3aFile *file)
{
    /* A g1a is mapped whole: its code is linked to run at 0x00300200 */
    uint32_t skip = file->info.g1a ? 0 : G3A_HEADER_SIZE;
    uint32_t code_size = file->size - skip;
    /* Map whole 64 kB pages, like the OS's TLB entries. */
    uint32_t mapped = (code_size + 0xFFFF) & ~0xFFFFu;
    memcpy(emu->rom + ADDIN_PHYS - ROM_PHYS, file->data + skip, code_size);
    emu->entry = ADDIN_BASE + (file->info.g1a ? G1A_HEADER_SIZE : 0);
    /* Writable: some add-ins patch their own constant data (Tetrizm writes
     * a digit into a string before drawing it) and run on the calculator.
     * Writes only change this in-memory copy, not the file. */
    mem_map(&emu->mem, ADDIN_BASE, mapped, emu->rom + ADDIN_PHYS - ROM_PHYS, true);
    sh7305_add_mapping(&emu->hw, ADDIN_BASE, ADDIN_PHYS, mapped);
    snprintf(emu->app_name, sizeof emu->app_name, "%s", file->info.internal);
    return true;
}
