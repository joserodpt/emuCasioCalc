#include "casio_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"

#define DUMP_BASE    0x80000000u
#define OS_HEADER    0x00020000u  /* "CASIOWIN", then the version at +0x20 */
#define LOOKUP_STACK 0x8C100000u  /* the private CPU's RAM: stack below, */
#define LOOKUP_INFO  0x8C100000u  /* the glyph info the lookup writes here */
#define LOOKUP_RAM   0x1000u

/* The glyph lookup syscall of each font, and its glyphs' height */
static const struct {
    uint16_t syscall;
    int h;
} FONTS[CFONT_COUNT] = {
    [CFONT_BIG] = {0x01E8, 24},
    [CFONT_MINI] = {0x01E9, 18},
    [CFONT_TINY] = {0x01EA, 11},
    [CFONT_TINY_BOLD] = {0x01EB, 11},
};

/* The characters the OS draws: single bytes, and the 2-byte ones after
 * these lead bytes (as PrintMini reads strings) */
static const uint8_t PAGES[] = {0x00, 0x7F, 0xE5, 0xE6, 0xE7, 0xF7, 0xF9};
#define PAGE_COUNT_ (int)sizeof PAGES

typedef struct {
    uint32_t addr; /* in the dump, 0: no glyph */
    uint8_t w;
} Entry;

static Entry table[CFONT_COUNT][PAGE_COUNT_][256];
static uint8_t *bits;       /* the dump's bytes from bits_addr, all glyphs */
static uint32_t bits_addr, bits_len;
static char os_version[16];

static int page_of(uint8_t lead)
{
    for (int i = 0; i < PAGE_COUNT_; i++)
        if (PAGES[i] == lead)
            return i;
    return -1;
}

static void fail(char *err, size_t errlen, const char *msg)
{
    snprintf(err, errlen, "%s", msg);
}

/* Runs the dump's function fn(code, &info) on cpu; its result and info. */
static bool lookup(Cpu *cpu, Memory *mem, uint32_t fn, uint16_t code, uint32_t *addr, uint16_t *info)
{
    mem_write16(mem, LOOKUP_INFO, 0);
    cpu_reset(cpu, mem, fn, LOOKUP_STACK + LOOKUP_RAM - 64);
    cpu->pr = CALL_RETURN;
    cpu->r[4] = code;
    cpu->r[5] = LOOKUP_INFO;
    cpu_run(cpu, 100000);
    if (cpu->state != CPU_RETURNED)
        return false;
    *addr = cpu->r[0];
    *info = mem_read16(mem, LOOKUP_INFO);
    return true;
}

bool casio_fonts_load(const uint8_t *dump, size_t size, char *err, size_t errlen)
{
    if (size < 0x00200000 || memcmp(dump + OS_HEADER, "CASIOWIN", 8)) {
        fail(err, errlen, "not a dump of the calculator's OS (no CASIOWIN header at 0x80020000)");
        return false;
    }
    /* The syscall dispatcher at 0x80020070 starts with mov.l @(disp,pc),r2
     * loading the table's address */
    uint32_t entry = SYSCALL_ENTRY - DUMP_BASE;
    uint16_t op = (uint16_t)be_read(dump + entry, 2);
    uint32_t table_lit = ((entry + 4) & ~3u) + (op & 0xFFu) * 4;
    uint32_t sc_table = (op & 0xFF00) == 0xD200 ? be_read(dump + table_lit, 4) - DUMP_BASE : UINT32_MAX;
    if (sc_table >= size - 0x2000 * 4) {
        fail(err, errlen, "the OS dump's syscall table wasn't found");
        return false;
    }

    Memory *mem = malloc(sizeof *mem);
    Cpu *cpu = calloc(1, sizeof *cpu);
    Entry(*found)[PAGE_COUNT_][256] = calloc(CFONT_COUNT, sizeof *found);
    if (!mem || !cpu || !found || !mem_init(mem)) {
        free(mem);
        free(cpu);
        free(found);
        fail(err, errlen, "out of memory");
        return false;
    }
    /* Mapped read-only: the lookups only read the dump */
    mem_map(mem, DUMP_BASE, (uint32_t)size & ~(PAGE_SIZE - 1), (uint8_t *)dump, false);
    mem_add_ram(mem, LOOKUP_STACK, LOOKUP_RAM, true);

    uint32_t lo = UINT32_MAX, hi = 0;
    int glyphs = 0;
    bool ok = true;
    for (int f = 0; f < CFONT_COUNT && ok; f++) {
        uint32_t fn = be_read(dump + sc_table + 4u * FONTS[f].syscall, 4);
        for (int p = 0; p < PAGE_COUNT_ && ok; p++) {
            for (int c = 0; c < 256; c++) {
                uint16_t code = (uint16_t)(PAGES[p] << 8 | c);
                uint32_t addr;
                uint16_t w;
                if (!lookup(cpu, mem, fn, code, &addr, &w)) {
                    ok = false;
                    break;
                }
                uint32_t len = (uint32_t)FONTS[f].h * ((w + 7u) / 8);
                if (!addr || w > 32 || addr < DUMP_BASE || addr - DUMP_BASE + len > size)
                    continue; /* no glyph (or nothing we could draw) */
                found[f][p][c] = (Entry){addr, (uint8_t)w};
                glyphs++;
                if (addr < lo)
                    lo = addr;
                if (addr + len > hi)
                    hi = addr + len;
            }
        }
    }
    mem_free(mem);
    free(mem);
    free(cpu);
    if (!ok || !glyphs) {
        free(found);
        fail(err, errlen, "the OS dump's glyph lookups didn't run: not an fx-CG OS?");
        return false;
    }

    uint8_t *copy = malloc(hi - lo);
    if (!copy) {
        free(found);
        fail(err, errlen, "out of memory");
        return false;
    }
    memcpy(copy, dump + (lo - DUMP_BASE), hi - lo);
    free(bits);
    bits = copy;
    bits_addr = lo;
    bits_len = hi - lo;
    memcpy(table, found, sizeof table);
    free(found);
    const char *v = (const char *)dump + OS_HEADER + 0x20;
    snprintf(os_version, sizeof os_version, "%.10s", v);
    return true;
}

bool casio_fonts_load_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "can't open %s", path);
        return false;
    }
    /* The OS is in the first 8 MB; a full ROM dump has the storage after */
    size_t cap = 0x00800000, size;
    uint8_t *dump = malloc(cap);
    if (!dump) {
        fclose(f);
        fail(err, errlen, "out of memory");
        return false;
    }
    size = fread(dump, 1, cap, f);
    fclose(f);
    bool ok = casio_fonts_load(dump, size, err, errlen);
    free(dump);
    return ok;
}

bool casio_fonts_loaded(void)
{
    return bits != NULL;
}

const char *casio_fonts_os_version(void)
{
    return os_version;
}

bool casio_glyph(CasioFontId font, uint16_t code, CasioGlyph *out)
{
    int p = page_of((uint8_t)(code >> 8));
    if (!bits || font < 0 || font >= CFONT_COUNT || p < 0)
        return false;
    const Entry *e = &table[font][p][code & 0xFF];
    if (!e->addr)
        return false;
    out->bits = bits + (e->addr - bits_addr);
    out->w = e->w;
    out->h = FONTS[font].h;
    out->stride = (e->w + 7) / 8;
    return true;
}
