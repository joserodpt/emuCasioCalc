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

bool png_save_argb(const char *path, const uint32_t *argb, int w, int h)
{
    /* the image data: each row is a filter byte (0) and RGBA pixels */
    size_t row = (size_t)w * 4 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!raw)
        return false;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = argb[y * w + x];
            r[1 + 4 * x] = (uint8_t)(p >> 16);
            r[2 + 4 * x] = (uint8_t)(p >> 8);
            r[3 + 4 * x] = (uint8_t)p;
            r[4 + 4 * x] = (uint8_t)(p >> 24);
        }
    }
    /* zlib stream of stored blocks (at most 65535 bytes each) */
    size_t blocks = raw_n / 65535 + 1, z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n);
    if (!z) {
        free(raw);
        return false;
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n || i == 0;) {
        size_t n = raw_n - i > 65535 ? 65535 : raw_n - i;
        z[o++] = i + n == raw_n ? 1 : 0; /* last block? */
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        o += n;
        for (size_t k = 0; k < n; k++) {
            a = (a + raw[i + k]) % 65521;
            b = (b + a) % 65521;
        }
        i += n;
        if (n == 0)
            break;
    }
    be32(z + o, b << 16 | a);
    o += 4;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bits per channel */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, o) &&
              chunk(f, "IEND", NULL, 0);
    if (f)
        ok = fclose(f) == 0 && ok;
    free(raw);
    free(z);
    return ok;
}
