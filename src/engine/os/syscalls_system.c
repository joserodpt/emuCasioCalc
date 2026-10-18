/*
 * HLE syscalls: system odds and ends. Restart, screenshots, file type icons,
 * App_InitDlgDescriptor, BCDtoInternal.
 *
 * From WikiPrizm: Restart restarts the calculator at once; TakeScreenshot
 * (and TakeScreenshot2, apparently the same) captures VRAM like SHIFT 7
 * and lets the user save it as a picture; SMEM_MapIconToExt gives an
 * 18x24 RGB565 icon with a magenta (0xF81F) background for a file name, or
 * the folder icon when foldername isn't empty, and a message number for the
 * type's name; App_InitDlgDescriptor's writes to its structure;
 * BCDtoInternal's structures.
 *
 * Our choices: Restart ends the add-in (the calculator would go to its main
 * menu); screenshots are saved at once as \\fls0\CAPTnnnn.bmp (no dialog);
 * the icons are drawn here (a page with the extension, or a folder), and
 * msgno is 0 (the OS's type names are unknown); BCDtoInternal's sign is 1
 * for negative numbers and its unknown field 0 (the BCD layout is the one
 * PRGM_GetKey's results show: exponent + 0x100 in 12 bits, plus 0x500 when
 * negative, then 15 digits).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "display.h"
#include "font.h"
#include "platform.h"
#include "syscalls_impl.h"

/* Restart(): the calculator restarts, so the add-in ends. */
SC(sc_restart)
{
    (void)a;
    printf("add-in called Restart: the calculator would restart\n");
    platform_flash("Restart: the calculator restarted, the add-in ended");
    emu->cpu.state = CPU_EXITED;
    return 0;
}

/* TakeScreenshot() / TakeScreenshot2(): VRAM to the next free
 * \\fls0\CAPTnnnn.bmp. */
SC(sc_take_screenshot)
{
    (void)a;
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/fls0", emu->data_dir);
    mkdir(emu->data_dir, 0755);
    mkdir(dir, 0755);
    struct stat st;
    int n = 1;
    for (; n < 10000; n++) {
        snprintf(path, sizeof path, "%s/CAPT%04d.bmp", dir, n);
        if (stat(path, &st) != 0)
            break;
    }
    static uint16_t pixels[LCD_WIDTH * LCD_HEIGHT];
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
        pixels[i] = (uint16_t)(emu->vram[2 * i] << 8 | emu->vram[2 * i + 1]);
    char msg[64];
    if (platform_save_bmp(path, pixels, LCD_WIDTH, LCD_HEIGHT))
        snprintf(msg, sizeof msg, "Screenshot saved as \\\\fls0\\CAPT%04d.bmp", n);
    else
        snprintf(msg, sizeof msg, "Screenshot could not be saved");
    printf("%s\n", msg);
    platform_flash(msg);
    return 0;
}

#define ICON_W 18
#define ICON_H 24
#define MAGENTA 0xF81F

/* SMEM_MapIconToExt(filename, foldername, unsigned *msgno, iconbuffer):
 * an 18x24 icon (0x1B0 RGB565 values, magenta = transparent): a folder if
 * foldername is a non-empty 16-bit string, else a page showing the file's
 * extension, coloured by kind. Returns 0. */
