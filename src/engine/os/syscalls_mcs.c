/*
 * HLE syscalls: main memory (MCS), where add-ins keep settings and saves.
 * Each item is a host file emu->data_dir/<dir>/<item>. Return values are 0
 * for success and nonzero for failure (WikiPrizm documents no codes).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "syscalls_impl.h"

#define MCS_FAIL 1

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
    if (create) {
        /* data_dir itself may be nested (a/b/c) */
        char path[512];
        snprintf(path, sizeof path, "%s", emu->data_dir);
        for (char *p = path + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                make_dir(path);
                *p = '/';
            }
        }
        make_dir(path);
        snprintf(out, cap, "%s/%s", emu->data_dir, d);
        if (!make_dir(out))
            return false;
    }
    return snprintf(out, cap, "%s/%s/%s", emu->data_dir, d, i) < (int)cap;
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
