#include "gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLORS 255 /* per frame: the 256th index is the transparent one */
#define HASH_SIZE  8192 /* LZW table (4096 codes at most), open addressing */

typedef struct {
    uint16_t c; /* RGB565 */
    uint32_t n; /* pixels of that colour */
} Color;

struct GifWriter {
    FILE *f;
    int w, h, scale;
    bool ok;
    bool have_prev;
    uint16_t *prev;   /* the last frame */
    uint8_t *indices; /* a frame's rectangle as palette indices, scaled */
    uint32_t *count;  /* pixels per RGB565 colour, 0 between frames */
    uint8_t *lut;     /* palette index per RGB565 colour */
    Color *colors;    /* the frame's colours */
    int32_t keys[HASH_SIZE]; /* LZW: prefix code << 8 | index, -1 = free */
    uint16_t codes[HASH_SIZE];
};

static void put16(FILE *f, int v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static int component(uint16_t c, int axis)
{
    int v;
    switch (axis) {
    case 0: v = c >> 11; return v << 3 | v >> 2;
    case 1: v = (c >> 5) & 0x3F; return v << 2 | v >> 4;
    default: v = c & 0x1F; return v << 3 | v >> 2;
    }
}

GifWriter *gif_open(const char *path, int w, int h, int scale)
{
    GifWriter *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->f = fopen(path, "wb");
    g->prev = malloc((size_t)w * (size_t)h * sizeof *g->prev);
    g->indices = malloc((size_t)w * (size_t)scale * (size_t)h * (size_t)scale);
    g->count = calloc(65536, sizeof *g->count);
    g->lut = malloc(65536);
    g->colors = malloc((size_t)w * (size_t)h * sizeof *g->colors);
    if (!g->f || !g->prev || !g->indices || !g->count || !g->lut || !g->colors) {
        if (g->f)
            fclose(g->f);
        free(g->prev);
        free(g->indices);
        free(g->count);
        free(g->lut);
        free(g->colors);
        free(g);
        return NULL;
    }
    g->w = w;
    g->h = h;
    g->scale = scale;
    g->ok = true;
    fwrite("GIF89a", 1, 6, g->f);
    put16(g->f, w * scale);
    put16(g->f, h * scale);
    fputc(0x70, g->f); /* no global colour table, 8-bit colour resolution */
    fputc(0, g->f);    /* background colour */
    fputc(0, g->f);    /* square pixels */
    static const uint8_t LOOP[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                                   0x03, 0x01, 0x00, 0x00, 0x00}; /* repeat forever */
    fwrite(LOOP, 1, sizeof LOOP, g->f);
    return g;
}

/*---------------------------------------------------------------------------
 * Median cut: colours[0..n) into at most max boxes, each one palette entry
 * (the mean of its pixels).
 *-------------------------------------------------------------------------*/

typedef struct {
    int start, end; /* colours[start..end) */
    int range, axis; /* its longest side and which one (0 red, 1 green, 2 blue) */
} Box;

static int sort_axis;

static int by_axis(const void *a, const void *b)
{
    return component(((const Color *)a)->c, sort_axis) - component(((const Color *)b)->c, sort_axis);
}

static Box make_box(const Color *colors, int start, int end)
{
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = start; i < end; i++) {
        for (int k = 0; k < 3; k++) {
            int v = component(colors[i].c, k);
            if (v < lo[k])
                lo[k] = v;
            if (v > hi[k])
                hi[k] = v;
        }
    }
    Box b = {start, end, -1, 0};
    for (int k = 0; k < 3; k++) {
        if (hi[k] - lo[k] > b.range) {
            b.range = hi[k] - lo[k];
            b.axis = k;
        }
    }
    return b;
}

