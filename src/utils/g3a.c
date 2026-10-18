#include "g3a.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * 1. code that refers to fx-CG50 RAM (0x8C000000.., 0xAC000000..) knows the
 *    fx-CG50: gint add-ins are like this, they pick the model at run time;
 * 2. else, built before 2017 (the fx-CG50 came out then) or drawing straight
 *    to the fx-CG10/20 VRAM (0xA8000000): fx-CG10/20;
 * 3. else fx-CG50, the current model.
 * Addresses are looked for as 32-bit constants (literal pools are 2-byte
 * aligned at least).
 */
static void detect_model(const unsigned char *code, uint32_t size, const char *date, G3aInfo *info)
{
    int cg50_refs = 0, cg20_vram_refs = 0;
    for (uint32_t i = 0; i + 4 <= size; i += 2) {
        uint32_t v = (uint32_t)code[i] << 24 | (uint32_t)code[i + 1] << 16 | (uint32_t)code[i + 2] << 8 | code[i + 3];
        if ((v >= 0x8C000000u && v < 0x8C800000u) || (v >= 0xAC000000u && v < 0xAC800000u))
            cg50_refs++;
        if (v >= 0xA8000000u && v < 0xA8000000u + VRAM_SIZE)
            cg20_vram_refs++;
    }
    int year = 0;
    if (sscanf(date, "%4d", &year) != 1)
        year = 0;

    if (cg50_refs) {
        info->model = MODEL_CG50;
        snprintf(info->model_reason, sizeof info->model_reason, "uses fx-CG50 memory");
    } else if (year >= 2000 && year < 2017) {
        info->model = MODEL_CG20;
        snprintf(info->model_reason, sizeof info->model_reason, "built in %d, before the fx-CG50", year);
    } else if (cg20_vram_refs) {
        info->model = MODEL_CG20;
        snprintf(info->model_reason, sizeof info->model_reason, "draws to the fx-CG10/20 VRAM address");
    } else {
        info->model = MODEL_CG50;
        snprintf(info->model_reason, sizeof info->model_reason, "no sign of an older model");
    }
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
    if (size <= G3A_HEADER_SIZE || size > G3A_HEADER_SIZE + (long)ADDIN_MAX) {
        fprintf(stderr, "%s: not a g3a add-in we can load (%ld bytes)\n", path, size);
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
        fprintf(stderr, "%s: warning: g3a magic not found, loading anyway\n", path);
    file->data = buf;
    file->size = (uint32_t)size;
    read_info(buf, size, &file->info);
    detect_model(buf + G3A_HEADER_SIZE, file->size - G3A_HEADER_SIZE, file->info.date, &file->info);
    return true;
}

void g3a_close(G3aFile *file)
{
    free(file->data);
    file->data = NULL;
}

bool g3a_map(Emu *emu, const G3aFile *file)
{
    uint32_t code_size = file->size - G3A_HEADER_SIZE;
    /* Map whole 64 kB pages, like the OS's TLB entries. */
    uint32_t mapped = (code_size + 0xFFFF) & ~0xFFFFu;
    memcpy(emu->rom + ADDIN_PHYS - ROM_PHYS, file->data + G3A_HEADER_SIZE, code_size);
    mem_map(&emu->mem, ADDIN_BASE, mapped, emu->rom + ADDIN_PHYS - ROM_PHYS, false);
    sh7305_add_mapping(&emu->hw, ADDIN_BASE, ADDIN_PHYS, mapped);
    return true;
}
