#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t head[8];
    be32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    uint32_t c = crc(crc(0, head + 4, 4), data, n);
    uint8_t tail[4];
    be32(tail, c);
    return fwrite(head, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(tail, 1, 4, f) == 4;
}

/*---------------------------------------------------------------------------
 * Deflate: one block of LZ77 literals and matches in the fixed Huffman
 * codes, in a zlib stream
 *-------------------------------------------------------------------------*/

#define WINDOW    32768
#define HASH_BITS 15
#define MAX_CHAIN 64  /* earlier positions tried per match */
#define MAX_MATCH 258

static const uint16_t LEN_BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                                       33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                                       1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

typedef struct {
    uint8_t *out;
    size_t n, cap;
    uint32_t acc;
    int bits;
    bool ok;
} Bits;

static void put_byte(Bits *b, uint8_t v)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        uint8_t *p = realloc(b->out, cap);
        if (!p) {
            b->ok = false;
            return;
        }
        b->out = p;
        b->cap = cap;
    }
    b->out[b->n++] = v;
}

static void put_bits(Bits *b, uint32_t v, int n)
{
    b->acc |= v << b->bits;
    b->bits += n;
    while (b->bits >= 8) {
        put_byte(b, (uint8_t)b->acc);
        b->acc >>= 8;
        b->bits -= 8;
    }
}

/* Huffman codes go most significant bit first. */
static void put_code(Bits *b, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    put_bits(b, r, n);
}

static void put_symbol(Bits *b, int v)
{
    if (v <= 143)
        put_code(b, 0x30 + (uint32_t)v, 8);
    else if (v <= 255)
        put_code(b, 0x190 + (uint32_t)(v - 144), 9);
    else if (v <= 279)
        put_code(b, (uint32_t)(v - 256), 7);
    else
        put_code(b, 0xC0 + (uint32_t)(v - 280), 8);
}

static void put_match(Bits *b, int len, int dist)
{
    int i = 28;
    while (LEN_BASE[i] > len)
        i--;
    put_symbol(b, 257 + i);
    put_bits(b, (uint32_t)(len - LEN_BASE[i]), LEN_EXTRA[i]);
    int d = 29;
    while (DIST_BASE[d] > dist)
        d--;
    put_code(b, (uint32_t)d, 5);
    put_bits(b, (uint32_t)(dist - DIST_BASE[d]), DIST_EXTRA[d]);
}

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

