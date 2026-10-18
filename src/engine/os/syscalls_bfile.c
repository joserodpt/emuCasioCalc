/*
 * HLE syscalls: the storage memory file system (Bfile_*), where add-ins keep
 * save files. Paths like \\fls0\GAME.sav (16-bit characters) become host
 * files emu->data_dir/fls0/GAME.sav; \\crd0\ (SD card) works the same way.
 *
 * Errors are the Fugue file system's negative codes; we use the two that
 * add-ins check for.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "syscalls_impl.h"

#define ERR_NOT_FOUND      (-1)   /* IML_FILEERR_ENTRYNOTFOUND */
#define ERR_ALREADY_EXISTS (-13)  /* IML_FILEERR_ALREADYEXISTENTRY */
#define ERR_OTHER          (-5)   /* generic failure */

#define MODE_READ          1      /* Bfile_OpenFile_OS */
#define CREATE_FOLDER      5      /* Bfile_CreateEntry_OS: 1 = file */

/* Makes every folder of a path (not the last component). */
static void make_parents(const char *path)
{
    char p[700];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    }
}

/* Converts an OS path (16-bit characters, '\\' separators) to a host path
 * under data_dir. Refuses anything that could leave it. */
static bool host_path(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    char name[300];
    size_t n = 0;
    for (;;) {
        uint16_t c = mem_read16(&emu->mem, addr);
        addr += 2;
        if (!c || emu->mem.fault || n + 1 >= sizeof name)
            break;
        name[n++] = c < 0x80 ? (char)c : '_';
    }
    name[n] = '\0';

    /* \\fls0\a\b -> fls0/a/b */
    const char *s = name;
    while (*s == '\\')
        s++;
    char rel[300];
    size_t r = 0;
    for (; *s && r + 1 < sizeof rel; s++)
        rel[r++] = *s == '\\' ? '/' : *s;
    rel[r] = '\0';
    if (!rel[0] || strstr(rel, "..") || strchr(rel, ':'))
        return false;
    return snprintf(out, cap, "%s/%s", emu->data_dir, rel) < (int)cap;
}

static int new_handle(Emu *emu, FILE *f)
{
    for (int i = 0; i < (int)(sizeof emu->files / sizeof emu->files[0]); i++) {
        if (!emu->files[i]) {
            emu->files[i] = f;
            return i;
        }
    }
    fclose(f);
    return ERR_OTHER;
}

static FILE *handle_file(Emu *emu, uint32_t handle)
{
    return handle < sizeof emu->files / sizeof emu->files[0] ? emu->files[handle] : NULL;
}

/* Bfile_OpenFile_OS(name, mode, 0): mode 1 reads, 2 writes, 3 both.
 * Returns a handle >= 0 or a negative error. */
SC(sc_bfile_open)
{
    char path[700];
    if (!host_path(emu, a[0], path, sizeof path))
        return (uint32_t)ERR_NOT_FOUND;
    FILE *f = fopen(path, (a[1] & 3) == MODE_READ ? "rb" : "r+b");
    if (!f)
        return (uint32_t)ERR_NOT_FOUND;
    return (uint32_t)new_handle(emu, f);
}

SC(sc_bfile_close)
{
    FILE *f = handle_file(emu, a[0]);
    if (!f)
        return (uint32_t)ERR_OTHER;
    fclose(f);
    emu->files[a[0]] = NULL;
    return 0;
}

/* Bfile_ReadFile_OS(handle, buf, size, readpos): readpos -1 reads from the
 * current position. Like Fugue, reading past the end gives zeros (without
 * growing the file) and returns the size asked for. */
SC(sc_bfile_read)
{
    FILE *f = handle_file(emu, a[0]);
    if (!f)
        return (uint32_t)ERR_OTHER;
    if ((int32_t)a[3] >= 0 && fseek(f, (long)a[3], SEEK_SET) != 0)
        return (uint32_t)ERR_OTHER;
    bool eof = false;
    for (uint32_t n = 0; n < a[2]; n++) {
        int c = eof ? EOF : fgetc(f);
        eof = c == EOF;
        mem_write8(&emu->mem, a[1] + n, eof ? 0 : (uint8_t)c);
    }
    return a[2];
}

/* Bfile_WriteFile_OS(handle, buf, size): at the current position. Returns
 * the number of bytes written. */
SC(sc_bfile_write)
{
    FILE *f = handle_file(emu, a[0]);
    if (!f)
        return (uint32_t)ERR_OTHER;
    uint32_t n = 0;
    for (; n < a[2]; n++)
        if (fputc(mem_read8(&emu->mem, a[1] + n), f) == EOF)
            break;
    fflush(f);
    return n;
}

/* Bfile_CreateEntry_OS(name, mode, size_t *size): mode 1 creates a file of
 * *size bytes (zeros), 5 a folder. */
SC(sc_bfile_create)
{
    char path[700];
    if (!host_path(emu, a[0], path, sizeof path))
        return (uint32_t)ERR_OTHER;
    make_parents(path);
    struct stat st;
    if (stat(path, &st) == 0)
        return (uint32_t)ERR_ALREADY_EXISTS;
    if (a[1] == CREATE_FOLDER)
        return mkdir(path, 0755) == 0 ? 0 : (uint32_t)ERR_OTHER;
    FILE *f = fopen(path, "wb");
    if (!f)
        return (uint32_t)ERR_OTHER;
    uint32_t size = a[2] ? mem_read32(&emu->mem, a[2]) : 0;
    for (uint32_t i = 0; i < size; i++)
        fputc(0, f);
    return fclose(f) == 0 ? 0 : (uint32_t)ERR_OTHER;
}

SC(sc_bfile_delete)
{
    char path[700];
    if (!host_path(emu, a[0], path, sizeof path))
        return (uint32_t)ERR_NOT_FOUND;
    if (remove(path) == 0)
        return 0;
    return errno == ENOENT ? (uint32_t)ERR_NOT_FOUND : (uint32_t)ERR_OTHER;
}
