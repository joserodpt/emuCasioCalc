/*
 * HLE syscalls: main memory (MCS), where add-ins keep settings and saves.
 * Each item is a host file emu->data_dir/mcs/<dir>/<item>. Return values are
 * WikiPrizm's codes where it gives them (MCS_WriteItem,
 * MCS_DeleteDirectory), else 0 for success and nonzero for failure.
 *
 * Our choices: main memory holds 61 KB (the fx-CG's), counted as the items'
 * sizes; MCS_GetMainMemoryStart points at unused RAM (the MCS isn't laid out
 * in memory here); the alpha variables A-Z, r and theta are 12-byte BCD
 * values kept in the item @REV2/ALPHAMEM, all zero at first.
 */
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "syscalls_impl.h"

#define MCS_FAIL 1
#define MCS_CAPACITY 61440          /* bytes of main memory */
#define MCS_NO_DIR 0x40             /* the directory doesn't exist */
#define MCS_NOT_EMPTY 0x45
#define MCS_SYSTEM_DIR 0x46
#define MCS_UNALIGNED 0x10
#define MCS_FULL 0x11
#define MCS_NO_NAME 0xF0

/* Directory and item names are short OS identifiers; keep them file-safe. */
static void safe_name(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    char raw[64];
    sc_string(emu, addr, raw, sizeof raw);
    size_t n = 0;
    for (const char *s = raw; *s && n + 1 < cap; s++) {
        char c = *s;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '@' || c == '$';
        out[n++] = ok ? c : '_';
    }
    if (!n && cap > 1)
        out[n++] = '_';
    out[n] = '\0';
}

static bool make_dir(const char *path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

/* Builds the item's path; creates its directory if create is set. */
static bool item_path(Emu *emu, uint32_t dir, uint32_t item, char *out, size_t cap, bool create)
{
    char d[64], i[64];
    safe_name(emu, dir, d, sizeof d);
    safe_name(emu, item, i, sizeof i);
    char root[512];
    snprintf(root, sizeof root, "%s/mcs", emu->data_dir);
    if (create) {
        /* the root itself may be nested (storage/mcs) */
        char path[512];
        snprintf(path, sizeof path, "%s", root);
        for (char *p = path + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                make_dir(path);
                *p = '/';
            }
        }
        make_dir(path);
        snprintf(out, cap, "%s/%s", root, d);
        if (!make_dir(out))
            return false;
    }
    return snprintf(out, cap, "%s/%s/%s", root, d, i) < (int)cap;
}

/* Copies len bytes of emulated memory to a host buffer. */
static uint8_t *copy_in(Emu *emu, uint32_t addr, uint32_t len)
{
    uint8_t *buf = malloc(len ? len : 1);
    if (buf)
        for (uint32_t k = 0; k < len; k++)
            buf[k] = mem_read8(&emu->mem, addr + k);
    return buf;
}

/* MCS_CreateDirectory(dir) */
SC(sc_mcs_create_dir)
{
    char path[600];
    /* item_path with create makes the directory; the item name is unused */
    return item_path(emu, a[0], a[0], path, sizeof path, true) ? 0 : MCS_FAIL;
}

/* MCSPutVar2(dir, item, data_len, buffer): creates or replaces an item. */
SC(sc_mcs_put_var2)
{
    char path[600];
    if (!item_path(emu, a[0], a[1], path, sizeof path, true))
        return MCS_FAIL;
    uint8_t *buf = copy_in(emu, a[3], a[2]);
    FILE *f = fopen(path, "wb");
    bool ok = buf && f && fwrite(buf, 1, a[2], f) == a[2];
    if (f)
        ok = fclose(f) == 0 && ok;
    free(buf);
    return ok ? 0 : MCS_FAIL;
}

/* MCSOvwDat2(dir, item, bytes_to_write, buffer, write_offset): overwrites
 * part of an item (creating it if needed). */
SC(sc_mcs_ovw_dat2)
{
    char path[600];
    if (!item_path(emu, a[0], a[1], path, sizeof path, true))
        return MCS_FAIL;
    FILE *f = fopen(path, "r+b");
    if (!f)
        f = fopen(path, "w+b");
    uint8_t *buf = copy_in(emu, a[3], a[2]);
    bool ok = buf && f && fseek(f, (long)a[4], SEEK_SET) == 0 && fwrite(buf, 1, a[2], f) == a[2];
    if (f)
        ok = fclose(f) == 0 && ok;
    free(buf);
    return ok ? 0 : MCS_FAIL;
}

SC(sc_mcs_del_var2)
{
    char path[600];
    if (!item_path(emu, a[0], a[1], path, sizeof path, false))
        return MCS_FAIL;
    return remove(path) == 0 ? 0 : MCS_FAIL;
}

