/*
 * difftest: run the C core and the Rust dmd_core 0.7.2 side by side and
 * compare them after every instruction.
 *
 * The Rust core is built from upstream with rust_core.patch: a fake
 * clock (rs_set_time), exports renamed to rs_dmd_*, and a few test
 * hooks. Both cores get the same clock, so the DUART timing is the same.
 *
 *   difftest boot FW STEPS [SEED [NS_PER_STEP]]
 *       Boot firmware FW (1 or 2), feed the same host text, keys and
 *       mouse moves to both, compare registers and I/O every step and
 *       all memory every 65536 steps.
 *   difftest fuzz TRIALS [SEED [TRIAL]]
 *       Random instruction streams with random registers and memory.
 *       With TRIAL, run only that trial and print each step.
 *
 * Copyright 2026 the dmd5620 authors. MIT License.
 */

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/core.h"

/* ---- The Rust core (rust_core.patch) ---- */
void rs_set_time(uint64_t ns);
void rs_recreate(void);
void rs_set_register(uint8_t reg, uint32_t val);
void rs_get_regs(uint32_t *out);
uint8_t *rs_ram(void);
uint8_t *rs_bbram(void);
uint8_t *rs_vid(void);
int rs_step_checked(char *buf, size_t len);
int rs_dmd_init(uint8_t version);
const uint8_t *rs_dmd_video_ram(void);
int rs_dmd_video_ram_dirty(void);
int rs_dmd_get_duart_output_port(uint8_t *v);
int rs_dmd_mouse_move(uint16_t x, uint16_t y);
int rs_dmd_mouse_down(uint8_t b);
int rs_dmd_mouse_up(uint8_t b);
int rs_dmd_rs232_rx(uint8_t c);
int rs_dmd_keyboard_rx(uint8_t c);
int rs_dmd_rs232_tx(uint8_t *c);
int rs_dmd_keyboard_tx(uint8_t *c);
int rs_dmd_set_nvram(const uint8_t *buf);
int rs_dmd_get_nvram(uint8_t *buf);

/* ---- The C core: test hooks ---- */
void dmd_core_test_recreate(void);
uint8_t *c_vid(void);
int test_opcode_list(uint16_t *out, int max);
int test_operand_types(uint16_t opcode, uint8_t *dtype, uint8_t ot[4]);

static uint64_t vtime;
static uint64_t c_clock(void) { return vtime; }

static jmp_buf c_fatal_jmp;
static char c_fatal_msg[512];
static void c_fatal(const char *msg)
{
    snprintf(c_fatal_msg, sizeof c_fatal_msg, "%s", msg);
    longjmp(c_fatal_jmp, 1);
}

/* Step the C core; 1 if it hit a fatal error */
static int c_step_checked(void)
{
    if (setjmp(c_fatal_jmp))
        return 1;
    dmd_step();
    return 0;
}

/* ---- PRNG (xorshift64*) ---- */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s * 0x9E3779B97F4A7C15ULL + 1; }
static uint32_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

static const char *regname(int i)
{
    static const char *n[16] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8",
                                 "fp", "ap", "psw", "sp", "pcbp", "isp", "pc" };
    return n[i];
}

static int compare_regs(const char *what, uint64_t step)
{
    uint32_t rr[16];
    int bad = 0;
    rs_get_regs(rr);
    for (int i = 0; i < 16; i++) {
        if (rr[i] != cpu_r[i]) {
            if (!bad)
                printf("MISMATCH %s step %" PRIu64 ":\n", what, step);
            printf("  %-4s rust=%08x c=%08x\n", regname(i), rr[i], cpu_r[i]);
            bad = 1;
        }
    }
    return bad;
}

