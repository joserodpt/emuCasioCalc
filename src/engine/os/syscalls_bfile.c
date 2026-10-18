/*
 * HLE syscalls: the storage memory file system (Bfile_*), where add-ins keep
 * save files. Paths like \\fls0\GAME.sav (16-bit characters) become host
 * files emu->data_dir/fls0/GAME.sav; \\crd0\ (SD card) works the same way.
 *
 * Errors are the Fugue file system's negative codes (gint's bfile.h).
 * Searches (Bfile_FindFirst...) follow WikiPrizm and gint: one '*' or '?'
 * pattern in the last path component, case-insensitive, names without the
 * path. Our choices: folders are listed first, then files, each sorted by
 * name; "." and ".." are not listed; storage is 16 MB.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "syscalls_impl.h"

#define ERR_NOT_FOUND      (-1)   /* IML_FILEERR_ENTRYNOTFOUND */
#define ERR_ALREADY_EXISTS (-13)  /* IML_FILEERR_ALREADYEXISTENTRY */
#define ERR_OTHER          (-5)   /* generic failure */
#define ERR_ENUM_END       (-16)  /* IML_FILEERR_ENUMERATEEND: search finished */

#define STORAGE_BYTES      (16u << 20) /* storage memory size, for the free space */

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
static int copy_to_flash(Emu *emu, const char *path, FILE *f)
{
    struct stat st;
    if (fstat(fileno(f), &st) != 0)
        return -1;
    int i;
    for (i = 0; i < emu->flash_file_count; i++)
        if (!strcmp(emu->flash_files[i].path, path))
            break;
    if (i < emu->flash_file_count && emu->flash_files[i].size == (uint32_t)st.st_size &&
        emu->flash_files[i].mtime == (int64_t)st.st_mtime)
        return i; /* unchanged */
    uint32_t off = (emu->flash_next + 0xFFF) & ~0xFFFu;
    bool reuse = i < emu->flash_file_count &&
                 (uint64_t)st.st_size <= ((emu->flash_files[i].size + 0xFFFu) & ~0xFFFu);
    if (reuse) /* changed but still fits: same place */
        off = emu->flash_files[i].offset;
    else if (off + (uint64_t)st.st_size + 0x1000 > ADDIN_PHYS) /* no room before the add-in */
        return -1;
    if (i == emu->flash_file_count) {
        if (i == (int)(sizeof emu->flash_files / sizeof emu->flash_files[0]))
            return -1;
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
    if (!reuse)
        emu->flash_next = end;
    return i;
}

/* Marks the flash copy of a file out of date, so the next open copies it
 * again (size and mtime alone miss changes within the same second). */
static void flash_forget(Emu *emu, const char *path)
{
    for (int i = 0; i < emu->flash_file_count; i++)
        if (!strcmp(emu->flash_files[i].path, path))
            emu->flash_files[i].mtime = INT64_MIN;
}

/* The flash copy of a host file, if there is an up-to-date one: -1 if not. */
static int flash_index(Emu *emu, const char *path, const struct stat *st)
{
    for (int i = 0; i < emu->flash_file_count; i++)
        if (!strcmp(emu->flash_files[i].path, path) && emu->flash_files[i].size == (uint32_t)st->st_size &&
            emu->flash_files[i].mtime == (int64_t)st->st_mtime)
            return i;
    return -1;
}

static int new_handle(Emu *emu, FILE *f, int flash)
{
    for (int i = 0; i < (int)(sizeof emu->files / sizeof emu->files[0]); i++) {
        if (!emu->files[i]) {
            emu->files[i] = f;
            emu->file_flash[i] = flash;
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
    int flash = copy_to_flash(emu, path, f);
    return (uint32_t)new_handle(emu, f, flash);
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
    if (emu->file_flash[a[0]] >= 0)
        emu->flash_files[emu->file_flash[a[0]]].mtime = INT64_MIN;
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
    flash_forget(emu, path);
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
    flash_forget(emu, from);
    flash_forget(emu, to);
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
    flash_forget(emu, path);
    if (remove(path) == 0)
        return 0;
    return errno == ENOENT ? (uint32_t)ERR_NOT_FOUND : (uint32_t)ERR_OTHER;
}

/* Bfile_GetFileSize_OS(handle) */
SC(sc_bfile_size)
{
    FILE *f = handle_file(emu, a[0]);
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0)
        return (uint32_t)ERR_OTHER;
    return (uint32_t)st.st_size;
}

/* Bfile_NameToStr_ncpy(char *dest, const unsigned short *source, n): the
 * reverse of Bfile_StrToName_ncpy. */
SC(sc_bfile_name_to_str)
{
    for (uint32_t i = 0; i < a[2]; i++) {
        uint16_t c = mem_read16(&emu->mem, a[1] + 2 * i);
        mem_write8(&emu->mem, a[0] + i, c < 0x100 ? (uint8_t)c : '?');
        if (!c)
            break;
    }
    return 0;
}

/* Reads a 16-bit OS string (8-bit characters kept, others become '?'). */
static void read_name(Emu *emu, uint32_t addr, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        uint16_t c = mem_read16(&emu->mem, addr);
        addr += 2;
        if (!c || emu->mem.fault || n + 1 >= cap)
            break;
        out[n++] = c < 0x80 ? (char)c : '?';
    }
    out[n] = '\0';
}

/* Case-insensitive match with '*' (any run) and '?' (one character). */
static bool match_mask(const char *mask, const char *name)
{
    if (!*mask)
        return !*name;
    if (*mask == '*') {
        for (const char *s = name;; s++) {
            if (match_mask(mask + 1, s))
                return true;
            if (!*s)
                return false;
        }
    }
    if (!*name)
        return false;
    if (*mask != '?' && tolower((unsigned char)*mask) != tolower((unsigned char)*name))
        return false;
    return match_mask(mask + 1, name + 1);
}

/* Bfile_Name_MatchMask(mask, filename): 1 if it matches, else 0. */
SC(sc_bfile_match_mask)
{
    char mask[300], name[300];
    read_name(emu, a[0], mask, sizeof mask);
    read_name(emu, a[1], name, sizeof name);
    return match_mask(mask, name);
}

/* Bfile_GetMediaFree_OS(device, int *freespace): the storage size minus
 * what the files in its host folder take. */
static uint64_t used_bytes(const char *dir)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[1024];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0)
            continue;
        total += S_ISDIR(st.st_mode) ? used_bytes(path) : (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

SC(sc_bfile_media_free)
{
    char dev[300], path[700];
    read_name(emu, a[0], dev, sizeof dev);
    const char *s = dev;
    while (*s == '\\')
        s++;
    if (strncmp(s, "fls0", 4) && strncmp(s, "crd0", 4))
        return (uint32_t)ERR_OTHER;
    snprintf(path, sizeof path, "%s/%.4s", emu->data_dir, s);
    uint64_t used = used_bytes(path);
    if (a[1])
        mem_write32(&emu->mem, a[1], used >= STORAGE_BYTES ? 0 : (uint32_t)(STORAGE_BYTES - used));
    return 0;
}

/* Bfile_GetBlockAddress(handle, block_offset, unsigned char **ptr): where
 * the 4 kB block at that offset of an open file is in flash (P2), from the
 * copy made when it was opened. Returns 0, or a negative error. */
SC(sc_bfile_block_address)
{
    FILE *f = handle_file(emu, a[0]);
    if (!f || emu->file_flash[a[0]] < 0)
        return (uint32_t)ERR_OTHER;
    int i = emu->file_flash[a[0]];
    uint32_t offset = emu->flash_files[i].offset, size = emu->flash_files[i].size;
    if ((a[1] & 0xFFF) || a[1] >= ((size + 0xFFF) & ~0xFFFu))
        return (uint32_t)ERR_OTHER;
    if (a[2])
        mem_write32(&emu->mem, a[2], 0xA0000000u | (ROM_PHYS + offset + a[1]));
    return 0;
}

/* Searches */

static void find_free(Emu *emu, int h)
{
    for (int k = 0; k < emu->finds[h].count; k++)
        free(emu->finds[h].names[k]);
    free(emu->finds[h].names);
    memset(&emu->finds[h], 0, sizeof emu->finds[h]);
}

/* Folders first, then by name. */
static const char *sort_dir;
static int entry_order(const void *x, const void *y)
{
    const char *p = *(char *const *)x, *q = *(char *const *)y;
    char a1[1024], b1[1024];
    struct stat sa, sb;
    snprintf(a1, sizeof a1, "%s/%s", sort_dir, p);
    snprintf(b1, sizeof b1, "%s/%s", sort_dir, q);
    bool da = stat(a1, &sa) == 0 && S_ISDIR(sa.st_mode), db = stat(b1, &sb) == 0 && S_ISDIR(sb.st_mode);
    if (da != db)
        return da ? -1 : 1;
    return strcasecmp(p, q);
}

/* The next match of search h into foundfile (16-bit name) and fileinfo
 * (file_type_t: id, type (u16), fsize, dsize, property, address). */
static uint32_t find_report(Emu *emu, int h, uint32_t found, uint32_t info)
{
    Memory *mem = &emu->mem;
    if (emu->finds[h].pos >= emu->finds[h].count)
        return (uint32_t)ERR_ENUM_END;
    int id = emu->finds[h].pos++;
    const char *name = emu->finds[h].names[id];
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", emu->finds[h].dir, name);
    struct stat st;
    bool dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    if (found) {
        size_t i = 0;
        for (; name[i]; i++)
            mem_write16(mem, found + 2 * (uint32_t)i, (uint8_t)name[i]);
        mem_write16(mem, found + 2 * (uint32_t)i, 0);
    }
    if (info) {
        uint32_t size = dir ? 0 : (uint32_t)st.st_size;
        int flash = dir ? -1 : flash_index(emu, path, &st);
        mem_write16(mem, info, (uint16_t)id);
        mem_write16(mem, info + 2, dir ? 0 : 1); /* BFile_Type_Directory / _File */
        mem_write32(mem, info + 4, size);
        mem_write32(mem, info + 8, size);
        mem_write32(mem, info + 12, 0);    /* complete */
        mem_write32(mem, info + 16, flash < 0 ? 0 : 0xA0000000u | (ROM_PHYS + emu->flash_files[flash].offset));
    }
    return 0;
}

/* Bfile_FindFirst(pattern, int *handle, foundfile, fileinfo). The handle
 * is given even when nothing matches (the add-in closes it). Nothing found:
 * -16 on the fx-CG10/20 (WikiPrizm), -1 on the fx-CG50 (gint, Fugue). */
SC(sc_bfile_find_first)
{
    char pattern[300];
    read_name(emu, a[0], pattern, sizeof pattern);
    const char *s = pattern;
    while (*s == '\\')
        s++;
    if (strncmp(s, "fls0\\", 5) && strncmp(s, "crd0\\", 5))
        return (uint32_t)ERR_OTHER;
    const char *last = strrchr(s, '\\');
    char rel[300], mask[300];
    snprintf(rel, sizeof rel, "%.*s", (int)(last - s), s);
    snprintf(mask, sizeof mask, "%s", last + 1);
    if (!mask[0] || strstr(rel, ".."))
        return (uint32_t)ERR_NOT_FOUND;
    for (char *c = rel; *c; c++)
        if (*c == '\\')
            *c = '/';

    int h = 0;
    while (h < 4 && emu->finds[h].used)
        h++;
    if (h == 4)
        return (uint32_t)ERR_OTHER;
    emu->finds[h].used = true;
    snprintf(emu->finds[h].dir, sizeof emu->finds[h].dir, "%s/%s", emu->data_dir, rel);
    if (a[1])
        mem_write32(&emu->mem, a[1], (uint32_t)h);

    DIR *d = opendir(emu->finds[h].dir);
    if (d) {
        struct dirent *e;
        int cap = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !match_mask(mask, e->d_name))
                continue;
            if (emu->finds[h].count == cap) {
                cap = cap ? 2 * cap : 16;
                char **grown = realloc(emu->finds[h].names, (size_t)cap * sizeof *grown);
                if (!grown)
                    break;
                emu->finds[h].names = grown;
            }
            char *copy = strdup(e->d_name);
            if (!copy)
                break;
            emu->finds[h].names[emu->finds[h].count++] = copy;
        }
        closedir(d);
        sort_dir = emu->finds[h].dir;
        if (emu->finds[h].count)
            qsort(emu->finds[h].names, (size_t)emu->finds[h].count, sizeof(char *), entry_order);
    }
    if (!emu->finds[h].count)
        return (uint32_t)(emu->model == MODEL_CG20 ? ERR_ENUM_END : ERR_NOT_FOUND);
    return find_report(emu, h, a[2], a[3]);
}

/* Bfile_FindNext(handle, foundfile, fileinfo): -16 after the last one. */
SC(sc_bfile_find_next)
{
    if (a[0] >= 4 || !emu->finds[a[0]].used)
        return (uint32_t)ERR_OTHER;
    return find_report(emu, (int)a[0], a[1], a[2]);
}

SC(sc_bfile_find_close)
{
    if (a[0] >= 4 || !emu->finds[a[0]].used)
        return (uint32_t)ERR_OTHER;
    find_free(emu, (int)a[0]);
    return 0;
}
