/*
 * Tiny PNG writer (1-bit palette) and base64, for MCP screenshots.
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#include "png.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CRC-32 (PNG chunks) and Adler-32 (zlib stream)                      */

static uint32_t crc_table[256];

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32_buf(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n > 0) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------------ */
/* zlib stream: one deflate block with the fixed Huffman codes and     */
/* LZ77 matches. The DMD screen is mostly long runs, so this does      */
/* nearly as well as zlib.                                             */

#define WSIZE     32768
#define HBITS     15
#define MAX_CHAIN 64

typedef struct {
    uint8_t *out;
    size_t pos, cap;
    uint64_t bits;
    int nbits;
} bitw;

static void put_bits(bitw *w, uint32_t v, int n) {
    w->bits |= (uint64_t)v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (w->pos < w->cap)
            w->out[w->pos] = (uint8_t)w->bits;
        w->pos++;
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

/* Huffman codes go most significant bit first. */
static void put_code(bitw *w, uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++)
        r |= ((code >> i) & 1) << (len - 1 - i);
    put_bits(w, r, len);
}

static void put_sym(bitw *w, int sym) {
    if (sym < 144)
        put_code(w, 0x30 + (uint32_t)sym, 8);
    else if (sym < 256)
        put_code(w, 0x190 + (uint32_t)(sym - 144), 9);
    else if (sym < 280)
        put_code(w, (uint32_t)(sym - 256), 7);
    else
        put_code(w, 0xc0 + (uint32_t)(sym - 280), 8);
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                      15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                      67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                       17,   25,   33,   49,   65,   97,    129,   193,
                                       257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                       4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(bitw *w, int len, int dist) {
    int i = 28;
    while (len_base[i] > len)
        i--;
    put_sym(w, 257 + i);
    if (len_extra[i])
        put_bits(w, (uint32_t)(len - len_base[i]), len_extra[i]);
    i = 29;
    while (dist_base[i] > dist)
        i--;
    put_code(w, (uint32_t)i, 5);
    if (dist_extra[i])
        put_bits(w, (uint32_t)(dist - dist_base[i]), dist_extra[i]);
}

static uint32_t hash3(const uint8_t *p) {
    return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & ((1u << HBITS) - 1);
}

/* Returns a malloc'd zlib stream of src, or NULL. */
static uint8_t *zlib_compress(const uint8_t *src, size_t n, size_t *zlen) {
    bitw w = {0};
    w.cap = n + n / 8 + 64; /* 9 bits a byte at worst */
    w.out = malloc(w.cap);
    int32_t *head = malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = malloc(sizeof(int32_t) * WSIZE);
    if (!w.out || !head || !prev) {
        free(w.out);
        free(head);
        free(prev);
        return NULL;
    }
    for (size_t i = 0; i < (1u << HBITS); i++)
        head[i] = -1;

    w.out[w.pos++] = 0x78; /* deflate, 32K window */
    w.out[w.pos++] = 0x01;
    put_bits(&w, 1, 1); /* last block */
    put_bits(&w, 1, 2); /* fixed Huffman codes */

    size_t i = 0;
    while (i < n) {
        int best = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = hash3(src + i);
            int32_t cand = head[h];
            int chain = MAX_CHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;
            while (cand >= 0 && i - (size_t)cand <= WSIZE && chain-- > 0) {
                size_t l = 0;
                while (l < maxlen && src[cand + l] == src[i + l])
                    l++;
                if ((int)l > best) {
                    best = (int)l;
                    best_dist = (int)(i - (size_t)cand);
                    if (l == maxlen)
                        break;
                }
                cand = prev[cand % WSIZE];
            }
        }
        size_t step = best >= 3 ? (size_t)best : 1;
        if (best >= 3)
            put_match(&w, best, best_dist);
        else
            put_sym(&w, src[i]);
        /* add the positions we passed to the hash chains */
        for (size_t k = 0; k < step; k++, i++) {
            if (i + 3 <= n) {
                uint32_t h = hash3(src + i);
                prev[i % WSIZE] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    put_sym(&w, 256);         /* end of block */
    put_bits(&w, 0, 7);       /* flush to a byte */
    w.nbits = 0;
    free(head);
    free(prev);

    uint32_t ad = adler32_buf(src, n);
    if (w.pos + 4 > w.cap) {
        free(w.out);
        return NULL;
    }
    w.out[w.pos++] = (uint8_t)(ad >> 24);
    w.out[w.pos++] = (uint8_t)(ad >> 16);
    w.out[w.pos++] = (uint8_t)(ad >> 8);
    w.out[w.pos++] = (uint8_t)ad;
    *zlen = w.pos;
    return w.out;
}

/* ------------------------------------------------------------------ */
/* PNG                                                                 */

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
    put32(p + 8 + len, crc32_buf(0, p + 4, len + 4));
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

    size_t zlen;
    uint8_t *z = zlib_compress(raw, rawlen, &zlen);
    free(raw);
    if (!z)
        return NULL;

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