/* MCSGetDlen2(dir, item, int *data_len): size of an item; also selects it
 * for MCSGetData1. */
SC(sc_mcs_get_dlen2)
{
    char path[600];
    struct stat st;
    emu->mcs_path[0] = '\0';
    if (!item_path(emu, a[0], a[1], path, sizeof path, false) || stat(path, &st) != 0)
        return MCS_FAIL;
    snprintf(emu->mcs_path, sizeof emu->mcs_path, "%s", path);
    mem_write32(&emu->mem, a[2], (uint32_t)st.st_size);
    return 0;
}

/* MCSGetData1(offset, len_to_copy, buffer): reads the selected item. */
SC(sc_mcs_get_data1)
{
    if (!emu->mcs_path[0])
        return MCS_FAIL;
    FILE *f = fopen(emu->mcs_path, "rb");
    if (!f)
        return MCS_FAIL;
    bool ok = fseek(f, (long)a[0], SEEK_SET) == 0;
    for (uint32_t k = 0; ok && k < a[1]; k++) {
        int c = fgetc(f);
        if (c == EOF) {
            ok = false;
            break;
        }
        mem_write8(&emu->mem, a[2] + k, (uint8_t)c);
    }
    fclose(f);
    return ok ? 0 : MCS_FAIL;
}

/* The host folder of MCS directory dir (exists or not). */
static void dir_path(Emu *emu, uint32_t dir, char *out, size_t cap)
{
    char d[64];
    safe_name(emu, dir, d, sizeof d);
    snprintf(out, cap, "%s/mcs/%s", emu->data_dir, d);
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Bytes the items take, over all directories. */
static uint32_t mcs_used(Emu *emu)
{
    char root[600];
    snprintf(root, sizeof root, "%s/mcs", emu->data_dir);
    uint32_t used = 0;
    DIR *d = opendir(root);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char sub[900];
        snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
        DIR *items = opendir(sub);
        if (!items)
            continue;
        struct dirent *it;
        while ((it = readdir(items))) {
            char path[1200];
            struct stat st;
            snprintf(path, sizeof path, "%s/%s", sub, it->d_name);
            if (it->d_name[0] != '.' && stat(path, &st) == 0 && S_ISREG(st.st_mode))
                used += (uint32_t)st.st_size;
        }
        closedir(items);
    }
    closedir(d);
    return used;
}

/* MCS_WriteItem(dir, item, itemtype, data_length, buffer): creates or
 * overwrites an item in an existing directory. 0x40: no such directory;
 * 0x10: length not a multiple of 4; 0x11: main memory full. itemtype's
 * flags (read-only...) aren't kept. */
SC(sc_mcs_write_item)
{
    char dir[600], path[600];
    dir_path(emu, a[0], dir, sizeof dir);
    if (!is_dir(dir))
        return MCS_NO_DIR;
    if (a[3] & 3)
        return MCS_UNALIGNED;
    if (!item_path(emu, a[0], a[1], path, sizeof path, false))
        return MCS_FAIL;
    struct stat st;
    uint32_t old = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    if (mcs_used(emu) - old + a[3] > MCS_CAPACITY)
        return MCS_FULL;
    uint8_t *buf = copy_in(emu, a[4], a[3]);
    FILE *f = fopen(path, "wb");
    bool ok = buf && f && fwrite(buf, 1, a[3], f) == a[3];
    if (f)
        ok = fclose(f) == 0 && ok;
    free(buf);
    return ok ? 0 : MCS_FAIL;
}

/* MCS_DeleteDirectory(dir): 0xF0 no name, 0x46 "main" / "@APLWORK" (the
 * system's), 0x40 doesn't exist, 0x45 still has items. */
SC(sc_mcs_delete_dir)
{
    char name[64], dir[600];
    sc_string(emu, a[0], name, sizeof name);
    if (!a[0] || !name[0])
        return MCS_NO_NAME;
    if (!strcmp(name, "main") || !strcmp(name, "@APLWORK"))
        return MCS_SYSTEM_DIR;
    dir_path(emu, a[0], dir, sizeof dir);
    if (!is_dir(dir))
        return MCS_NO_DIR;
    if (rmdir(dir) == 0)
        return 0;
    return errno == ENOTEMPTY || errno == EEXIST ? MCS_NOT_EMPTY : MCS_FAIL;
}

/* MCS_GetState(int *max, int *used, int *free): always 0. */
SC(sc_mcs_get_state)
{
    uint32_t used = mcs_used(emu);
    if (used > MCS_CAPACITY)
        used = MCS_CAPACITY;
    if (a[0]) mem_write32(&emu->mem, a[0], MCS_CAPACITY);
    if (a[1]) mem_write32(&emu->mem, a[1], used);
    if (a[2]) mem_write32(&emu->mem, a[2], MCS_CAPACITY - used);
    return 0;
}

