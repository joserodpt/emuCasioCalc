/* File > Copy add-in to calculator: copies the running .g3a to the
 * calculator's storage memory, which the calculator shows as a USB drive
 * (on the calculator: plug the cable, choose "USB Flash" / F1). The drive
 * is recognised by its @MainMem folder; the folder dialog starts there.
 * Every step is logged on the terminal ("copy to calculator: ..."). */
#define _GNU_SOURCE /* sync_file_range */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "view.h"

static char chosen[1024];
static SDL_AtomicInt chosen_ready;

static void LOG(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "copy to calculator: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* A calculator's storage memory has an @MainMem folder at its root. */
static bool looks_like_calculator(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        LOG("  can't list %s: %s", dir, strerror(errno));
        return false;
    }
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)))
        found = !strcasecmp(e->d_name, "@MainMem");
    closedir(d);
    LOG("  %s: %s", dir, found ? "has @MainMem, a calculator" : "no @MainMem");
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
    LOG("looking for a mounted calculator");
    for (int i = 0; i < 3; i++) {
        DIR *d = opendir(bases[i]);
        if (!d) {
            LOG("  %s: %s", bases[i], strerror(errno));
            continue;
        }
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
    LOG("  no calculator found");
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
    bool shown = SDL_ShowMessageBox(&box, &pressed);
    if (!shown)
        LOG("question \"%s\" could not be shown: %s", title, SDL_GetError());
    LOG("question \"%s\": %s", title, shown && pressed == 1 ? "Copy" : "Cancel");
    return shown && pressed == 1;
}

static void tell(SDL_MessageBoxFlags kind, const char *title, const char *text)
{
    LOG("%s: %s", title, text);
    SDL_ShowSimpleMessageBox(kind, title, text, view_window);
}

/* The calculator's USB storage fails any transfer over 4 kB (8 sectors),
 * while Linux sends up to 120 kB at once: written normally, the file
 * reaches it in merged transfers that fail with EIO. So each 4 kB block is
 * pushed to the device (sync_file_range) before the next is written, and
 * goes alone. (O_DIRECT doesn't help: FAT turns direct writes that extend a
 * file back into buffered ones.) */
#define CALC_BLOCK 4096

static const char EIO_HINT[] =
    "\n\nThe calculator only accepts USB transfers of up to 4 kB. To fix it for every "
    "program, limit them for the calculator (needs root), then plug it in again:\n\n"
    "echo 'ACTION==\"add|change\", SUBSYSTEM==\"block\", ENV{DEVTYPE}==\"disk\", ATTRS{idVendor}==\"07cf\", "
    "ATTR{queue/max_sectors_kb}=\"4\"' | sudo tee /etc/udev/rules.d/90-casio-calculator.rules\n"
    "sudo udevadm control --reload";

/* Writes all of buf to fd; false with errno set on failure. */
static bool write_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += w;
        n -= (size_t)w;
    }
    return true;
}

/* Copies src to dst through dst.part, synced to the device, then renamed:
 * an interrupted copy doesn't leave a broken add-in on the calculator. */
static bool copy_file(const char *src, const char *dst, char *err, size_t cap)
{
    char part[2300];
    snprintf(part, sizeof part, "%s.part", dst);
    LOG("opening %s", src);
    int in = open(src, O_RDONLY);
    if (in < 0) {
        snprintf(err, cap, "Can't read %s: %s", src, strerror(errno));
        LOG("%s", err);
        return false;
    }
    LOG("creating %s", part);
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        snprintf(err, cap, "Can't create %s: %s", part, strerror(errno));
        LOG("%s", err);
        close(in);
        return false;
    }
    LOG("writing in %d-byte blocks, each sent to the device on its own", CALC_BLOCK);
    char buf[CALC_BLOCK];
    unsigned long long total = 0;
    bool ok = true;
    for (;;) {
        /* fill a whole block (read may return less) */
        size_t n = 0;
        while (n < CALC_BLOCK) {
            ssize_t r = read(in, buf + n, CALC_BLOCK - n);
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0) {
                snprintf(err, cap, "Reading %s failed after %llu bytes: %s", src, total, strerror(errno));
                ok = false;
                break;
            }
            if (r == 0)
                break;
            n += (size_t)r;
        }
        if (!ok || n == 0)
            break;
        if (!write_all(out, buf, n)) {
            snprintf(err, cap, "Writing %s failed after %llu bytes: %s%s", part, total, strerror(errno),
                     errno == EIO ? EIO_HINT : "");
            ok = false;
            break;
        }
#ifdef SYNC_FILE_RANGE_WRITE
        if (sync_file_range(out, (off64_t)total, (off64_t)n,
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) != 0) {
            snprintf(err, cap, "Sending %s to the device failed after %llu bytes: %s%s", part, total,
                     strerror(errno), errno == EIO ? EIO_HINT : "");
            ok = false;
            break;
        }
