/*
 * The C API of the DMD 5620 core. Port of dmd.rs.
 *
 * Copyright 2018-2025 Seth J. Morabito <web@loomcom.com> (Rust original)
 * Copyright 2026 the dmd5620 authors (C port)
 * MIT License, see LICENSE.md.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "core.h"

uint64_t core_time; /* clock at the start of this instruction */
bool core_rust_compat;

void dmd_set_rust_compat(int on)
{
    core_rust_compat = on != 0;
}

static bool created;
static uint64_t (*clock_fn)(void);
static void (*fatal_fn)(const char *msg);

/* ---- Clock ---- */

static uint64_t host_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000000ULL +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000000ULL / (uint64_t)freq.QuadPart;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); /* as Rust's Instant */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/*
 * Time for the DUART. Three sources:
 *  - instruction clock (dmd_set_ns_per_step): every instruction adds a
 *    fixed time. Deterministic and cheapest. The front end uses it.
 *  - a clock function (dmd_set_clock): read before every instruction.
 *  - the host clock (default, as the Rust core): read every
 *    HOST_CLOCK_EVERY instructions, because reading it costs as much as
 *    an instruction.
 */
#define HOST_CLOCK_EVERY 32
static uint32_t ns_per_step;
static unsigned host_countdown;

uint64_t core_now(void)
{
    if (ns_per_step)
        return core_time;
    return clock_fn ? clock_fn() : host_now();
}

/* Called at the start of every instruction */
void core_tick(void)
{
    if (ns_per_step) {
        core_time += ns_per_step;
    } else if (clock_fn) {
        core_time = clock_fn();
    } else if (host_countdown == 0) {
        core_time = host_now();
        host_countdown = HOST_CLOCK_EVERY - 1;
    } else {
        host_countdown--;
    }
}

void dmd_set_clock(uint64_t (*now_ns)(void))
{
    clock_fn = now_ns;
    ns_per_step = 0;
    core_time = core_now();
}

void dmd_set_ns_per_step(uint32_t ns)
{
    ns_per_step = ns;
}

/* ---- Errors ---- */

const char *core_error_name(int e)
{
    switch (e) {
    case E_OK: return "OK";
    case E_BUS_INIT: return "Could not initialize bus";
    case E_BUS_READ: return "Could not read from bus";
    case E_BUS_WRITE: return "Could not write to bus";
    case E_BUS_NODEV: return "No device";
    case E_BUS_RANGE: return "Address out of range";
    case E_BUS_PERMISSION: return "Invalid permission";
    case E_BUS_ALIGNMENT: return "Memory Alignment";
    case E_ILLEGAL_OPCODE: return "Illegal Opcode";
    case E_INVALID_DESCRIPTOR: return "Invalid Descriptor";
    case E_PRIVILEGED_OPCODE: return "Privileged Opcode";
    case E_ZERO_DIVIDE: return "Integer Zero Divide";
    default: return "Unknown error";
    }
}

void dmd_set_fatal_handler(void (*handler)(const char *msg))
{
    fatal_fn = handler;
}

void core_fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (fatal_fn)
        fatal_fn(msg);
    fprintf(stderr, "dmd_core: fatal: %s\n", msg);
    abort();
}

/* ---- The machine ---- */

/* The Rust core creates its state on first use (lazy_static). */
static void ensure_created(void)
{
    if (created)
        return;
    created = true;
    cpu_new();
    bus_new();
    duart_new();
}

int dmd_init(uint8_t version)
{
    ensure_created();
    if (version == 1) {
        if (bus_load(0, lo_rom_v1, sizeof lo_rom_v1) || bus_load(sizeof lo_rom_v1, hi_rom_v1, sizeof hi_rom_v1))
            return DMD_ERROR;
    } else {
        if (bus_load(0, lo_rom_v2, sizeof lo_rom_v2) || bus_load(sizeof lo_rom_v2, hi_rom_v2, sizeof hi_rom_v2))
            return DMD_ERROR;
    }
    return cpu_reset() == E_OK ? DMD_SUCCESS : DMD_ERROR;
}

uint8_t *dmd_video_ram(void)
{
    ensure_created();
    return bus_video_ram();
}

int dmd_video_ram_dirty(void)
{
    ensure_created();
    return bus_video_ram_dirty() ? 1 : 0;
}

int dmd_step(void)
{
    ensure_created();
    cpu_step();
    return DMD_SUCCESS;
}

int dmd_step_loop(size_t steps)
{
    ensure_created();
    host_countdown = 0; /* fresh host clock after the caller's sleep */
    for (size_t i = 0; i < steps; i++)
        cpu_step();
    return DMD_SUCCESS;
}

int dmd_get_pc(uint32_t *pc)
{
    ensure_created();
    *pc = cpu_r[R_PC];
    return DMD_SUCCESS;
}

int dmd_get_register(uint8_t reg, uint32_t *val)
{
    ensure_created();
    *val = cpu_r[reg & 0xf];
    return DMD_SUCCESS;
}

int dmd_read_word(uint32_t addr, uint32_t *val)
{
    ensure_created();
    return bus_read_word(addr, val) == E_OK ? DMD_SUCCESS : DMD_ERROR;
}

int dmd_read_byte(uint32_t addr, uint8_t *val)
{
    ensure_created();
    return bus_read_byte(addr, val) == E_OK ? DMD_SUCCESS : DMD_ERROR;
}

int dmd_get_duart_output_port(uint8_t *oport)
{
    ensure_created();
    *oport = duart_output_port();
    return DMD_SUCCESS;
}

int dmd_mouse_move(uint16_t x, uint16_t y)
{
    ensure_created();
    bus_mouse_move(x, y);
    return DMD_SUCCESS;
}

int dmd_mouse_down(uint8_t button)
{
    ensure_created();
    duart_mouse_down(button);
    return DMD_SUCCESS;
}

int dmd_mouse_up(uint8_t button)
{
    ensure_created();
    duart_mouse_up(button);
    return DMD_SUCCESS;
}

int dmd_rs232_rx(uint8_t c)
{
    ensure_created();
    duart_rs232_rx(c);
    return DMD_SUCCESS;
}

int dmd_keyboard_rx(uint8_t c)
{
    ensure_created();
    duart_keyboard_rx(c);
    return DMD_SUCCESS;
}

int dmd_rs232_tx(uint8_t *c)
{
    ensure_created();
    return duart_rs232_tx(c) ? DMD_SUCCESS : DMD_BUSY;
}

int dmd_keyboard_tx(uint8_t *c)
{
    ensure_created();
    return duart_keyboard_tx(c) ? DMD_SUCCESS : DMD_BUSY;
}

int dmd_set_nvram(uint8_t *nvram)
{
    ensure_created();
    bus_set_nvram(nvram);
    return DMD_SUCCESS;
}

int dmd_get_nvram(uint8_t *nvram)
{
    ensure_created();
    bus_get_nvram(nvram);
    return DMD_SUCCESS;
}

/* For tests: forget all state, as a new Rust Dmd would. */
void dmd_core_test_recreate(void)
{
    created = false;
    ensure_created();
}
