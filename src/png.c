/*
 * Tiny PNG writer (1-bit palette) and base64, for MCP screenshots.
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#include "png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Appends one chunk to out at *pos. */
static void chunk(uint8_t *out, size_t *pos, const char *type, const uint8_t *data,
                  uint32_t len) {
    uint8_t *p = out + *pos;
    put32(p, len);
    memcpy(p + 4, type, 4);
    if (len)
        memcpy(p + 8, data, len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, p + 4, len + 4);
    put32(p + 8 + len, (uint32_t)crc);
    *pos += 12 + len;
}

uint8_t *png_encode_1bit(const uint8_t *rows, int w, int h, int stride,
                         uint32_t rgb0, uint32_t rgb1, size_t *out_len) {
    size_t rowbytes = ((size_t)w + 7) / 8;
    size_t rawlen = (rowbytes + 1) * (size_t)h;
    uint8_t *raw = malloc(rawlen);
    if (!raw)
        return NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (rowbytes + 1);
        r[0] = 0; /* filter: none */
        memcpy(r + 1, rows + (size_t)y * (size_t)stride, rowbytes);
    }

    uLongf zlen = compressBound((uLong)rawlen);
    uint8_t *z = malloc(zlen);
    if (!z || compress2(z, &zlen, raw, (uLong)rawlen, 9) != Z_OK) {
        free(raw);
        free(z);
        return NULL;
    }
    free(raw);

    size_t cap = 8 + (12 + 13) + (12 + 6) + (12 + zlen) + 12;
    uint8_t *out = malloc(cap);
    if (!out) {
        free(z);
        return NULL;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    memcpy(out, sig, 8);
    size_t pos = 8;

    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 1;  /* bit depth */
    ihdr[9] = 3;  /* color type: palette */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    chunk(out, &pos, "IHDR", ihdr, 13);

    uint8_t plte[6] = {(uint8_t)(rgb0 >> 16), (uint8_t)(rgb0 >> 8), (uint8_t)rgb0,
                       (uint8_t)(rgb1 >> 16), (uint8_t)(rgb1 >> 8), (uint8_t)rgb1};
    chunk(out, &pos, "PLTE", plte, 6);
    chunk(out, &pos, "IDAT", z, (uint32_t)zlen);
    chunk(out, &pos, "IEND", NULL, 0);
    free(z);

    *out_len = pos;
    return out;
}

char *base64_encode(const uint8_t *data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out)
        return NULL;
    size_t i = 0, o = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)data[i + 1] << 8;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}