/* in[0..n) as a zlib stream, in *out (malloc'd); its size, 0 on failure. */
static size_t zlib_compress(const uint8_t *in, size_t n, uint8_t **out)
{
    Bits b = {.ok = true};
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS), *prev = malloc(sizeof(int32_t) * WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    memset(head, 0xFF, sizeof(int32_t) << HASH_BITS);
    put_bits(&b, 0x78, 8); /* deflate, 32 kB window */
    put_bits(&b, 0x01, 8);
    put_bits(&b, 1, 1); /* the last block */
    put_bits(&b, 1, 2); /* fixed codes */
    size_t i = 0;
    while (i < n) {
        size_t best = 0, dist = 0;
        if (i + 3 <= n) {
            size_t max = n - i < MAX_MATCH ? n - i : MAX_MATCH;
            uint32_t h = hash3(in + i);
            int32_t p = head[h];
            for (int chain = MAX_CHAIN; p >= 0 && i - (size_t)p <= WINDOW && chain > 0; chain--) {
                if (in[p + best] == in[i + best]) {
                    size_t l = 0;
                    while (l < max && in[p + l] == in[i + l])
                        l++;
                    if (l > best) {
                        best = l;
                        dist = i - (size_t)p;
                        if (l == max)
                            break;
                    }
                }
                int32_t q = prev[p & (WINDOW - 1)];
                if (q >= p)
                    break;
                p = q;
            }
        }
        size_t step = best >= 3 ? best : 1;
        if (best >= 3)
            put_match(&b, (int)best, (int)dist);
        else
            put_symbol(&b, in[i]);
        for (size_t k = 0; k < step; k++, i++) { /* remember the positions passed */
            if (i + 3 <= n) {
                uint32_t h = hash3(in + i);
                prev[i & (WINDOW - 1)] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    put_symbol(&b, 256); /* end of block */
    if (b.bits > 0)
        put_bits(&b, 0, 8 - b.bits);
    uint32_t a = 1, s = 0;
    for (size_t k = 0; k < n; k++) {
        a = (a + in[k]) % 65521;
        s = (s + a) % 65521;
    }
    put_bits(&b, (s >> 8) & 0xFF, 8); /* Adler-32, big-endian */
    put_bits(&b, s & 0xFF, 8);
    put_bits(&b, (a >> 8) & 0xFF, 8);
    put_bits(&b, a & 0xFF, 8);
    free(head);
    free(prev);
    if (!b.ok) {
        free(b.out);
        return 0;
    }
    *out = b.out;
    return b.n;
}

/*---------------------------------------------------------------------------
 * Files
 *-------------------------------------------------------------------------*/

/* raw: the filtered rows (a filter byte, then the pixels). color_type: 2
 * RGB, 3 palette (plte: n_colors RGB triples), 6 RGBA. */
static bool write_png(const char *path, int w, int h, int color_type, const uint8_t *raw, size_t raw_n,
                      const uint8_t *plte, int n_colors)
{
    uint8_t *z = NULL;
    size_t z_n = zlib_compress(raw, raw_n, &z);
    if (!z_n)
        return false;
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; /* bits per channel or index */
    ihdr[9] = (uint8_t)color_type;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(SIG, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              (color_type != 3 || chunk(f, "PLTE", plte, (size_t)n_colors * 3)) && chunk(f, "IDAT", z, z_n) &&
              chunk(f, "IEND", NULL, 0);
    if (f)
        ok = fclose(f) == 0 && ok;
    free(z);
    return ok;
}

bool png_save_argb(const char *path, const uint32_t *argb, int w, int h)
{
    size_t row = (size_t)w * 4 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!raw)
        return false;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0; /* no filter */
        for (int x = 0; x < w; x++) {
            uint32_t p = argb[y * w + x];
            r[1 + 4 * x] = (uint8_t)(p >> 16);
            r[2 + 4 * x] = (uint8_t)(p >> 8);
            r[3 + 4 * x] = (uint8_t)p;
            r[4 + 4 * x] = (uint8_t)(p >> 24);
        }
    }
    bool ok = write_png(path, w, h, 6, raw, raw_n, NULL, 0);
    free(raw);
    return ok;
}

static void rgb(uint16_t c, uint8_t *out)
{
    int r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
    out[0] = (uint8_t)(r << 3 | r >> 2);
    out[1] = (uint8_t)(g << 2 | g >> 4);
    out[2] = (uint8_t)(b << 3 | b >> 2);
}

bool png_save_rgb565(const char *path, const uint16_t *px, int w, int h, int scale)
{
    /* a palette if the colours fit in one */
    uint16_t *index = malloc(65536 * sizeof *index);
    if (!index)
        return false;
    memset(index, 0xFF, 65536 * sizeof *index);
    uint8_t plte[256 * 3];
    int n_colors = 0;
    for (int i = 0; i < w * h && n_colors <= 256; i++) {
        if (index[px[i]] == 0xFFFF) {
            if (n_colors < 256)
                rgb(px[i], plte + 3 * n_colors);
            index[px[i]] = (uint16_t)n_colors++;
        }
    }
    bool palette = n_colors <= 256;
    int sw = w * scale, sh = h * scale, bpp = palette ? 1 : 3;
    size_t row = (size_t)sw * (size_t)bpp + 1, raw_n = row * (size_t)sh;
    uint8_t *raw = malloc(raw_n), *line = malloc(row), *above = calloc(row, 1); /* unfiltered rows */
    if (!raw || !line || !above) {
        free(index);
        free(raw);
        free(line);
        free(above);
        return false;
    }
    for (int y = 0; y < sh; y++) {
        uint8_t *r = raw + (size_t)y * row;
        const uint16_t *src = px + (size_t)(y / scale) * (size_t)w;
        for (int x = 0; x < sw; x++) {
            if (palette)
                line[1 + x] = (uint8_t)index[src[x / scale]];
            else
                rgb(src[x / scale], line + 1 + 3 * x);
        }
        /* palette rows unfiltered; RGB rows with whichever of none, Sub
         * and Up leaves the smallest values */
        int best = 0;
        if (!palette) {
            long sums[3] = {0, 0, 0};
            for (size_t k = 1; k < row; k++) {
                uint8_t v = line[k], left = k > 3 ? line[k - 3] : 0;
                sums[0] += abs((int8_t)v);
                sums[1] += abs((int8_t)(uint8_t)(v - left));
                sums[2] += abs((int8_t)(uint8_t)(v - above[k]));
            }
            best = sums[1] < sums[best] ? 1 : best;
            best = sums[2] < sums[best] ? 2 : best;
        }
        r[0] = (uint8_t)best;
        for (size_t k = 1; k < row; k++) {
            uint8_t v = line[k];
            if (best == 1)
                v = (uint8_t)(v - (k > 3 ? line[k - 3] : 0));
            else if (best == 2)
                v = (uint8_t)(v - above[k]);
            r[k] = v;
        }
        uint8_t *t = above; /* this row is the next one's above */
        above = line;
        line = t;
    }
    bool ok = write_png(path, sw, sh, palette ? 3 : 2, raw, raw_n, plte, palette ? n_colors : 0);
    free(index);
    free(raw);
    free(line);
    free(above);
    return ok;
}
