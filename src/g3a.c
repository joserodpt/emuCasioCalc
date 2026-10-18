#include "g3a.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The header starts with "USBPower" with every byte inverted. */
static const unsigned char MAGIC[8] = {0xAA, 0xAC, 0xBD, 0xAF, 0x90, 0x88, 0x9A, 0x8D};

static void read_info(const unsigned char *buf, G3aInfo *info)
{
    memcpy(info->name, buf + 0x40, 16);
    info->name[16] = '\0';
    info->has_icon = false;
    for (int i = 0; i < G3A_ICON_W * G3A_ICON_H; i++) {
        info->icon[i] = (uint16_t)(buf[0x1000 + 2 * i] << 8 | buf[0x1000 + 2 * i + 1]);
        if (info->icon[i])
            info->has_icon = true;
    }
}

bool g3a_load(Emu *emu, const char *path, G3aInfo *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= G3A_HEADER_SIZE) {
        fprintf(stderr, "%s: too small to be a g3a (%ld bytes)\n", path, size);
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
    if (info)
        read_info(buf, info);

    uint32_t code_size = (uint32_t)(size - G3A_HEADER_SIZE);
    if (code_size > ADDIN_MAX) {
        fprintf(stderr, "%s: add-in too large (%u bytes, at most %u)\n", path, code_size, ADDIN_MAX);
        free(buf);
        return false;
    }
    /* Map whole 64 kB pages, like the OS's TLB entries. */
    uint32_t mapped = (code_size + 0xFFFF) & ~0xFFFFu;
    memcpy(emu->rom + ADDIN_PHYS - ROM_PHYS, buf + G3A_HEADER_SIZE, code_size);
    mem_map(&emu->mem, ADDIN_BASE, mapped, emu->rom + ADDIN_PHYS - ROM_PHYS, false);
    sh7305_add_mapping(&emu->hw, ADDIN_BASE, ADDIN_PHYS, mapped);
    free(buf);
    printf("loaded %s: %u bytes of code at 0x%08X\n", path, code_size, ADDIN_BASE);
    return true;
}