static int compare_mem(const char *what, uint64_t step)
{
    uint8_t cn[DMD_NVRAM_SIZE], rnv[DMD_NVRAM_SIZE];
    const uint8_t *r = rs_ram(), *c = bus_ram();
    if (memcmp(r, c, RAM_SIZE)) {
        for (size_t i = 0; i < RAM_SIZE; i++) {
            if (r[i] != c[i]) {
                printf("MISMATCH %s step %" PRIu64 ": RAM %06zx rust=%02x c=%02x\n", what, step,
                       0x700000 + i, r[i], c[i]);
                break;
            }
        }
        return 1;
    }
    dmd_get_nvram(cn);
    rs_dmd_get_nvram(rnv);
    if (memcmp(cn, rnv, sizeof cn)) {
        printf("MISMATCH %s step %" PRIu64 ": NVRAM\n", what, step);
        return 1;
    }
    if (memcmp(rs_vid(), c_vid(), 2)) {
        printf("MISMATCH %s step %" PRIu64 ": display register\n", what, step);
        return 1;
    }
    return 0;
}

static void dump_instr(uint32_t pc)
{
    printf("  bytes at pc %08x:", pc);
    for (int i = 0; i < 16; i++) {
        uint8_t b;
        if (bus_read_byte((addr_t)pc + i, &b) == E_OK)
            printf(" %02x", b);
        else
            printf(" ??");
    }
    printf("\n");
}

/* ---- boot ---- */

static const char *host_text[] = {
    "Hello from the host. The quick brown fox jumps over the lazy dog 0123456789.\r\n",
    "$ ls -l\r\ntotal 42\r\n-rw-r--r--  1 dmr  bell  1024 Jan  1  1985 README\r\n",
    "\033[H\033[J\033[1;1HTop\033[10;20Hmiddle\033[K\033[2J",
    "tab\there\tand\tthere\b\b\bbs\a\r\n",
    "\033[7minverse\033[m normal \033[A\033[B\033[C\033[D\r\n",
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\r\n",
};

static const uint8_t keys[] = { 'a', 'b', 'z', '0', '9', ' ', '\r', 0xe7, 0xd0, 0xd1, 0xfe, 0x1b,
                                0xc1, 0xc2, 0xc3, 0xc4, 0xe8, 0xe9, 'l' & 0x1f, 'c' & 0x1f, 'Q' };

