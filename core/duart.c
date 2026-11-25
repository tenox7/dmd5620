/*
 * The 2681 DUART. Port A is the host RS-232 line, port B the keyboard
 * (and the write-only printer port). Port of duart.rs and utils.rs.
 *
 * Each port has the mode, status and configuration registers, a 3-slot
 * receive FIFO, a receive shift register, and transmit holding and shift
 * registers. On top of that, unbounded RX and TX queues (not in the
 * hardware) buffer data for the user of the library. Characters move at
 * the programmed baud rate, on the clock from core_now(). The DUART also
 * makes the 60 Hz vertical blank interrupt and takes the mouse buttons.
 *
 * Copyright 2018-2025 Seth J. Morabito <web@loomcom.com> (Rust original)
 * Copyright 2026 the dmd5620 authors (C port)
 * MIT License, see LICENSE.md.
 */

#include <stdlib.h>
#include <string.h>

#include "core.h"

#define START_ADDR 0x200000

#define VERTICAL_BLANK_DELAY 16666666ULL /* ns, 60 Hz */

static const uint32_t BAUD_RATES_A[13] = { 50, 110, 135, 200, 300, 600, 1200,
                                           1050, 2400, 4800, 7200, 9600, 38400 };
static const uint32_t BAUD_RATES_B[13] = { 75, 110, 135, 150, 300, 600, 1200,
                                           2000, 2400, 4800, 1800, 9600, 19200 };

/* Registers (offset from START_ADDR) */
#define MR12A        0x03
#define CSRA         0x07
#define CRA          0x0b
#define THRA         0x0f
#define RHRA         0x0f
#define IPCR_ACR     0x13
#define ISR_MASK     0x17
#define MR12B        0x23
#define CSRB         0x27
#define CRB          0x2b
#define THRB         0x2f
#define RHRB         0x2f
#define IP_OPCR      0x37
#define OPBITS_SET   0x3b
#define OPBITS_RESET 0x3f

/* Port configuration bits */
#define CNF_ETX 0x01
#define CNF_ERX 0x02

/* Status flags */
#define STS_RXR 0x01 /* Rx ready */
#define STS_FFL 0x02 /* FIFO full */
#define STS_TXR 0x04 /* Tx ready */
#define STS_TXE 0x08 /* Tx register empty */
#define STS_OER 0x10 /* Overflow error */
#define STS_PER 0x20 /* Parity error */
#define STS_FER 0x40 /* Framing error */
#define STS_RXB 0x80 /* Received break */

/* Command register */
#define CMD_ERX 0x01
#define CMD_DRX 0x02
#define CMD_ETX 0x04
#define CMD_DTX 0x08

/* Control register commands */
#define CR_RST_MR    0x01
#define CR_RST_RX    0x02
#define CR_RST_TX    0x03
#define CR_RST_ERR   0x04
#define CR_RST_BRK   0x05
#define CR_START_BRK 0x06
#define CR_STOP_BRK  0x07

/* Interrupt status register */
#define ISTS_TAI 0x01
#define ISTS_RAI 0x02
#define ISTS_DBA 0x04
#define ISTS_TBI 0x10
#define ISTS_RBI 0x20
#define ISTS_DBB 0x40
#define ISTS_IPC 0x80

/* CPU interrupt vector bits */
#define KEYBOARD_INT    0x04
#define MOUSE_BLANK_INT 0x02
#define TX_INT          0x10
#define RX_INT          0x20

#define FIFO_LEN 3

struct fifo {
    uint8_t buf[FIFO_LEN];
    int rd, wr, len;
};

struct port {
    uint8_t mode[2];
    int mode_ptr;
    uint8_t stat;
    uint8_t conf;
    struct fifo rx_fifo;
    bool rx_shift_full;
    uint8_t rx_shift_reg;
    bool tx_holding_full;
    uint8_t tx_holding_reg;
    bool tx_shift_full;
    uint8_t tx_shift_reg;
    struct byteq rx_q; /* from the user, into the terminal */
    struct byteq tx_q; /* from the terminal, to the user */
    uint64_t char_delay;
    uint64_t next_tx_service;
    uint64_t next_rx_service;
};

