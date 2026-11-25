/*
 * The DMD 5620 system bus: ROM, DUART, mouse, display start register,
 * NVRAM and RAM. Port of bus.rs, mem.rs and mouse.rs.
 *
 * Memory map:
 *   0x000000..0x01ffff  ROM (128 KB)
 *   0x200000..0x20003f  DUART (port A: host, port B: keyboard)
 *   0x400000..0x400003  mouse Y (+0), X (+2)
 *   0x500000..0x500001  display start (in 4-byte units from RAM start)
 *   0x600000..0x601fff  NVRAM (BBRAM)
 *   0x700000..0x7fffff  RAM (1 MB)
 *
 * Data is big-endian. Instruction operands are fetched little-endian
 * (bus_read_op_half/word).
 *
 * Copyright 2018-2025 Seth J. Morabito <web@loomcom.com> (Rust original)
 * Copyright 2026 the dmd5620 authors (C port)
 * MIT License, see LICENSE.md.
 */

#include <string.h>

#include "core.h"

enum { DEV_ROM, DEV_DUART, DEV_MOUSE, DEV_VID, DEV_BBRAM, DEV_RAM };

struct mem {
    addr_t start;
    size_t len;
    uint8_t *ram;
    bool read_only;
};

#define rom_buf bus_rom_buf
#define vid_buf bus_vid_buf
#define ram_buf bus_ram_buf
#define video_ram_dirty bus_video_dirty
uint8_t bus_rom_buf[ROM_SIZE];
uint8_t bus_vid_buf[2];
uint8_t bus_ram_buf[RAM_SIZE];
static uint8_t bbram_buf[BBRAM_SIZE];

static struct mem mems[] = {
    [DEV_ROM] = { 0x000000, ROM_SIZE, rom_buf, true },
    [DEV_VID] = { 0x500000, 2, vid_buf, false },
    [DEV_BBRAM] = { 0x600000, BBRAM_SIZE, bbram_buf, false },
    [DEV_RAM] = { 0x700000, RAM_SIZE, ram_buf, false },
};

static uint16_t mouse_x, mouse_y;
bool bus_video_dirty;

void bus_new(void)
{
    memset(rom_buf, 0, sizeof rom_buf);
    memset(vid_buf, 0, sizeof vid_buf);
    memset(bbram_buf, 0, sizeof bbram_buf);
    memset(ram_buf, 0, sizeof ram_buf);
    mouse_x = mouse_y = 0;
    video_ram_dirty = false;
}

uint8_t *bus_ram(void)
{
    return ram_buf;
}

static int get_device(addr_t a)
{
    if (a < 0x20000)
        return DEV_ROM;
    if (a >= 0x200000 && a < 0x200040)
        return DEV_DUART;
    if (a >= 0x400000 && a < 0x400004)
        return DEV_MOUSE;
    if (a >= 0x500000 && a < 0x500002)
        return DEV_VID;
    if (a >= 0x600000 && a < 0x602000)
        return DEV_BBRAM;
    if (a >= 0x700000 && a < 0x800000)
        return DEV_RAM;
    return -1;
}

/* ---- Mem: one plain memory range ---- */

static size_t mem_offset(const struct mem *m, addr_t address, size_t n)
{
    addr_t off = address - m->start;
    if (off + n > m->len)
        core_fatal("memory index out of bounds: address %08llx", (unsigned long long)address);
    return (size_t)off;
}

static int mem_read(const struct mem *m, addr_t address, int n, uint32_t *val)
{
    if (address >= m->start + m->len)
        return E_BUS_RANGE;
    size_t o = mem_offset(m, address, (size_t)n);
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | m->ram[o + i];
    *val = v;
    return E_OK;
}

static int mem_write(struct mem *m, addr_t address, int n, uint32_t val)
{
    if (m->read_only)
        return E_BUS_WRITE;
    if (address >= m->start + m->len)
        return E_BUS_RANGE;
    size_t o = mem_offset(m, address, (size_t)n);
    for (int i = n - 1; i >= 0; i--) {
        m->ram[o + i] = (uint8_t)val;
        val >>= 8;
    }
    return E_OK;
}

/* ---- Mouse ---- */

static int mouse_read_half(addr_t address, uint16_t *val)
{
    switch (address - 0x400000) {
    case 0:
        *val = mouse_y;
        return E_OK;
    case 2:
        *val = mouse_x;
        return E_OK;
    default:
        return E_BUS_NODEV;
    }
}

/* ---- Device access ---- */

static int dev_read_byte(int dev, addr_t a, uint8_t *val)
{
    uint32_t v;
    switch (dev) {
    case DEV_DUART:
        return duart_read_byte(a, val);
    case DEV_MOUSE:
        core_fatal("not implemented: mouse byte read at %08llx", (unsigned long long)a);
    default:
        TRY(mem_read(&mems[dev], a, 1, &v));
        *val = (uint8_t)v;
        return E_OK;
    }
}