static int run_boot(int fw, uint64_t steps, uint64_t seed, uint64_t ns_per_step)
{
    char rmsg[512];
    uint16_t mx = 0, my = 0;
    int buttons = 0;
    uint64_t tx_bytes = 0, kb_bytes = 0;

    rng_seed(seed);
    vtime = 0;
    rs_set_time(0);
    dmd_set_clock(c_clock);
    dmd_set_fatal_handler(c_fatal);
    rs_recreate();
    dmd_core_test_recreate();

    if (rs_dmd_init((uint8_t)fw) != dmd_init((uint8_t)fw)) {
        printf("init results differ\n");
        return 1;
    }
    if (compare_regs("init", 0))
        return 1;

    uint64_t next_event = 3000000 + rn(1000000);
    for (uint64_t s = 1; s <= steps; s++) {
        vtime = s * ns_per_step;
        rs_set_time(vtime);

        if (s == next_event) {
            uint32_t k = rn(100);
            if (k < 30) { /* host text */
                const char *t = host_text[rn(sizeof host_text / sizeof host_text[0])];
                for (; *t; t++) {
                    rs_dmd_rs232_rx((uint8_t)*t);
                    dmd_rs232_rx((uint8_t)*t);
                }
            } else if (k < 40) { /* random host bytes: mostly text, some control */
                for (int n = (int)rn(40); n > 0; n--) {
                    uint8_t c = (uint8_t)(rn(10) ? 0x20 + rn(0x5f) : rn(10) ? rn(0x20) : rnd());
                    rs_dmd_rs232_rx(c);
                    dmd_rs232_rx(c);
                }
            } else if (k < 60) { /* keys */
                for (int n = 1 + (int)rn(5); n > 0; n--) {
                    uint8_t c = keys[rn(sizeof keys)];
                    rs_dmd_keyboard_rx(c);
                    dmd_keyboard_rx(c);
                }
            } else if (k < 62) { /* Setup key */
                rs_dmd_keyboard_rx(0xae);
                dmd_keyboard_rx(0xae);
            } else if (k < 85) { /* mouse move */
                mx = (uint16_t)(mx + rn(200) - 100);
                my = (uint16_t)(my + rn(200) - 100);
                rs_dmd_mouse_move(mx, my);
                dmd_mouse_move(mx, my);
            } else { /* mouse button */
                uint8_t b = (uint8_t)rn(3);
                if (buttons & (1 << b)) {
                    rs_dmd_mouse_up(b);
                    dmd_mouse_up(b);
                } else {
                    rs_dmd_mouse_down(b);
                    dmd_mouse_down(b);
                }
                buttons ^= 1 << b;
            }
            if (getenv("DIFF_EVENTS"))
                printf("event step %" PRIu64 " kind %u mouse %d,%d buttons %d\n", s, k, (int16_t)mx, (int16_t)my, buttons);
            next_event = s + 20000 + rn(400000);
        }

        uint32_t pc = cpu_r[R_PC];
        static uint32_t ring[4096], ring_sp[4096];
        ring[s & 4095] = pc;
        ring_sp[s & 4095] = cpu_r[R_SP];
        int rp = rs_step_checked(rmsg, sizeof rmsg);
        int cp = c_step_checked();
        if (rp || cp) {
            printf("step %" PRIu64 ": rust %.200s, c %s\n", s, rp ? rmsg : "ok", cp ? c_fatal_msg : "ok");
            dump_instr(pc);
            if (getenv("DIFF_TRACE")) {
                uint32_t last = 0xffffffff;
                for (uint64_t i = s - 4095; i <= s; i++) {
                    uint32_t q = ring[i & 4095];
                    if (q < 0x20000 && last < 0x20000 && q > last && q - last < 16) { last = q; continue; }
                    printf("  trace %" PRIu64 ": pc %08x sp %08x\n", i, q, ring_sp[i & 4095]);
                    last = q;
                }
            }
            if (rp && cp && !compare_regs("stop", s) && !compare_mem("stop", s)) {
                printf("boot fw %d: both cores stopped the same way at step %" PRIu64 " (seed %" PRIu64 ")\n", fw, s, seed);
                return 0;
            }
            return 1;
        }
        if (compare_regs("boot", s)) {
            dump_instr(pc);
            return 1;
        }

        uint8_t a = 0, b = 0;
        int ra, ca;
        for (;;) {
            ra = rs_dmd_rs232_tx(&a);
            ca = dmd_rs232_tx(&b);
            if (ra == DMD_BUSY && ca == DMD_BUSY)
                break;
            if (ra != ca || a != b) {
                printf("MISMATCH step %" PRIu64 ": rs232 tx\n", s);
                return 1;
            }
            tx_bytes++;
        }
        for (;;) {
            ra = rs_dmd_keyboard_tx(&a);
            ca = dmd_keyboard_tx(&b);
            if (ra == DMD_BUSY && ca == DMD_BUSY)
                break;
            if (ra != ca || a != b) {
                printf("MISMATCH step %" PRIu64 ": keyboard tx\n", s);
                return 1;
            }
            kb_bytes++;
        }
        rs_dmd_get_duart_output_port(&a);
        dmd_get_duart_output_port(&b);
        if (a != b) {
            printf("MISMATCH step %" PRIu64 ": output port\n", s);
            return 1;
        }

        if ((s & 0xffff) == 0) {
            if (rs_dmd_video_ram_dirty() != dmd_video_ram_dirty()) {
                printf("MISMATCH step %" PRIu64 ": dirty flag\n", s);
                return 1;
            }
            const uint8_t *rv = rs_dmd_video_ram();
            const uint8_t *cv = dmd_video_ram();
            if (memcmp(rv, cv, DMD_VIDEO_BYTES)) {
                printf("MISMATCH step %" PRIu64 ": frame\n", s);
                return 1;
            }
            if (compare_mem("boot", s))
                return 1;
        }
        if (s % 10000000 == 0) {
            printf("  %" PRIu64 "M steps ok (pc %08x, host tx %" PRIu64 ", kbd tx %" PRIu64 ")\n",
                   s / 1000000, cpu_r[R_PC], tx_bytes, kb_bytes);
            fflush(stdout);
        }
    }
    if (compare_mem("boot end", steps))
        return 1;
    printf("boot fw %d: %" PRIu64 " steps identical (seed %" PRIu64 ", %" PRIu64
           " ns/step, host tx %" PRIu64 " bytes, kbd tx %" PRIu64 ")\n",
           fw, steps, seed, ns_per_step, tx_bytes, kb_bytes);
    return 0;
}

