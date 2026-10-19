/* The emulator as a desktop app (Linux): --install-desktop puts its icon in
 * the user's icon theme and a launcher in the applications menu, so docks,
 * the app grid and Alt-Tab show the icon (they match the window to the
 * launcher by the window class, the app identifier, APP_ID). --export-icon
 * saves the icon as a PNG. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_icon.h"
#include "platform.h"
#include "png.h"

bool desktop_export_icon(const char *path, int size)
{
    uint32_t *px = malloc((size_t)size * (size_t)size * 4);
    if (!px)
        return false;
    app_icon_pixels_size(size, px);
    bool ok = png_save_argb(path, px, size, size);
    free(px);
    return ok;
}

/* mkdir -p */
static bool make_dirs(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(p, 0755) != 0 && errno != EEXIST)
                return false;
            *s = '/';
        }
    }
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}

bool desktop_install(const char *run)
{
#ifdef __APPLE__
    (void)run;
    fprintf(stderr, "--install-desktop is for Linux desktops\n");
    return false;
#else
    const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
    char data[512];
    if (xdg && xdg[0])
        snprintf(data, sizeof data, "%s", xdg);
    else if (home)
        snprintf(data, sizeof data, "%s/.local/share", home);
    else {
        fprintf(stderr, "install: no HOME\n");
        return false;
    }

    /* the icon at the theme's usual sizes */
    static const int SIZES[] = {16, 24, 32, 48, 64, 128, 256, 512};
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        char dir[700], file[800];
        snprintf(dir, sizeof dir, "%s/icons/hicolor/%dx%d/apps", data, SIZES[i], SIZES[i]);
        snprintf(file, sizeof file, "%s/%s.png", dir, APP_ID);
        if (!make_dirs(dir) || !desktop_export_icon(file, SIZES[i])) {
            fprintf(stderr, "install: can't write %s\n", file);
            return false;
        }
        printf("wrote %s\n", file);
    }

    /* the launcher */
    char dir[700], file[800];
    snprintf(dir, sizeof dir, "%s/applications", data);
    snprintf(file, sizeof file, "%s/%s.desktop", dir, APP_ID);
    char folder[1024];
    snprintf(folder, sizeof folder, "%s", run);
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder)
        *slash = '\0';
    else
        snprintf(folder, sizeof folder, "%s", home ? home : "/");
    FILE *f = make_dirs(dir) ? fopen(file, "w") : NULL;
    if (!f) {
        fprintf(stderr, "install: can't write %s\n", file);
        return false;
    }
    fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=emuCasioCalc\n"
            "Comment=Emulator for Casio fx-CG graphing calculator add-ins\n"
            "Exec=\"%s\" %%f\n"
            "Icon=%s\n"
            "Terminal=false\n"
            "Categories=System;Emulator;\n"
            "Keywords=casio;calculator;prizm;fx-cg50;g3a;emulator;\n"
            "StartupWMClass=%s\n"
            "Path=%s\n", /* where storage/ is: the command's folder */
            run, APP_ID, APP_ID, folder);
    bool ok = fclose(f) == 0;
    printf("wrote %s (runs %s)\n", file, run);
    /* let the desktop notice the new icons */
    char cache[800];
    snprintf(cache, sizeof cache, "gtk-update-icon-cache -q -t -f '%s/icons/hicolor' 2>/dev/null", data);
    if (system(cache) != 0)
        printf("(gtk-update-icon-cache not run: icons show after the next login)\n");
    return ok;
#endif
}
