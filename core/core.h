/*
 * Internal declarations of the C dmd_core. Not a public API.
 *
 * Copyright 2018-2025 Seth J. Morabito <web@loomcom.com> (Rust original)
 * Copyright 2026 the dmd5620 authors (C port)
 * MIT License, see LICENSE.md.
 */

#ifndef DMD_CORE_INTERNAL_H
#define DMD_CORE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dmd_core.h"

/* Bus addresses are 64-bit: the Rust core computes some of them in
 * usize, where they do not wrap at 32 bits. */
typedef uint64_t addr_t;

/* Errors (the Rust BusError and CpuException). 0 = no error. */
enum {
    E_OK = 0,
    E_BUS_INIT,
    E_BUS_READ,
    E_BUS_WRITE,
    E_BUS_NODEV,
    E_BUS_RANGE,
    E_BUS_PERMISSION,
    E_BUS_ALIGNMENT,
    E_ILLEGAL_OPCODE,
    E_INVALID_DESCRIPTOR,
    E_PRIVILEGED_OPCODE,
    E_ZERO_DIVIDE,
};

#define TRY(x)                \
    do {                      \
        int e_ = (x);         \
        if (e_ != E_OK)       \
            return e_;        \
    } while (0)

/* fatal.c / dmd.c */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
void core_fatal(const char *fmt, ...);
const char *core_error_name(int e);
uint64_t core_now(void);
extern uint64_t core_time; /* clock at the start of this instruction */
extern bool core_rust_compat; /* true: no fixes, exactly as Rust 0.7.2 */
void core_tick(void);

/* ROM images (rom.c) */
extern const uint8_t lo_rom_v1[0x8000];
extern const uint8_t hi_rom_v1[0x8000];
extern const uint8_t lo_rom_v2[0x10000];
extern const uint8_t hi_rom_v2[0x10000];

/* Unbounded byte FIFO (the Rust VecDeque used as a queue). */
struct byteq {
    uint8_t *buf;
    size_t cap, head, len;
};
void byteq_push(struct byteq *q, uint8_t c);
bool byteq_pop(struct byteq *q, uint8_t *c);
void byteq_free(struct byteq *q);

/* duart.c */
void duart_new(void);
void duart_free(void);
void duart_service(void);
uint8_t duart_get_interrupt(void); /* 0 = none */
int duart_read_byte(addr_t address, uint8_t *val);
int duart_write_byte(addr_t address, uint8_t val);
uint8_t duart_output_port(void);
void duart_rs232_rx(uint8_t c);
void duart_keyboard_rx(uint8_t c);
bool duart_rs232_tx(uint8_t *c);
bool duart_keyboard_tx(uint8_t *c);
void duart_mouse_down(uint8_t button);
void duart_mouse_up(uint8_t button);

/* bus.c */
#define RAM_SIZE   0x100000
#define ROM_SIZE   0x20000
#define BBRAM_SIZE 0x2000
#define RAM_START  0x700000
extern uint8_t bus_ram_buf[RAM_SIZE];
extern uint8_t bus_rom_buf[ROM_SIZE];
extern uint8_t bus_vid_buf[2];
extern bool bus_video_dirty;
void bus_new(void);
int bus_read_byte_slow(addr_t address, uint8_t *val);
int bus_read_half_slow(addr_t address, uint16_t *val);
int bus_read_word_slow(addr_t address, uint32_t *val);
int bus_read_op_half_slow(addr_t address, uint16_t *val);
int bus_read_op_word_slow(addr_t address, uint32_t *val);
int bus_write_byte_slow(addr_t address, uint8_t val);
int bus_write_half_slow(addr_t address, uint16_t val);
int bus_write_word_slow(addr_t address, uint32_t val);
int bus_load(addr_t address, const uint8_t *data, size_t len);
uint8_t *bus_video_ram(void);
bool bus_video_ram_dirty(void);
void bus_mouse_move(uint16_t x, uint16_t y);
void bus_get_nvram(uint8_t *out);
void bus_set_nvram(const uint8_t *in);
uint8_t *bus_ram(void); /* tests */