/* ---- fuzz ---- */

#define WIN_START 0x700000u
#define WIN_LEN   0x20000u

static uint32_t rand_ptr(void)
{
    uint32_t k = rn(100);
    if (k < 90)
        return WIN_START + (rn(WIN_LEN) & ~3u);
    if (k < 93)
        return WIN_START + rn(WIN_LEN);
    if (k < 95)
        return 0x600000 + (rn(0x2000) & ~3u); /* NVRAM */
    if (k < 97)
        return 0x200000 + rn(0x40); /* DUART */
    if (k < 98)
        return 0x400000 + (rn(2) << 1); /* mouse */
    if (k < 99)
        return rn(0x20000) & ~3u; /* ROM */
    return rnd();
}

static uint32_t rand_val(void)
{
    uint32_t k = rn(10);
    if (k < 4)
        return rand_ptr();
    if (k < 7)
        return rn(64) - 32;
    return rnd();
}

/* Valid opcodes (byte table entries, plus the 0x30xx ones) */
static uint16_t opcodes[300];
static int n_opcodes;

/* Decoded but not executed by the Rust core (or HALT/BPT): rare */
static int unimplemented(uint16_t op)
{
    switch (op) {
    case 0x00: case 0x02: case 0x03: case 0x06: case 0x07: case 0x13: case 0x14:
    case 0x17: case 0x22: case 0x23: case 0x2E: case 0x2F: case 0x32: case 0x33:
    case 0x54: case 0x58: case 0x302F: case 0x303F: case 0x3061:
        return 1;
    }
    return 0;
}

/* Opcodes whose first operand is an address (effective_address) */
static int address_op(uint16_t op)
{
    return op == 0x04 || op == 0x0C || op == 0x24 || op == 0x2C || op == 0x34 || op == 0xE0;
}

static void put8(uint8_t **p, uint32_t v) { *(*p)++ = (uint8_t)v; }
static void put16(uint8_t **p, uint32_t v) { put8(p, v); put8(p, v >> 8); }
static void put32(uint8_t **p, uint32_t v) { put16(p, v); put16(p, v >> 16); }

static int rand_reg(void)
{
    uint32_t k = rn(100);
    if (k < 70)
        return (int)rn(9);
    if (k < 80)
        return R_FP;
    if (k < 90)
        return R_AP;
    if (k < 95)
        return R_SP;
    return (int)rn(16);
}

static uint32_t short_off(void)
{
    return rn(5) ? rn(4) * 4 : rn(15);
}