static int median_cut(GifWriter *g, int n, uint8_t *palette, int max)
{
    Color *colors = g->colors;
    Box boxes[256];
    int count = 1;
    boxes[0] = make_box(colors, 0, n);
    while (count < max) {
        int pick = -1;
        for (int i = 0; i < count; i++)
            if (boxes[i].end - boxes[i].start > 1 && (pick < 0 || boxes[i].range > boxes[pick].range))
                pick = i;
        if (pick < 0)
            break;
        Box b = boxes[pick];
        sort_axis = b.axis;
        qsort(colors + b.start, (size_t)(b.end - b.start), sizeof *colors, by_axis);
        uint64_t total = 0, acc = 0;
        for (int i = b.start; i < b.end; i++)
            total += colors[i].n;
        int cut = b.end - 1; /* split at the median pixel, both halves non-empty */
        for (int i = b.start; i < b.end - 1; i++) {
            acc += colors[i].n;
            if (acc * 2 >= total) {
                cut = i + 1;
                break;
            }
        }
        boxes[pick] = make_box(colors, b.start, cut);
        boxes[count++] = make_box(colors, cut, b.end);
    }
    for (int i = 0; i < count; i++) {
        uint64_t sum[3] = {0, 0, 0}, pixels = 0;
        for (int j = boxes[i].start; j < boxes[i].end; j++) {
            for (int k = 0; k < 3; k++)
                sum[k] += (uint64_t)component(colors[j].c, k) * colors[j].n;
            pixels += colors[j].n;
            g->lut[colors[j].c] = (uint8_t)i;
        }
        for (int k = 0; k < 3; k++)
            palette[3 * i + k] = (uint8_t)((sum[k] + pixels / 2) / pixels);
    }
    return count;
}

/*---------------------------------------------------------------------------
 * LZW, in 255-byte sub-blocks
 *-------------------------------------------------------------------------*/

typedef struct {
    FILE *f;
    uint32_t acc;
    int bits;
    uint8_t block[255];
    int len;
} Bits;

static void put_byte(Bits *b, uint8_t v)
{
    b->block[b->len++] = v;
    if (b->len == 255) {
        fputc(255, b->f);
        fwrite(b->block, 1, 255, b->f);
        b->len = 0;
    }
}

static void put_code(Bits *b, int code, int size)
{
    b->acc |= (uint32_t)code << b->bits;
    b->bits += size;
    while (b->bits >= 8) {
        put_byte(b, (uint8_t)b->acc);
        b->acc >>= 8;
        b->bits -= 8;
    }
}

static void lzw(GifWriter *g, const uint8_t *idx, size_t n, int min_size)
{
    fputc(min_size, g->f);
    Bits b = {.f = g->f};
    int clear = 1 << min_size, size = min_size + 1, last = clear + 1; /* last: the newest code */
    memset(g->keys, 0xFF, sizeof g->keys);
    put_code(&b, clear, size);
    int cur = idx[0];
    for (size_t i = 1; i < n; i++) {
        int32_t key = cur << 8 | idx[i];
        unsigned h = ((uint32_t)key * 2654435761u) >> 19; /* 13 bits */
        while (g->keys[h] >= 0 && g->keys[h] != key)
            h = (h + 1) & (HASH_SIZE - 1);
        if (g->keys[h] == key) {
            cur = g->codes[h];
            continue;
        }
        put_code(&b, cur, size);
        g->keys[h] = key;
        g->codes[h] = (uint16_t)++last;
        if (last >= 1 << size)
            size++;
        if (last == 4095) { /* the table is full: start again */
            put_code(&b, clear, size);
            memset(g->keys, 0xFF, sizeof g->keys);
            size = min_size + 1;
            last = clear + 1;
        }
        cur = idx[i];
    }
    put_code(&b, cur, size);
    /* the decoder adds a code on reading cur, and may widen before the end */
    if (++last >= 1 << size && size < 12)
        size++;
    put_code(&b, clear + 1, size);
    if (b.bits > 0)
        put_byte(&b, (uint8_t)b.acc);
    if (b.len) {
        fputc(b.len, g->f);
        fwrite(b.block, 1, (size_t)b.len, g->f);
    }
    fputc(0, g->f);
}