#else
        if (fdatasync(out) != 0) {
            snprintf(err, cap, "Sending %s to the device failed after %llu bytes: %s%s", part, total,
                     strerror(errno), errno == EIO ? EIO_HINT : "");
            ok = false;
            break;
        }
#endif
        total += n;
        if (n < CALC_BLOCK)
            break;
    }
    if (ok)
        LOG("wrote %llu bytes", total);
    if (ok) {
        LOG("syncing to the device");
        if (fsync(out) != 0) {
            snprintf(err, cap, "Syncing %s to the device failed: %s%s", part, strerror(errno),
                     errno == EIO ? EIO_HINT : "");
            ok = false;
        }
    }
    close(in);
    if (close(out) != 0 && ok) {
        snprintf(err, cap, "Closing %s failed: %s", part, strerror(errno));
        ok = false;
    }
    if (ok) {
        LOG("renaming %s -> %s", part, dst);
        if (rename(part, dst) != 0) {
            /* Some file systems don't replace on rename: only then remove
             * the old add-in, once the new one is safely written. */
            LOG("rename failed: %s; removing the old %s and retrying", strerror(errno), dst);
            if (remove(dst) != 0) {
                snprintf(err, cap, "Can't remove the old %s: %s", dst, strerror(errno));
                ok = false;
            } else if (rename(part, dst) != 0) {
                snprintf(err, cap, "Can't rename %s to %s: %s", part, dst, strerror(errno));
                ok = false;
            }
        }
    }
    if (!ok) {
        LOG("failed: %s", err);
        LOG("removing %s", part);
        if (remove(part) != 0 && errno != ENOENT)
            LOG("  could not remove it: %s", strerror(errno));
        return false;
    }
    struct stat st;
    if (stat(dst, &st) == 0)
        LOG("done: %s is %lld bytes", dst, (long long)st.st_size);
    else
        LOG("done, but %s can't be found: %s", dst, strerror(errno));
    return true;
}

/* Runs a command, logging it and its output. True if it exited with 0. */
static bool run_command(const char *const *argv, char *output, size_t cap)
{
    char line[600] = "";
    for (int i = 0; argv[i]; i++)
        snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i ? " " : "", argv[i]);
    LOG("running: %s", line);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)argv);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    output[0] = '\0';
    if (!p) {
        snprintf(output, cap, "%s", SDL_GetError());
        LOG("  can't run it: %s", output);
        return false;
    }
    size_t size = 0;
    int code = -1;
    char *text = SDL_ReadProcess(p, &size, &code);
    SDL_DestroyProcess(p);
    if (text) {
        snprintf(output, cap, "%s", text);
        size_t n = strlen(output);
        while (n && (output[n - 1] == '\n' || output[n - 1] == '\r'))
            output[--n] = '\0';
        SDL_free(text);
    }
    LOG("  exit code %d%s%s", code, output[0] ? ", output: " : "", output);
    return code == 0;
}

/* The device mounted at dir, from /proc/mounts (Linux). */
static bool mount_device(const char *dir, char *dev, size_t cap)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return false;
    char line[2048];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char src[1024], mnt[1024];
        if (sscanf(line, "%1023s %1023s", src, mnt) != 2)
            continue;
        /* spaces are written \040 */
        char plain[1024];
        size_t n = 0;
        for (const char *s = mnt; *s && n + 1 < sizeof plain; s++) {
            if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] && s[3]) {
                plain[n++] = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
                s += 3;
            } else {
                plain[n++] = *s;
            }
        }
        plain[n] = '\0';
        if (!strcmp(plain, dir)) {
            snprintf(dev, cap, "%s", src);
            found = true;
        }
    }
    fclose(f);
    return found;
}

/* Ejects the drive mounted at dir, so the cable can be unplugged and the
 * calculator leaves USB mode. Returns false with why in reason. */
static bool eject_drive(const char *dir, char *reason, size_t cap)
{
    char out[1024];
    LOG("ejecting %s", dir);
    sync();
#ifdef __APPLE__
    const char *const diskutil[] = {"diskutil", "eject", dir, NULL};
    if (run_command(diskutil, out, sizeof out))
        return true;
    snprintf(reason, cap, "diskutil: %s", out);
    return false;
#else
    char dev[1024];
    if (mount_device(dir, dev, sizeof dev)) {
        LOG("  %s is mounted from %s", dir, dev);
        const char *const unmount[] = {"udisksctl", "unmount", "-b", dev, NULL};
        if (run_command(unmount, out, sizeof out)) {
            /* Powering off tells the calculator the drive is gone; if it
             * fails, being unmounted is enough to unplug safely. */
            const char *const power_off[] = {"udisksctl", "power-off", "-b", dev, NULL};
            if (!run_command(power_off, out, sizeof out))
                LOG("  power-off failed (the drive is unmounted, unplugging is safe)");
            return true;
        }
        snprintf(reason, cap, "udisksctl: %s", out);
    } else {
        LOG("  %s is not in /proc/mounts", dir);
        snprintf(reason, cap, "%s is not a mount point", dir);
    }
    const char *const gio[] = {"gio", "mount", "-e", dir, NULL};
    if (run_command(gio, out, sizeof out))
        return true;
    if (out[0])
        snprintf(reason + strlen(reason), cap - strlen(reason), "; gio: %s", out);
    return false;
#endif
}

