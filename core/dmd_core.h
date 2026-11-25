/*
 * dmd_core: AT&T / Teletype DMD 5620 terminal emulator core, in C.
 *
 * A plain C port of Seth Morabito's Rust dmd_core 0.7.2, with the same
 * C API. It emulates the WE32100 CPU, the 2681 DUART (host RS-232 line
 * and keyboard), the mouse, the 8 KB NVRAM, 1 MB of RAM and the
 * 800x1024 one-bit display.
 *
 * Copyright 2018-2025 Seth J. Morabito <web@loomcom.com> (Rust original)
 * Copyright 2026 the dmd5620 authors (C port)
 * MIT License, see LICENSE.md.
 *
 * All calls must come from one thread. Return codes: DMD_SUCCESS,
 * DMD_ERROR, or DMD_BUSY (no character waiting).
 */

#ifndef DMD_CORE_H
#define DMD_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMD_SUCCESS 0
#define DMD_ERROR   1
#define DMD_BUSY    2

#define DMD_WIDTH       800
#define DMD_HEIGHT      1024
#define DMD_NVRAM_SIZE  8192
#define DMD_VIDEO_BYTES 0x19000 /* 100 bytes x 1024 lines, MSB = left */

/* version 1 = firmware 8;7;3, 2 = firmware 8;7;5. Loads the ROM and
 * resets the CPU. RAM, NVRAM and the DUART keep their state. */
int dmd_init(uint8_t version);

/* Pointer to the current frame (DMD_VIDEO_BYTES). Clears the dirty flag. */
uint8_t *dmd_video_ram(void);
int dmd_video_ram_dirty(void);

int dmd_step(void);
int dmd_step_loop(size_t steps);

int dmd_get_pc(uint32_t *pc);
int dmd_get_register(uint8_t reg, uint32_t *val);
int dmd_read_word(uint32_t addr, uint32_t *val);
int dmd_read_byte(uint32_t addr, uint8_t *val);
int dmd_get_duart_output_port(uint8_t *oport);

/* Mouse: x and y are free-running counters (the firmware moves its
 * arrow by their change). button: 0 left, 1 middle, 2 right. */
int dmd_mouse_move(uint16_t x, uint16_t y);
int dmd_mouse_down(uint8_t button);
int dmd_mouse_up(uint8_t button);

/* Host line (RS-232) and keyboard. *_rx queues a byte into the
 * terminal; *_tx takes one byte out (DMD_BUSY if there is none). */
int dmd_rs232_rx(uint8_t c);
int dmd_keyboard_rx(uint8_t c);
int dmd_rs232_tx(uint8_t *c);
int dmd_keyboard_tx(uint8_t *c);

int dmd_set_nvram(uint8_t *nvram); /* DMD_NVRAM_SIZE bytes */
int dmd_get_nvram(uint8_t *nvram);

/* ---- Extensions (not in the Rust core) ---- */

/* Time source in nanoseconds for the DUART (baud rate and 60 Hz
 * vertical blank). NULL = the host's monotonic clock. */
void dmd_set_clock(uint64_t (*now_ns)(void));

/* Instruction clock: each instruction takes ns nanoseconds of emulated
 * time (72 = 7.2 MHz WE32100 at about 1.8 clocks per instruction, the
 * rate the front ends run the CPU at). Deterministic, and no host clock
 * calls. 0 = back to the clock above. */
void dmd_set_ns_per_step(uint32_t ns);

/* 1 = behave exactly as the Rust dmd_core 0.7.2, without the fixes of
 * this port (for the difference test). Default 0. The fixes:
 *  - DUART local loopback receives a break as a NUL character with
 *    Received Break and Framing Error, as a 2681 does. Firmware 8;7;3
 *    needs this to finish its power-on I/O test. */
void dmd_set_rust_compat(int on);

/* Called on an internal error the emulation cannot go on from (the Rust
 * core panics there). Default: print the message and abort(). A
 * handler that returns also ends in abort(); it may longjmp out. */
void dmd_set_fatal_handler(void (*handler)(const char *msg));

#ifdef __cplusplus
}
#endif

#endif