/*---------------------------------------------------------------------------
 * Frames
 *-------------------------------------------------------------------------*/

bool gif_frame(GifWriter *g, const uint16_t *px, int delay_cs)
{
    if (!g->ok)
        return false;
    int w = g->w, h = g->h, s = g->scale;
    bool keep = g->have_prev; /* pixels that stayed the same are transparent */

    /* The rectangle that changed (one transparent pixel if none did) */
    int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    if (keep) {
        x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (px[y * w + x] != g->prev[y * w + x]) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    y1 = y;
                }
            }
        }
        if (x1 < 0)
            x0 = y0 = x1 = y1 = 0;
    }

    /* Its colours: exact up to MAX_COLORS, else reduced */
    int n = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int i = y * w + x;
            if (keep && px[i] == g->prev[i])
                continue;
            if (!g->count[px[i]]++)
                g->colors[n++].c = px[i];
        }
    }
    for (int k = 0; k < n; k++)
        g->colors[k].n = g->count[g->colors[k].c];
    uint8_t palette[256 * 3];
    int np = n;
    if (n <= MAX_COLORS) {
        for (int k = 0; k < n; k++) {
            g->lut[g->colors[k].c] = (uint8_t)k;
            for (int c = 0; c < 3; c++)
                palette[3 * k + c] = (uint8_t)component(g->colors[k].c, c);
        }
    } else {
        np = median_cut(g, n, palette, MAX_COLORS);
    }
    for (int k = 0; k < n; k++)
        g->count[g->colors[k].c] = 0;
    int transparent = np, bits = 1;
    while (1 << bits < np + (keep ? 1 : 0))
        bits++;

    /* Graphic control: keep the frame under the next one, its delay and
     * transparent index */
    FILE *f = g->f;
    fputc(0x21, f);
    fputc(0xF9, f);
    fputc(4, f);
    fputc(1 << 2 | (keep ? 1 : 0), f);
    put16(f, delay_cs < 1 ? 1 : delay_cs > 65535 ? 65535 : delay_cs);
    fputc(keep ? transparent : 0, f);
    fputc(0, f);

    /* The image: its rectangle and colour table */
    int rw = (x1 - x0 + 1) * s, rh = (y1 - y0 + 1) * s;
    fputc(0x2C, f);
    put16(f, x0 * s);
    put16(f, y0 * s);
    put16(f, rw);
    put16(f, rh);
    fputc(0x80 | (bits - 1), f);
    for (int i = 0; i < 1 << bits; i++)
        for (int c = 0; c < 3; c++)
            fputc(i < np ? palette[3 * i + c] : 0, f);

    uint8_t *out = g->indices;
    for (int y = y0; y <= y1; y++) {
        uint8_t *row = out;
        for (int x = x0; x <= x1; x++) {
            int i = y * w + x;
            uint8_t v = keep && px[i] == g->prev[i] ? (uint8_t)transparent : g->lut[px[i]];
            for (int k = 0; k < s; k++)
                *out++ = v;
        }
        for (int k = 1; k < s; k++, out += rw)
            memcpy(out, row, (size_t)rw);
    }
    lzw(g, g->indices, (size_t)rw * (size_t)rh, bits < 2 ? 2 : bits);

    memcpy(g->prev, px, (size_t)w * (size_t)h * sizeof *px);
    g->have_prev = true;
    if (ferror(f))
        g->ok = false;
    return g->ok;
}

bool gif_close(GifWriter *g)
{
    if (!g)
        return false;
    fputc(0x3B, g->f);
    bool ok = g->ok && !ferror(g->f);
    if (fclose(g->f) != 0)
        ok = false;
    free(g->prev);
    free(g->indices);
    free(g->count);
    free(g->lut);
    free(g->colors);
    free(g);
    return ok;
}
