/*
 * This file is part of the SDL DMD 5620 Emultor.
 *
 * Copyright 2025, Seth Morabito <web@loomcom.com>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __DMD_5620_H__
#define __DMD_5620_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define WIDTH          800
#define HEIGHT         1024
#define WIDTH_IN_BYTES 100
#define NVRAM_SIZE     (2 << 12)
#define TX_BUF_LEN     64

struct color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

static const struct color COLOR_LIGHT = { 0, 255, 0, 255 };
static const struct color COLOR_DARK = { 0, 0, 0, 255 };

#include "../core/dmd_core.h"

bool sdl_init(const char *title);
void sdl_teardown();
void sdl_main_loop();
void int_handler(int signal);

#endif