/* MCS_GetCapa(int *current_bottom): the capacity; *current_bottom is the
 * start plus what is used. */
SC(sc_mcs_get_capa)
{
    if (a[0])
        mem_write32(&emu->mem, a[0], emu->layout->mcs + mcs_used(emu));
    return MCS_CAPACITY;
}

SC(sc_mcs_get_main_memory_start)
{
    (void)a;
    return emu->layout->mcs;
}

/* MCSGetOpenItem(char *item): the name of the item MCSGetDlen2 selected. */
SC(sc_mcs_get_open_item)
{
    if (!emu->mcs_path[0])
        return MCS_FAIL;
    const char *name = strrchr(emu->mcs_path, '/');
    name = name ? name + 1 : emu->mcs_path;
    for (size_t i = 0; i <= strlen(name); i++)
        mem_write8(&emu->mem, a[0] + (uint32_t)i, (uint8_t)name[i]);
    return 0;
}

/* CLIP_Store(buffer, length): the clipboard, item @REV2/CLIP. Returns 0
 * (the real return value isn't documented). */
SC(sc_clip_store)
{
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/mcs/@REV2", emu->data_dir);
    char mcs[600];
    snprintf(mcs, sizeof mcs, "%s/mcs", emu->data_dir);
    make_dir(emu->data_dir);
    make_dir(mcs);
    make_dir(dir);
    snprintf(path, sizeof path, "%s/CLIP", dir);
    uint8_t *buf = copy_in(emu, a[0], a[1]);
    FILE *f = fopen(path, "wb");
    bool ok = buf && f && fwrite(buf, 1, a[1], f) == a[1];
    if (f)
        ok = fclose(f) == 0 && ok;
    free(buf);
    return ok ? 0 : MCS_FAIL;
}

/* Alpha variables: 'A'-'Z', r (0xCD), theta (0xCE) -> 0..27, else -1. */
#define BCD_SIZE 12
#define ALPHA_COUNT 28

static int alpha_index(uint8_t name)
{
    if (name >= 'A' && name <= 'Z')
        return name - 'A';
    if (name == 0xCD)
        return 26;
    if (name == 0xCE)
        return 27;
    return -1;
}

static void alpha_file(Emu *emu, char *out, size_t cap, bool create)
{
    char mcs[600], dir[650];
    snprintf(mcs, sizeof mcs, "%s/mcs", emu->data_dir);
    snprintf(dir, sizeof dir, "%s/@REV2", mcs);
    if (create) {
        make_dir(emu->data_dir);
        make_dir(mcs);
        make_dir(dir);
    }
    snprintf(out, cap, "%s/ALPHAMEM", dir);
}

/* Alpha_GetData(name, struct BCDvalue *dest): 12 bytes; zero if never set
 * (exponent 0x100, mantissa 0). */
SC(sc_alpha_get)
{
    int i = alpha_index((uint8_t)a[0]);
    uint8_t v[BCD_SIZE] = {0x10, 0x00};
    char path[700];
    alpha_file(emu, path, sizeof path, false);
    FILE *f = i >= 0 ? fopen(path, "rb") : NULL;
    if (f) {
        uint8_t stored[BCD_SIZE];
        if (fseek(f, (long)i * BCD_SIZE, SEEK_SET) == 0 && fread(stored, 1, BCD_SIZE, f) == BCD_SIZE)
            memcpy(v, stored, BCD_SIZE);
        fclose(f);
    }
    if (i >= 0)
        for (int k = 0; k < BCD_SIZE; k++)
            mem_write8(&emu->mem, a[1] + (uint32_t)k, v[k]);
    return 0;
}

/* Alpha_SetData(name, struct BCDvalue *src) */
SC(sc_alpha_set)
{
    int i = alpha_index((uint8_t)a[0]);
    if (i < 0)
        return 0;
    char path[700];
    alpha_file(emu, path, sizeof path, true);
    uint8_t all[ALPHA_COUNT][BCD_SIZE];
    for (int k = 0; k < ALPHA_COUNT; k++) {
        memset(all[k], 0, BCD_SIZE);
        all[k][0] = 0x10;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(all, 1, sizeof all, f);
        (void)n;
        fclose(f);
    }
    for (int k = 0; k < BCD_SIZE; k++)
        all[i][k] = mem_read8(&emu->mem, a[1] + (uint32_t)k);
    f = fopen(path, "wb");
    if (f) {
        fwrite(all, 1, sizeof all, f);
        fclose(f);
    }
    return 0;
}