static int dev_read_half(int dev, addr_t a, uint16_t *val)
{
    uint32_t v;
    uint8_t b;
    switch (dev) {
    case DEV_DUART:
        TRY(duart_read_byte(a + 2, &b));
        *val = b;
        return E_OK;
    case DEV_MOUSE:
        return mouse_read_half(a, val);
    default:
        TRY(mem_read(&mems[dev], a, 2, &v));
        *val = (uint16_t)v;
        return E_OK;
    }
}

static int dev_read_word(int dev, addr_t a, uint32_t *val)
{
    uint8_t b;
    switch (dev) {
    case DEV_DUART:
        TRY(duart_read_byte(a + 3, &b));
        *val = b;
        return E_OK;
    case DEV_MOUSE:
        core_fatal("not implemented: mouse word read at %08llx", (unsigned long long)a);
    default:
        return mem_read(&mems[dev], a, 4, val);
    }
}

static int dev_write(int dev, addr_t a, int n, uint32_t val)
{
    switch (dev) {
    case DEV_DUART: /* half: byte at +2, word: byte at +3 */
        return duart_write_byte(a + (n == 1 ? 0 : n == 2 ? 2 : 3), (uint8_t)val);
    case DEV_MOUSE:
        core_fatal("not implemented: mouse write at %08llx", (unsigned long long)a);
    default:
        return mem_write(&mems[dev], a, n, val);
    }
}

/* ---- Bus ---- */

static void video_range(addr_t *start, addr_t *end)
{
    addr_t reg = ((addr_t)vid_buf[0] << 8) | vid_buf[1];
    *start = reg * 4;
    *end = *start + DMD_VIDEO_BYTES;
}

static bool is_video_ram(addr_t a)
{
    if (a < 0x700000 || a >= 0x800000)
        return false;
    addr_t s, e;
    video_range(&s, &e);
    return a - 0x700000 >= s && a - 0x700000 < e;
}

int bus_read_byte_slow(addr_t a, uint8_t *val)
{
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    return dev_read_byte(dev, a, val);
}

int bus_read_half_slow(addr_t a, uint16_t *val)
{
    if (a & 1)
        return E_BUS_ALIGNMENT;
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    return dev_read_half(dev, a, val);
}

int bus_read_word_slow(addr_t a, uint32_t *val)
{
    if (a & 3)
        return E_BUS_ALIGNMENT;
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    return dev_read_word(dev, a, val);
}

int bus_read_op_half_slow(addr_t a, uint16_t *val)
{
    uint8_t b0, b1;
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    TRY(dev_read_byte(dev, a, &b0));
    TRY(dev_read_byte(dev, a + 1, &b1));
    *val = (uint16_t)(b0 | (b1 << 8));
    return E_OK;
}

int bus_read_op_word_slow(addr_t a, uint32_t *val)
{
    uint8_t b[4];
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    for (int i = 0; i < 4; i++)
        TRY(dev_read_byte(dev, a + (addr_t)i, &b[i]));
    *val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return E_OK;
}

int bus_write_byte_slow(addr_t a, uint8_t val)
{
    if (is_video_ram(a))
        video_ram_dirty = true;
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    return dev_write(dev, a, 1, val);
}

int bus_write_half_slow(addr_t a, uint16_t val)
{
    if (a & 1)
        return E_BUS_ALIGNMENT;
    if (is_video_ram(a))
        video_ram_dirty = true;
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    return dev_write(dev, a, 2, val);
}

int bus_write_word_slow(addr_t a, uint32_t val)
{
    if (a & 3)
        return E_BUS_ALIGNMENT;
    if (is_video_ram(a))
        video_ram_dirty = true;
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    return dev_write(dev, a, 4, val);
}

/* Load data into memory, ROM included. */
int bus_load(addr_t a, const uint8_t *data, size_t len)
{
    int dev = get_device(a);
    if (dev < 0)
        return E_BUS_NODEV;
    if (dev == DEV_DUART || dev == DEV_MOUSE)
        core_fatal("not implemented: load into a device");
    struct mem *m = &mems[dev];
    if (len > m->len)
        return E_BUS_RANGE;
    size_t o = mem_offset(m, a, len);
    memcpy(m->ram + o, data, len);
    return E_OK;
}

uint8_t *bus_video_ram(void)
{
    addr_t s, e;
    video_ram_dirty = false;
    video_range(&s, &e);
    if (e > RAM_SIZE)
        core_fatal("video RAM out of range: %08llx", (unsigned long long)s);
    return ram_buf + s;
}

bool bus_video_ram_dirty(void)
{
    return video_ram_dirty;
}

void bus_mouse_move(uint16_t x, uint16_t y)
{
    mouse_x = x;
    mouse_y = y;
}

void bus_get_nvram(uint8_t *out)
{
    memcpy(out, bbram_buf, BBRAM_SIZE);
}

void bus_set_nvram(const uint8_t *in)
{
    memcpy(bbram_buf, in, BBRAM_SIZE);
}

uint8_t *c_vid(void) /* tests */
{
    return vid_buf;
}