/* kind: 0 = source, 1 = destination, 2 = address */
static void gen_descriptor(uint8_t **p, int depth, int kind)
{
    uint32_t k;
    int r = rand_reg();
    if (rn(50) == 0) {
        put8(p, rnd()); /* anything */
        return;
    }
    for (;;) {
        k = rn(100);
        if (kind >= 1 && (k < 8 || (k >= 25 && k < 28) || (k >= 38 && k < 40) || (k >= 45 && k < 47) ||
                          k >= 91) && rn(20))
            continue; /* literals and immediates: rarely as destinations */
        if (kind == 2 && k >= 8 && k < 25 && rn(20))
            continue; /* registers: rarely as addresses */
        break;
    }
    if (r == 15 && rn(4))
        r = 0;
    if (k < 8)
        put8(p, rn(0x40)); /* positive literal */
    else if (k < 25)
        put8(p, 0x40 | (r == 15 ? 0 : r)); /* register */
    else if (k < 28) {
        put8(p, 0x4f); /* word immediate */
        put32(p, rand_val());
    } else if (k < 38)
        put8(p, 0x50 | (r == 11 || r == 15 ? 1 : r)); /* register deferred */
    else if (k < 40) {
        put8(p, 0x5f);
        put16(p, rnd());
    } else if (k < 45)
        put8(p, 0x60 | short_off()); /* FP short offset */
    else if (k < 47) {
        put8(p, 0x6f);
        put8(p, rnd());
    } else if (k < 52)
        put8(p, 0x70 | short_off()); /* AP short offset */
    else if (k < 56) {
        put8(p, 0x7f); /* absolute */
        put32(p, rand_ptr());
    } else if (k < 60) {
        put8(p, 0x80 | (r == 11 ? 0 : r));
        put32(p, rn(64) * 4 - 128);
    } else if (k < 63) {
        put8(p, 0x90 | (r == 11 ? 0 : r));
        put32(p, rn(64) * 4 - 128);
    } else if (k < 67) {
        put8(p, 0xa0 | (r == 11 ? 0 : r));
        put16(p, rn(64) * 4 - 128);
    } else if (k < 70) {
        put8(p, 0xb0 | (r == 11 ? 0 : r));
        put16(p, rn(64) * 4 - 128);
    } else if (k < 76) {
        put8(p, 0xc0 | (r == 11 ? 0 : r));
        put8(p, rn(64) * 4 - 128);
    } else if (k < 80) {
        put8(p, 0xd0 | (r == 11 ? 0 : r));
        put8(p, rn(64) * 4 - 128);
    } else if (k < 88 && depth < 2) {
        static const uint8_t ex[] = { 0xe0, 0xe2, 0xe3, 0xe4, 0xe6, 0xe7 };
        put8(p, ex[rn(6)]); /* expanded type */
        gen_descriptor(p, depth + 1, kind);
    } else if (k < 91 || k < 88) {
        put8(p, 0xef); /* absolute deferred */
        put32(p, rand_ptr());
    } else
        put8(p, 0xf0 | rn(16)); /* negative literal */
}

static void gen_instr(uint8_t **p)
{
    uint16_t op;
    if (rn(500) == 0) {
        put8(p, rnd());
        return;
    }
    do
        op = opcodes[rn((uint32_t)n_opcodes)];
    while (unimplemented(op) && rn(30));
    if (op >= 0x100) {
        put16(p, ((op & 0xff) << 8) | (op >> 8)); /* 0x30 first, then the low byte */
        return;
    }
    put8(p, op);
    uint8_t dtype, ot[4];
    test_operand_types(op, &dtype, ot);
    for (int i = 0; i < 4; i++) {
        if (ot[i] == 1) { /* literal: short branches mostly */
            uint32_t v = rn(4) ? rn(32) - 16 : rnd();
            if (dtype == 1)
                put8(p, v);
            else if (dtype == 2)
                put16(p, v);
            else
                put32(p, v);
        } else if (ot[i] >= 2) {
            gen_descriptor(p, 0, (i == 0 && address_op(op)) ? 2 : ot[i] == 3 ? 1 : 0);
        }
    }
}