static struct {
    struct port ports[2];
    uint8_t acr;
    uint8_t ipcr;
    uint8_t inprt;
    uint8_t outprt;
    uint8_t isr;
    uint8_t imr;
    uint8_t ivec;
    uint64_t next_vblank;
} d;

/* ---- Byte queue ---- */

void byteq_push(struct byteq *q, uint8_t c)
{
    if (q->len == q->cap) {
        size_t ncap = q->cap ? q->cap * 2 : 256;
        uint8_t *nb = malloc(ncap);
        if (!nb)
            core_fatal("out of memory");
        for (size_t i = 0; i < q->len; i++)
            nb[i] = q->buf[(q->head + i) % q->cap];
        free(q->buf);
        q->buf = nb;
        q->cap = ncap;
        q->head = 0;
    }
    q->buf[(q->head + q->len) % q->cap] = c;
    q->len++;
}

bool byteq_pop(struct byteq *q, uint8_t *c)
{
    if (q->len == 0)
        return false;
    *c = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return true;
}

void byteq_free(struct byteq *q)
{
    free(q->buf);
    memset(q, 0, sizeof *q);
}

/* ---- 3-slot FIFO ---- */

static bool fifo_push(struct fifo *f, uint8_t c)
{
    if (f->len == FIFO_LEN)
        return false;
    f->buf[f->wr] = c;
    f->wr = (f->wr + 1) % FIFO_LEN;
    f->len++;
    return true;
}

static bool fifo_pop(struct fifo *f, uint8_t *c)
{
    if (f->len == 0)
        return false;
    *c = f->buf[f->rd];
    f->rd = (f->rd + 1) % FIFO_LEN;
    f->len--;
    return true;
}

static void fifo_clear(struct fifo *f)
{
    f->rd = f->wr = f->len = 0;
}

/* ---- Port ---- */

static void port_init(struct port *p, uint64_t now)
{
    memset(p, 0, sizeof *p);
    p->char_delay = 1000000; /* 1 ms */
    p->next_tx_service = now;
    p->next_rx_service = now;
}

static bool port_loopback(const struct port *p)
{
    return (p->mode[1] & 0xc0) == 0x80;
}

static bool port_rx_enabled(const struct port *p)
{
    return (p->conf & CNF_ERX) != 0;
}

/* Read one character out of the RX FIFO. A character waiting in the
 * shift register moves into the FIFO. */
static bool port_rx_read_char(struct port *p, uint8_t *out)
{
    uint8_t val;
    if (!port_rx_enabled(p))
        return false;
    if (!fifo_pop(&p->rx_fifo, &val))
        return false;
    p->stat &= (uint8_t)~STS_FFL;
    /* A character waiting in the shift register moves into the FIFO. The
     * Rust core does not empty the shift register, so the same character
     * came again on every read, without end. */
    if (p->rx_shift_full) {
        fifo_push(&p->rx_fifo, p->rx_shift_reg);
        if (!core_rust_compat)
            p->rx_shift_full = false;
        p->stat |= STS_RXR;
        if (p->rx_fifo.len == FIFO_LEN)
            p->stat |= STS_FFL;
    }
    if (p->rx_fifo.len == 0)
        p->stat &= (uint8_t)~STS_RXR;
    p->stat &= (uint8_t)~(STS_PER | STS_RXB);
    *out = val;
    return true;
}

/* Receive one character: into the FIFO, or the shift register if the
 * FIFO is full (overwriting it sets the overflow flag). */
static void port_rx_char(struct port *p, uint8_t c)
{
    if (p->rx_fifo.len != FIFO_LEN) {
        fifo_push(&p->rx_fifo, c);
        if (p->rx_fifo.len == FIFO_LEN)
            p->stat |= STS_FFL;
    } else {
        if (p->rx_shift_full)
            p->stat |= STS_OER;
        p->rx_shift_full = true;
        p->rx_shift_reg = c;
    }
    p->stat |= STS_RXR;
}