/* Fast paths for RAM and ROM. Everything else, and every error, goes
 * through the *_slow functions, which follow the Rust core exactly. */

static inline const uint8_t *bus_fast_ptr(addr_t a, addr_t n)
{
    addr_t o = a - RAM_START;
    if (o < RAM_SIZE && o + n <= RAM_SIZE)
        return bus_ram_buf + o;
    if (a < ROM_SIZE && a + n <= ROM_SIZE)
        return bus_rom_buf + a;
    return NULL;
}

static inline void bus_mark_video(addr_t o)
{
    addr_t start = (((addr_t)bus_vid_buf[0] << 8) | bus_vid_buf[1]) * 4;
    if (o >= start && o < start + DMD_VIDEO_BYTES)
        bus_video_dirty = true;
}

static inline int bus_read_byte(addr_t a, uint8_t *val)
{
    const uint8_t *p = bus_fast_ptr(a, 1);
    if (!p)
        return bus_read_byte_slow(a, val);
    *val = p[0];
    return E_OK;
}

static inline int bus_read_half(addr_t a, uint16_t *val)
{
    const uint8_t *p = (a & 1) ? NULL : bus_fast_ptr(a, 2);
    if (!p)
        return bus_read_half_slow(a, val);
    *val = (uint16_t)((p[0] << 8) | p[1]);
    return E_OK;
}

static inline int bus_read_word(addr_t a, uint32_t *val)
{
    const uint8_t *p = (a & 3) ? NULL : bus_fast_ptr(a, 4);
    if (!p)
        return bus_read_word_slow(a, val);
    *val = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return E_OK;
}

static inline int bus_read_op_half(addr_t a, uint16_t *val)
{
    const uint8_t *p = bus_fast_ptr(a, 2);
    if (!p)
        return bus_read_op_half_slow(a, val);
    *val = (uint16_t)(p[0] | (p[1] << 8));
    return E_OK;
}

static inline int bus_read_op_word(addr_t a, uint32_t *val)
{
    const uint8_t *p = bus_fast_ptr(a, 4);
    if (!p)
        return bus_read_op_word_slow(a, val);
    *val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return E_OK;
}

static inline int bus_write_byte(addr_t a, uint8_t val)
{
    addr_t o = a - RAM_START;
    if (o >= RAM_SIZE)
        return bus_write_byte_slow(a, val);
    bus_mark_video(o);
    bus_ram_buf[o] = val;
    return E_OK;
}

static inline int bus_write_half(addr_t a, uint16_t val)
{
    addr_t o = a - RAM_START;
    if ((a & 1) || o >= RAM_SIZE)
        return bus_write_half_slow(a, val);
    bus_mark_video(o);
    bus_ram_buf[o] = (uint8_t)(val >> 8);
    bus_ram_buf[o + 1] = (uint8_t)val;
    return E_OK;
}

static inline int bus_write_word(addr_t a, uint32_t val)
{
    addr_t o = a - RAM_START;
    if ((a & 3) || o >= RAM_SIZE)
        return bus_write_word_slow(a, val);
    bus_mark_video(o);
    bus_ram_buf[o] = (uint8_t)(val >> 24);
    bus_ram_buf[o + 1] = (uint8_t)(val >> 16);
    bus_ram_buf[o + 2] = (uint8_t)(val >> 8);
    bus_ram_buf[o + 3] = (uint8_t)val;
    return E_OK;
}

/* cpu.c */
#define R_FP   9
#define R_AP   10
#define R_PSW  11
#define R_SP   12
#define R_PCBP 13
#define R_ISP  14
#define R_PC   15
extern uint32_t cpu_r[16];
void cpu_new(void);
int cpu_reset(void);
void cpu_step(void);
int cpu_step_with_error(void);

#endif
