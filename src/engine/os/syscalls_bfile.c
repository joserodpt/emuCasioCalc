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

/* Copies an opened file into the flash (ROM), 4 kB-aligned like the file
 * system's clusters, unless the same version is already there. Add-ins like
 * CGDoom find a file's data by searching flash for its pages. */
static void copy_to_flash(Emu *emu, const char *path, FILE *f)
{
    struct stat st;
    if (fstat(fileno(f), &st) != 0)
        return;
    int i;
    for (i = 0; i < emu->flash_file_count; i++)
        if (!strcmp(emu->flash_files[i].path, path))
            break;
    if (i < emu->flash_file_count && emu->flash_files[i].size == (uint32_t)st.st_size &&
        emu->flash_files[i].mtime == (int64_t)st.st_mtime)
        return; /* unchanged */
    uint32_t off = (emu->flash_next + 0xFFF) & ~0xFFFu;
    if (off + (uint64_t)st.st_size + 0x1000 > ADDIN_PHYS) /* no room before the add-in */
        return;
    if (i == emu->flash_file_count) {
        if (i == (int)(sizeof emu->flash_files / sizeof emu->flash_files[0]))
            return;
        emu->flash_file_count++;
    }
    long pos = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = fread(emu->rom + off, 1, (size_t)st.st_size, f);
    fseek(f, pos, SEEK_SET);
    /* The rest of the last cluster reads as zeros, like Bfile_ReadFile_OS
     * past the end of the file. */
    uint32_t end = (off + (uint32_t)n + 0xFFF) & ~0xFFFu;
    memset(emu->rom + off + n, 0, end - off - n);
    snprintf(emu->flash_files[i].path, sizeof emu->flash_files[i].path, "%s", path);
    emu->flash_files[i].offset = off;
    emu->flash_files[i].size = (uint32_t)n;
    emu->flash_files[i].mtime = (int64_t)st.st_mtime;
    emu->flash_next = end;
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
    copy_to_flash(emu, path, f);
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
 * current position. Past the end of the file the buffer gets zeros. What it
 * returns there depends on the OS: the fx-CG50's returns the size asked for
 * (as gint documents for Fugue), the fx-CG10/20's the bytes actually read,
 * which add-ins of that time rely on to find the end (CGDoom). */
SC(sc_bfile_read)
{
    FILE *f = handle_file(emu, a[0]);
    if (!f)
        return (uint32_t)ERR_OTHER;
    if ((int32_t)a[3] >= 0 && fseek(f, (long)a[3], SEEK_SET) != 0)
        return (uint32_t)ERR_OTHER;
    uint32_t got = 0;
    bool eof = false;
    for (uint32_t n = 0; n < a[2]; n++) {
        int c = eof ? EOF : fgetc(f);
        eof = c == EOF;
        if (!eof)
            got++;
        mem_write8(&emu->mem, a[1] + n, eof ? 0 : (uint8_t)c);
    }
    return emu->model == MODEL_CG20 ? got : a[2];
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

/* Bfile_SeekFile_OS(handle, pos): absolute. Like Fugue, returns how much
 * of the file follows the new position. */
SC(sc_bfile_seek)
{
    FILE *f = handle_file(emu, a[0]);
    if (!f || fseek(f, 0, SEEK_END) != 0)
        return (uint32_t)ERR_OTHER;
    long size = ftell(f);
    if (fseek(f, (long)(int32_t)a[1], SEEK_SET) != 0)
        return (uint32_t)ERR_OTHER;
    return (uint32_t)(size - (long)(int32_t)a[1]);
}

SC(sc_bfile_tell)
{
    FILE *f = handle_file(emu, a[0]);
    return f ? (uint32_t)ftell(f) : (uint32_t)ERR_OTHER;
}

/* Bfile_RenameEntry(oldpath, newpath) */
SC(sc_bfile_rename)
{
    char from[700], to[700];
    if (!host_path(emu, a[0], from, sizeof from) || !host_path(emu, a[1], to, sizeof to))
        return (uint32_t)ERR_NOT_FOUND;
    make_parents(to);
    if (rename(from, to) == 0)
        return 0;
    return errno == ENOENT ? (uint32_t)ERR_NOT_FOUND : (uint32_t)ERR_OTHER;
}

/* Bfile_StrToName_ncpy(unsigned short *dest, const char *source, n): an
 * 8-bit string to the 16-bit file name format, at most n characters, NUL
 * terminated when there is room. */
SC(sc_bfile_str_to_name)
{
    uint32_t i = 0;
    for (; i < a[2]; i++) {
        uint8_t c = mem_read8(&emu->mem, a[1] + i);
        mem_write16(&emu->mem, a[0] + 2 * i, c);
        if (!c)
            return 0;
    }
    return 0;
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