static void port_rx_service(struct port *p, uint64_t now)
{
    uint8_t c;
    if (!(port_rx_enabled(p) && p->rx_q.len != 0 && now >= p->next_rx_service))
        return;
    if (!port_loopback(p)) {
        /* FIFO and shift register full: a real 2681 would overrun and lose
         * a character. Keep it in the queue instead, so pasted text and
         * fast typing are not lost while the firmware is busy. */
        if (!core_rust_compat && p->rx_fifo.len == FIFO_LEN && p->rx_shift_full)
            return;
        if (byteq_pop(&p->rx_q, &c))
            port_rx_char(p, c);
    }
    p->next_rx_service = now + p->char_delay;
}

static void port_tx_service(struct port *p, bool keyboard, uint64_t now)
{
    if (!p->tx_holding_full && !p->tx_shift_full)
        return;
    if (now < p->next_tx_service)
        return;

    if (p->tx_shift_full) {
        uint8_t c = p->tx_shift_reg;
        if (port_loopback(p)) {
            port_rx_char(p, c);
        } else {
            if (keyboard && c == 0x02)
                p->stat |= STS_PER;
            byteq_push(&p->tx_q, c);
        }
        p->tx_shift_full = false;
        if (!p->tx_holding_full)
            p->stat |= STS_TXR | STS_TXE;
    }

    if (p->tx_holding_full) {
        p->tx_shift_reg = p->tx_holding_reg;
        p->tx_shift_full = true;
        p->tx_holding_full = false;
        p->next_tx_service = now + p->char_delay;
    }
}

static void port_enable_tx(struct port *p)
{
    p->conf |= CNF_ETX;
    if (!p->tx_holding_full)
        p->stat |= STS_TXR;
    if (!p->tx_shift_full)
        p->stat |= STS_TXE;
}

static void port_disable_tx(struct port *p)
{
    p->conf &= (uint8_t)~CNF_ETX;
    p->stat &= (uint8_t)~(STS_TXR | STS_TXE);
}

static void port_enable_rx(struct port *p)
{
    p->conf |= CNF_ERX;
    p->stat &= (uint8_t)~STS_RXR;
}

static void port_disable_rx(struct port *p)
{
    p->conf &= (uint8_t)~CNF_ERX;
    p->stat &= (uint8_t)~STS_RXR;
}

/* ---- DUART ---- */

static uint64_t delay_rate(uint8_t csr_bits, uint8_t acr_bits)
{
    unsigned baud_bits = (csr_bits >> 4) & 0xf;
    if (baud_bits >= 13)
        core_fatal("DUART: unsupported baud rate code %u", baud_bits);
    uint32_t baud = (acr_bits & 0x80) == 0 ? BAUD_RATES_A[baud_bits] : BAUD_RATES_B[baud_bits];
    return 1000000000u / (baud / 8u);
}

void duart_free(void)
{
    for (int i = 0; i < 2; i++) {
        byteq_free(&d.ports[i].rx_q);
        byteq_free(&d.ports[i].tx_q);
    }
}

void duart_new(void)
{
    uint64_t now = core_time = core_now();
    duart_free();
    memset(&d, 0, sizeof d);
    port_init(&d.ports[0], now);
    port_init(&d.ports[1], now);
    d.ipcr = 0x40;
    d.inprt = 0xb;
    d.next_vblank = now + VERTICAL_BLANK_DELAY;
}

static void vertical_blank(void)
{
    d.ivec |= MOUSE_BLANK_INT;
    d.ipcr |= 0x40;
    d.isr |= ISTS_IPC;
    if ((d.inprt & 0x04) == 0)
        d.ipcr |= 0x40;
    else
        d.inprt &= (uint8_t)~0x04;
}

uint8_t duart_get_interrupt(void)
{
    uint64_t now = core_time;
    if (now > d.next_vblank) {
        d.next_vblank = now + VERTICAL_BLANK_DELAY;
        vertical_blank();
    }
    if (d.ports[0].stat & STS_RXR) {
        d.ivec |= RX_INT;
        d.isr |= ISTS_RAI;
    }
    if (d.ports[1].stat & STS_RXR) {
        d.ivec |= KEYBOARD_INT;
        d.isr |= ISTS_RBI;
    }
    if (d.ports[0].stat & STS_TXR) {
        d.ivec |= TX_INT;
        d.isr |= ISTS_TAI;
    }
    return d.ivec;
}

