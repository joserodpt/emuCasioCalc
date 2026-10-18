/* File > Copy add-in to calculator: copies the running .g3a to the
 * calculator's storage memory, which the calculator shows as a USB drive
 * (on the calculator: plug the cable, choose "USB Flash" / F1). The drive
 * is recognised by its @MainMem folder; the folder dialog starts there. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "view.h"

static char chosen[1024];
static SDL_AtomicInt chosen_ready;

/* A calculator's storage memory has an @MainMem folder at its root. */
static bool looks_like_calculator(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)))
        found = !strcasecmp(e->d_name, "@MainMem");
    closedir(d);
    return found;
}

/* A mounted calculator, if there is one: drives under /media/$USER,
 * /run/media/$USER (Linux) and /Volumes (macOS). */
static bool find_calculator(char *out, size_t cap)
{
    const char *user = getenv("USER");
    char bases[3][256];
    snprintf(bases[0], sizeof bases[0], "/media/%s", user ? user : "");
    snprintf(bases[1], sizeof bases[1], "/run/media/%s", user ? user : "");
    snprintf(bases[2], sizeof bases[2], "/Volumes");
    for (int i = 0; i < 3; i++) {
        DIR *d = opendir(bases[i]);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", bases[i], e->d_name);
            if (looks_like_calculator(path)) {
                snprintf(out, cap, "%s", path);
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    return false;
}

/* Yes/No question; true for Yes. */
static bool ask(const char *title, const char *text)
{
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Copy"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_WARNING, view_window, title, text, 2, buttons, NULL};
    int pressed = 0;
    return SDL_ShowMessageBox(&box, &pressed) && pressed == 1;
}

static void tell(SDL_MessageBoxFlags kind, const char *title, const char *text)
{
    SDL_ShowSimpleMessageBox(kind, title, text, view_window);
}

/* Copies src to dst through dst.part, flushed to the device, then renamed:
 * an interrupted copy doesn't leave a broken add-in on the calculator. */
static bool copy_file(const char *src, const char *dst, char *err, size_t cap)
{
    char part[2300];
    snprintf(part, sizeof part, "%s.part", dst);
    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(err, cap, "Can't read %s: %s", src, strerror(errno));
        return false;
    }
    FILE *out = fopen(part, "wb");
    if (!out) {
        snprintf(err, cap, "Can't write to the calculator: %s", strerror(errno));
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    ok = ok && !ferror(in) && fflush(out) == 0 && fsync(fileno(out)) == 0;
    if (!ok)
        snprintf(err, cap, "Writing to the calculator failed: %s", strerror(errno));
    fclose(in);
    if (fclose(out) != 0 && ok) {
        snprintf(err, cap, "Writing to the calculator failed: %s", strerror(errno));
        ok = false;
    }
    if (ok) {
        remove(dst); /* rename doesn't replace on every file system */
        if (rename(part, dst) != 0) {
            snprintf(err, cap, "Can't rename the copy: %s", strerror(errno));
            ok = false;
        }
    }
    if (!ok)
        remove(part);
    return ok;
}

static void SDLCALL folder_done(void *user, const char *const *files, int filter)
{
    (void)user; (void)filter;
    /* NULL: no dialog available; no entry or an empty one (some dialog
     * backends): cancelled */
    if (!files || !files[0] || !files[0][0])
        return;
    snprintf(chosen, sizeof chosen, "%s", files[0]);
    SDL_SetAtomicInt(&chosen_ready, 1);
}

void send_addin_start(void)
{
    if (!view_have_app || !view_app_path[0]) {
        view_flash_status("Open an add-in first");
        return;
    }
    char start[1100];
    bool found = find_calculator(start, sizeof start);
    if (found)
        view_flash_status("Calculator found: choose its drive and press Open");
    else
        view_flash_status("Plug the calculator in (USB Flash, F1), then choose its drive");
    SDL_ShowOpenFolderDialog(folder_done, NULL, view_window, found ? start : NULL, false);
}

static void send_to(const char *dir)
{
    struct stat dir_st;
    if (stat(dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode))
        return; /* not a folder: nothing was really chosen */
    const char *base = strrchr(view_app_path, '/');
    base = base ? base + 1 : view_app_path;
    char dst[2200], text[5000];
    snprintf(dst, sizeof dst, "%s/%s", dir, base);

    if (!looks_like_calculator(dir)) {
        snprintf(text, sizeof text,
                 "%s doesn't look like a calculator's storage memory (it has no @MainMem folder).\n\n"
                 "Copy %s there anyway?", dir, base);
        if (!ask("Not a calculator?", text))
            return;
    }
    struct stat src_st, dst_st;
    if (stat(view_app_path, &src_st) != 0) {
        snprintf(text, sizeof text, "Can't read %s: %s", view_app_path, strerror(errno));
        tell(SDL_MESSAGEBOX_ERROR, "Copy failed", text);
        return;
    }
    bool exists = stat(dst, &dst_st) == 0;
    if (exists) {
        snprintf(text, sizeof text, "The calculator already has %s (%lld bytes).\n\nReplace it with this one (%lld bytes)?",
                 base, (long long)dst_st.st_size, (long long)src_st.st_size);
        if (!ask("Replace the add-in?", text))
            return;
    }
    struct statvfs fs;
    if (statvfs(dir, &fs) == 0) {
        unsigned long long avail = (unsigned long long)fs.f_bavail * fs.f_frsize;
        unsigned long long need = (unsigned long long)src_st.st_size - (exists ? (unsigned long long)dst_st.st_size : 0);
        if (src_st.st_size > (exists ? dst_st.st_size : 0) && avail < need) {
            snprintf(text, sizeof text, "Not enough room on the calculator: %s needs %lld bytes, %llu are free.",
                     base, (long long)src_st.st_size, avail);
            tell(SDL_MESSAGEBOX_ERROR, "Copy failed", text);
            return;
        }
    }
    view_flash_status("Copying to the calculator...");
    view_redraw();
    char err[2400];
    if (!copy_file(view_app_path, dst, err, sizeof err)) {
        tell(SDL_MESSAGEBOX_ERROR, "Copy failed", err);
        return;
    }
    snprintf(text, sizeof text,
             "%s was copied to the calculator (%s).\n\n"
             "Eject the drive before unplugging the cable (on the calculator, wait until it "
             "finishes updating its storage); the add-in then appears in the main menu.",
             base, dir);
    tell(SDL_MESSAGEBOX_INFORMATION, "Add-in copied", text);
    view_flash_status("Add-in copied to the calculator");
}

void send_addin_poll(void)
{
    if (SDL_GetAtomicInt(&chosen_ready)) {
        SDL_SetAtomicInt(&chosen_ready, 0);
        send_to(chosen);
    }
}