static void SDLCALL folder_done(void *user, const char *const *files, int filter)
{
    (void)user; (void)filter;
    /* NULL: no dialog available; no entry or an empty one (some dialog
     * backends): cancelled */
    if (!files) {
        LOG("folder dialog failed: %s", SDL_GetError());
        return;
    }
    if (!files[0] || !files[0][0]) {
        LOG("folder dialog cancelled");
        return;
    }
    LOG("folder dialog chose %s", files[0]);
    snprintf(chosen, sizeof chosen, "%s", files[0]);
    SDL_SetAtomicInt(&chosen_ready, 1);
}

void send_addin_start(void)
{
    LOG("start (add-in: %s)", view_have_app && view_app_path[0] ? view_app_path : "none");
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
    LOG("opening the folder dialog at %s", found ? start : "(default)");
    SDL_ShowOpenFolderDialog(folder_done, NULL, view_window, found ? start : NULL, false);
}

static void send_to(const char *dir)
{
    LOG("copying %s to %s", view_app_path, dir);
    struct stat dir_st;
    if (stat(dir, &dir_st) != 0) {
        LOG("%s: %s; nothing done", dir, strerror(errno));
        return;
    }
    if (!S_ISDIR(dir_st.st_mode)) {
        LOG("%s is not a folder; nothing done", dir);
        return;
    }
    const char *base = strrchr(view_app_path, '/');
    base = base ? base + 1 : view_app_path;
    char dst[2200], text[5000];
    snprintf(dst, sizeof dst, "%s/%s", dir, base);
    LOG("target %s", dst);

    bool calculator = looks_like_calculator(dir);
    if (!calculator) {
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
    LOG("source is %lld bytes", (long long)src_st.st_size);
    bool exists = stat(dst, &dst_st) == 0;
    if (exists) {
        LOG("the calculator already has %s (%lld bytes)", base, (long long)dst_st.st_size);
        snprintf(text, sizeof text, "The calculator already has %s (%lld bytes).\n\nReplace it with this one (%lld bytes)?",
                 base, (long long)dst_st.st_size, (long long)src_st.st_size);
        if (!ask("Replace the add-in?", text))
            return;
    }
    struct statvfs fs;
    if (statvfs(dir, &fs) == 0) {
        unsigned long long avail = (unsigned long long)fs.f_bavail * fs.f_frsize;
        unsigned long long need = (unsigned long long)src_st.st_size - (exists ? (unsigned long long)dst_st.st_size : 0);
        LOG("free space %llu bytes, read-only: %s, longest name: %lu", avail,
            fs.f_flag & ST_RDONLY ? "yes" : "no", (unsigned long)fs.f_namemax);
        if (src_st.st_size > (exists ? dst_st.st_size : 0) && avail < need) {
            snprintf(text, sizeof text, "Not enough room on the calculator: %s needs %lld bytes, %llu are free.",
                     base, (long long)src_st.st_size, avail);
            tell(SDL_MESSAGEBOX_ERROR, "Copy failed", text);
            return;
        }
    } else {
        LOG("can't get the free space: %s (going on)", strerror(errno));
    }
    view_flash_status("Copying to the calculator...");
    view_redraw();
    char err[5000];
    if (!copy_file(view_app_path, dst, err, sizeof err)) {
        tell(SDL_MESSAGEBOX_ERROR, "Copy failed", err);
        return;
    }
    if (!calculator) { /* a folder chosen anyway: nothing to eject */
        snprintf(text, sizeof text, "%s was copied to %s.", base, dir);
        tell(SDL_MESSAGEBOX_INFORMATION, "Add-in copied", text);
        view_flash_status("Add-in copied");
        return;
    }
    view_flash_status("Ejecting the calculator...");
    view_redraw();
    char reason[2400] = "";
    if (eject_drive(dir, reason, sizeof reason)) {
        snprintf(text, sizeof text,
                 "%s was copied to the calculator and the drive was ejected.\n\n"
                 "You can unplug the cable; the calculator updates its storage, then the add-in "
                 "appears in the main menu.",
                 base);
        tell(SDL_MESSAGEBOX_INFORMATION, "Add-in copied", text);
        view_flash_status("Add-in copied, calculator ejected");
    } else {
        snprintf(text, sizeof text,
                 "%s was copied to the calculator (%s), but the drive could not be ejected:\n%s\n\n"
                 "Eject it from the file manager before unplugging the cable.",
                 base, dir, reason);
        tell(SDL_MESSAGEBOX_WARNING, "Add-in copied", text);
        view_flash_status("Add-in copied; eject the calculator before unplugging");
    }
}

void send_addin_poll(void)
{
    if (SDL_GetAtomicInt(&chosen_ready)) {
        SDL_SetAtomicInt(&chosen_ready, 0);
        send_to(chosen);
    }
}