void duart_service(void)
{
    uint64_t now = core_time;
    port_tx_service(&d.ports[0], false, now);
    port_rx_service(&d.ports[0], now);
    port_tx_service(&d.ports[1], true, now);
    port_rx_service(&d.ports[1], now);
}

uint8_t duart_output_port(void)
{
    /* The output port always returns the complement of the bits */
    return (uint8_t)~d.outprt;
}

void duart_rs232_rx(uint8_t c)
{
    byteq_push(&d.ports[0].rx_q, c);
}

void duart_keyboard_rx(uint8_t c)
{
    byteq_push(&d.ports[1].rx_q, c);
}

bool duart_rs232_tx(uint8_t *c)
{
    return byteq_pop(&d.ports[0].tx_q, c);
}

bool duart_keyboard_tx(uint8_t *c)
{
    return byteq_pop(&d.ports[1].tx_q, c);
}

void duart_mouse_down(uint8_t button)
{
    d.ipcr = 0;
    d.inprt |= 0xb;
    d.isr |= ISTS_IPC;
    d.ivec |= MOUSE_BLANK_INT;
    switch (button) {
    case 0:
        d.ipcr |= 0x80;
        d.inprt &= (uint8_t)~0x08;
        break;
    case 1:
        d.ipcr |= 0x20;
        d.inprt &= (uint8_t)~0x02;
        break;
    case 2:
        d.ipcr |= 0x10;
        d.inprt &= (uint8_t)~0x01;
        break;
    }
}

void duart_mouse_up(uint8_t button)
{
    d.ipcr = 0;
    d.inprt |= 0xb;
    d.isr |= ISTS_IPC;
    d.ivec |= MOUSE_BLANK_INT;
    switch (button) {
    case 0:
        d.ipcr |= 0x80;
        break;
    case 1:
        d.ipcr |= 0x20;
        break;
    case 2:
        d.ipcr |= 0x10;
        break;
    }
}

static void handle_command(uint8_t cmd, int port_no)
{
    struct port *p = &d.ports[port_no];
    uint8_t tx_ists = port_no == 0 ? ISTS_TAI : ISTS_TBI;
    uint8_t rx_ists = port_no == 0 ? ISTS_RAI : ISTS_RBI;
    uint8_t dbk_ists = port_no == 0 ? ISTS_DBA : ISTS_DBB;

    /* Enable or disable the transmitter. Disable wins if both are set. */
    if (cmd & CMD_DTX) {
        port_disable_tx(p);
        d.ivec &= (uint8_t)~TX_INT;
        d.isr &= (uint8_t)~tx_ists;
    } else if (cmd & CMD_ETX) {
        port_enable_tx(p);
        d.ivec |= TX_INT;
        d.isr |= tx_ists;
    }

    /* Enable or disable the receiver. Disable wins if both are set. */
    if (cmd & CMD_DRX) {
        port_disable_rx(p);
        d.isr &= (uint8_t)~rx_ists;
        if (port_no == 0) {
            d.ivec &= (uint8_t)~RX_INT;
            d.isr &= (uint8_t)~ISTS_RAI;
        } else {
            d.ivec &= (uint8_t)~KEYBOARD_INT;
            d.isr &= (uint8_t)~ISTS_RBI;
        }
    } else if (cmd & CMD_ERX) {
        port_enable_rx(p);
    }

    switch ((cmd >> 4) & 7) {
    case CR_RST_MR:
        p->mode_ptr = 0;
        break;
    case CR_RST_RX:
        p->stat &= (uint8_t)~STS_RXR;
        p->conf &= (uint8_t)~CNF_ERX;
        fifo_clear(&p->rx_fifo);
        p->rx_shift_full = false;
        break;
    case CR_RST_TX:
        p->stat &= (uint8_t)~(STS_TXR | STS_TXE);
        p->conf &= (uint8_t)~CNF_ETX;
        p->tx_holding_full = false;
        p->tx_shift_full = false;
        break;
    case CR_RST_ERR:
        p->stat &= (uint8_t)~(STS_RXB | STS_FER | STS_PER | STS_OER);
        break;
    case CR_RST_BRK:
        d.isr &= (uint8_t)~dbk_ists;
        break;
    case CR_START_BRK:
        if (port_loopback(p)) {
            if (core_rust_compat) {
                p->stat |= STS_RXB | STS_PER;
            } else {
                /* A 2681 receives a break as one NUL character with
                 * Received Break and Framing Error. The I/O self test
                 * of firmware 8;7;3 checks this, and repeats forever
                 * when it does not see it. */
                port_rx_char(p, 0);
                p->stat |= STS_RXB | STS_FER;
            }
            d.isr |= dbk_ists;
        }
        break;
    case CR_STOP_BRK:
        if (port_loopback(p))
            d.isr |= dbk_ists;
        break;
    }
}