static int run_fuzz(uint64_t trials, uint64_t seed, int64_t only)
{
    char rmsg[512];
    uint64_t steps_total = 0, both_panic = 0, known = 0, bad = 0;
    static uint8_t mem_init[WIN_LEN];

    n_opcodes = test_opcode_list(opcodes, 300);
    dmd_set_clock(c_clock);
    dmd_set_fatal_handler(c_fatal);

    for (uint64_t t = 0; t < trials; t++) {
        if (only >= 0 && (uint64_t)only != t)
            continue;
        rng_seed(seed * 1000003 + t);

        vtime = rn(4) == 0 ? 20000000 : 0; /* sometimes past the first vblank */
        rs_set_time(vtime);
        rs_recreate();
        dmd_core_test_recreate();
        if (rn(4)) {
            uint8_t fw = (uint8_t)(1 + rn(2));
            rs_dmd_init(fw);
            dmd_init(fw);
        }

        /* Memory: random bytes and pointers, then the program */
        for (uint32_t i = 0; i < WIN_LEN; i += 4) {
            uint32_t v = rn(5) ? rand_ptr() : rnd();
            mem_init[i] = (uint8_t)(v >> 24);
            mem_init[i + 1] = (uint8_t)(v >> 16);
            mem_init[i + 2] = (uint8_t)(v >> 8);
            mem_init[i + 3] = (uint8_t)v;
        }
        uint32_t pc = WIN_START + 0x1000 + rn(WIN_LEN - 0x4000);
        uint8_t *p = mem_init + (pc - WIN_START);
        for (int n = 0; n < 300 && p < mem_init + WIN_LEN - 64; n++)
            gen_instr(&p);
        memcpy(rs_ram(), mem_init, WIN_LEN);
        memcpy(bus_ram(), mem_init, WIN_LEN);

        /* Registers */
        for (int i = 0; i < 16; i++) {
            uint32_t v;
            if (i <= 8)
                v = rand_val();
            else if (i == R_PSW)
                v = (rnd() & ~0x1800u) | (rn(8) == 0 ? (rn(4) << 11) : 0);
            else if (i == R_PC)
                v = pc;
            else
                v = WIN_START + 0x100 + (rn(WIN_LEN - 0x200) & ~3u);
            rs_set_register((uint8_t)i, v);
            cpu_r[i] = v;
        }

        for (int s = 0; s < 300; s++) {
            uint32_t spc = cpu_r[R_PC];
            if (only >= 0) {
                printf("trial %" PRIu64 " step %d:", t, s);
                dump_instr(spc);
            }
            int rp = rs_step_checked(rmsg, sizeof rmsg);
            int cp = c_step_checked();
            steps_total++;
            if (rp && cp) {
                both_panic++;
                if (getenv("FUZZ_WHY")) {
                    char *q = strstr(rmsg, "Unexpected CPU Error '");
                    printf("WHY %.60s | %.40s\n", q ? q + 22 : rmsg, c_fatal_msg);
                }
                if (only >= 0)
                    printf("  both stop: rust '%s' / c '%s'\n", rmsg, c_fatal_msg);
                break;
            }
            if (rp || cp) {
                if (rp && (strstr(rmsg, "divide with overflow") ||
                           strstr(rmsg, "remainder with overflow"))) {
                    known++;
                    break;
                }
                printf("MISMATCH trial %" PRIu64 " step %d: rust %s, c %s\n", t, s,
                       rp ? rmsg : "ok", cp ? c_fatal_msg : "ok");
                dump_instr(spc);
                bad++;
                break;
            }
            char what[64];
            snprintf(what, sizeof what, "trial %" PRIu64, t);
            if (compare_regs(what, (uint64_t)s)) {
                dump_instr(spc);
                bad++;
                break;
            }
            if (only >= 0 && compare_mem(what, (uint64_t)s)) {
                bad++;
                break;
            }
        }
        if (!bad || only >= 0) {
            char what[64];
            snprintf(what, sizeof what, "trial %" PRIu64 " end", t);
            if (compare_mem(what, 0))
                bad++;
        }
        if (bad >= 10)
            break;
    }
    printf("fuzz: %" PRIu64 " trials, %" PRIu64 " steps, %" PRIu64 " ended in a fatal error in both, %" PRIu64
           " known differences (Rust panics on MIN/-1), %" PRIu64 " mismatches\n",
           only >= 0 ? 1 : trials, steps_total, both_panic, known, bad);
    return bad != 0;
}

int main(int argc, char **argv)
{
    dmd_set_rust_compat(1); /* compare without the fixes of the C port */
    if (argc >= 4 && !strcmp(argv[1], "boot"))
        return run_boot(atoi(argv[2]), strtoull(argv[3], NULL, 0), argc > 4 ? strtoull(argv[4], NULL, 0) : 1,
                        argc > 5 ? strtoull(argv[5], NULL, 0) : 72);
    if (argc >= 3 && !strcmp(argv[1], "fuzz"))
        return run_fuzz(strtoull(argv[2], NULL, 0), argc > 3 ? strtoull(argv[3], NULL, 0) : 1,
                        argc > 4 ? strtoll(argv[4], NULL, 0) : -1);
    fprintf(stderr, "usage: difftest boot FW STEPS [SEED [NS_PER_STEP]]\n"
                    "       difftest fuzz TRIALS [SEED [TRIAL]]\n");
    return 2;
}
