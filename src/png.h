/*
 * Tiny PNG writer (1-bit palette) and base64, for MCP screenshots.
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#ifndef __PNG_H__
#define __PNG_H__

#include <stddef.h>
#include <stdint.h>

/*
 * Encode a 1-bit image. Rows are packed MSB first, `stride` bytes per
 * row. Palette index 0 = rgb0, index 1 = rgb1 (0xRRGGBB).
 * Returns a malloc'd buffer, or NULL.
 */
uint8_t *png_encode_1bit(const uint8_t *rows, int w, int h, int stride,
                         uint32_t rgb0, uint32_t rgb1, size_t *out_len);

/* Returns a malloc'd NUL-terminated base64 string. */
char *base64_encode(const uint8_t *data, size_t len);

#endif