int duart_read_byte(addr_t address, uint8_t *val)
{
    struct port *p;
    uint8_t c;

    switch ((uint8_t)(address - START_ADDR)) {
    case MR12A:
        p = &d.ports[0];
        *val = p->mode[p->mode_ptr];
        p->mode_ptr = (p->mode_ptr + 1) % 2;
        return E_OK;
    case CSRA:
        *val = d.ports[0].stat;
        return E_OK;
    case RHRA:
        d.isr &= (uint8_t)~ISTS_RAI;
        d.ivec &= (uint8_t)~RX_INT;
        *val = port_rx_read_char(&d.ports[0], &c) ? c : 0;
        return E_OK;
    case IPCR_ACR:
        *val = d.ipcr;
        d.ipcr &= (uint8_t)~0x0f;
        d.ivec = 0;
        d.isr &= (uint8_t)~ISTS_IPC;
        return E_OK;
    case ISR_MASK:
        *val = d.isr;
        return E_OK;
    case MR12B:
        p = &d.ports[1];
        *val = p->mode[p->mode_ptr];
        p->mode_ptr = (p->mode_ptr + 1) % 2;
        return E_OK;
    case CSRB:
        *val = d.ports[1].stat;
        return E_OK;
    case RHRB:
        /* The Rust core clears RAI (port A) here, not RBI. */
        d.isr &= (uint8_t)~(core_rust_compat ? ISTS_RAI : ISTS_RBI);
        d.ivec &= (uint8_t)~KEYBOARD_INT;
        *val = port_rx_read_char(&d.ports[1], &c) ? c : 0;
        return E_OK;
    case IP_OPCR:
        *val = d.inprt;
        return E_OK;
    default:
        return E_BUS_NODEV;
    }
}

int duart_write_byte(addr_t address, uint8_t val)
{
    struct port *p;

    switch ((uint8_t)(address - START_ADDR)) {
    case MR12A:
        p = &d.ports[0];
        p->mode[p->mode_ptr] = val;
        p->mode_ptr = (p->mode_ptr + 1) % 2;
        break;
    case CSRA:
        d.ports[0].char_delay = delay_rate(val, d.acr);
        break;
    case CRA:
        handle_command(val, 0);
        break;
    case THRA:
        p = &d.ports[0];
        p->tx_holding_reg = val;
        p->tx_holding_full = true;
        p->stat &= (uint8_t)~(STS_TXR | STS_TXE);
        d.isr &= (uint8_t)~ISTS_TAI;
        break;
    case IPCR_ACR:
        d.acr = val;
        break;
    case ISR_MASK:
        d.imr = val;
        break;
    case MR12B:
        p = &d.ports[1];
        p->mode[p->mode_ptr] = val;
        p->mode_ptr = (p->mode_ptr + 1) % 2;
        break;
    case CSRB:
        d.ports[1].char_delay = delay_rate(val, d.acr);
        break;
    case CRB:
        handle_command(val, 1);
        break;
    case THRB:
        p = &d.ports[1];
        p->tx_holding_reg = val;
        p->tx_holding_full = true;
        p->stat &= (uint8_t)~(STS_TXR | STS_TXE);
        d.isr &= (uint8_t)~ISTS_TBI;
        break;
    case IP_OPCR:
        break;
    case OPBITS_SET:
        d.outprt |= val;
        break;
    case OPBITS_RESET:
        d.outprt &= (uint8_t)~val;
        break;
    default:
        break;
    }
    return E_OK;
}