SC(sc_smem_map_icon)
{
    Memory *mem = &emu->mem;
    bool folder = a[1] && mem_read16(mem, a[1]) != 0;
    char name[64] = "";
    if (a[0])
        sc_string(emu, a[0], name, sizeof name);
    const char *dot = strrchr(name, '.');
    char ext[4] = "";
    if (dot)
        for (int i = 0; i < 3 && dot[1 + i]; i++)
            ext[i] = (char)(dot[1 + i] >= 'a' && dot[1 + i] <= 'z' ? dot[1 + i] - 32 : dot[1 + i]);

    /* drawn in a VRAM-sized scratch buffer, then copied out */
    static uint8_t buf[VRAM_SIZE];
    display_rect(buf, 0, 0, ICON_W - 1, ICON_H - 1, MAGENTA);
    if (folder) {
        display_rect(buf, 1, 5, 8, 7, 0xC440);              /* tab */
        display_rect(buf, 1, 7, 16, 20, 0xC440);            /* back */
        display_rect(buf, 1, 9, 16, 20, 0xFE60);            /* front */
        display_rect(buf, 1, 20, 16, 20, 0x8200);
    } else {
        uint16_t band = 0x6B6D; /* other files: grey */
        if (!strcmp(ext, "G3A")) band = 0x2A7F;                              /* add-ins: blue */
        else if (!strcmp(ext, "G3P") || !strcmp(ext, "BMP")) band = 0x2E65;  /* pictures: green */
        else if (!strcmp(ext, "G3E") || !strcmp(ext, "G3M")) band = 0xE8E4;  /* eActivities, memory: red */
        else if (!strcmp(ext, "TXT")) band = 0xFB80;                         /* text: orange */
        display_rect(buf, 2, 1, 12, 22, 0x0000);            /* outline */
        display_rect(buf, 3, 2, 11, 21, 0xFFFF);
        display_rect(buf, 12, 5, 15, 22, 0x0000);
        display_rect(buf, 12, 6, 14, 21, 0xFFFF);
        display_rect(buf, 12, 1, 12, 5, 0x0000);            /* folded corner */
        display_rect(buf, 13, 2, 13, 2, 0x0000);
        display_rect(buf, 14, 3, 14, 3, 0x0000);
        display_rect(buf, 15, 4, 15, 4, 0x0000);
        display_rect(buf, 1, 13, 16, 21, band);              /* band across the page */
        if (ext[0]) {
            int w = display_text_prop(buf, &FONT_TINY, 0, 0, 8, 0, ext, 0, 0, false, false, LCD_WIDTH, false);
            display_text_prop(buf, &FONT_TINY, 1 + (16 - w) / 2, 13, 8, 0, ext, 0xFFFF, 0, false, false,
                              ICON_W, true);
        }
    }
    if (a[3])
        for (int y = 0; y < ICON_H; y++)
            for (int x = 0; x < ICON_W; x++)
                mem_write16(mem, a[3] + (uint32_t)(y * ICON_W + x) * 2, display_get_pixel(buf, x, y));
    if (a[2])
        mem_write32(mem, a[2], 0);
    return 0;
}

/* App_InitDlgDescriptor(void *P1, unsigned char P2): initialises the
 * structure as WikiPrizm describes (its use is unknown). */
SC(sc_app_init_dlg_descriptor)
{
    Memory *mem = &emu->mem;
    uint32_t p = a[0];
    mem_write8(mem, p, (uint8_t)a[1]);
    mem_write16(mem, p + 2, 0);
    for (uint32_t i = 0; i < 13; i++)
        mem_write8(mem, p + 4 + i, 0);
    for (uint32_t i = 0; i < 6; i++)
        mem_write16(mem, p + 0x12 + 2 * i, 0xFFFF);
    mem_write16(mem, p + 0x20, 0);
    mem_write32(mem, p + 0x24, 0);
    mem_write32(mem, p + 0x28, 0);
    mem_write8(mem, p + 0x2C, 0);
    mem_write8(mem, p + 0x2D, 0);
    return 0;
}

/* BCDtoInternal(struct BCDinternal *target, struct BCDvalue *source):
 * target is int exponent, int sign, int unknown, char mantissa[15] (one
 * digit per byte). Returns 0, or -1 if a digit isn't 0-9. */
SC(sc_bcd_to_internal)
{
    Memory *mem = &emu->mem;
    uint32_t src = a[1], dst = a[0];
    uint16_t head = mem_read16(mem, src);
    int e12 = head >> 4;
    bool negative = e12 >= 0x500;
    if (negative)
        e12 -= 0x500;
    uint8_t digits[15];
    digits[0] = head & 15;
    for (int i = 0; i < 7; i++) {
        uint8_t b = mem_read8(mem, src + 2 + (uint32_t)i);
        digits[1 + 2 * i] = b >> 4;
        digits[2 + 2 * i] = b & 15;
    }
    for (int i = 0; i < 15; i++)
        if (digits[i] > 9)
            return (uint32_t)-1;
    mem_write32(mem, dst, (uint32_t)(e12 - 0x100));
    mem_write32(mem, dst + 4, negative ? 1 : 0);
    mem_write32(mem, dst + 8, 0);
    for (int i = 0; i < 15; i++)
        mem_write8(mem, dst + 12 + (uint32_t)i, digits[i]);
    return 0;
}
