/*
 * Host line over TCP: raw, or telnet (RFC 854) with the few options a
 * terminal needs: binary, echo, suppress go-ahead, terminal type "dmd"
 * and window size 88x70.
 * MIT license, same as the rest of the DMD 5620 emulator.
 */

#include "host.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251,
    SB = 250, SE = 240,
    O_BINARY = 0, O_ECHO = 1, O_SGA = 3, O_TTYPE = 24, O_NAWS = 31,
};

static sock_t net = SOCK_BAD;
static bool telnet;
static bool us[256], them[256]; /* options on at our side / their side */
static bool binary_out;         /* we send 8-bit data as is */

/* receive parser */
static enum { S_DATA, S_IAC, S_OPT, S_SB, S_SB_IAC, S_CR } st;
static uint8_t verb;
static uint8_t sb[64];
static int sb_len;

static void send_bytes(const uint8_t *b, int n) {
    if (!sock_send_all(net, b, (size_t)n))
        fprintf(stderr, "telnet: send: %s\n", sock_error());
}

static void send_cmd(uint8_t v, uint8_t opt) {
    uint8_t b[3] = {IAC, v, opt};
    send_bytes(b, 3);
}

static void send_naws(void) {
    static const uint8_t b[] = {IAC, SB, O_NAWS, 0, 88, 0, 70, IAC, SE};
    send_bytes(b, sizeof(b));
}

static bool we_can(uint8_t opt) {
    return opt == O_BINARY || opt == O_SGA || opt == O_TTYPE || opt == O_NAWS;
}

static bool they_can(uint8_t opt) {
    return opt == O_BINARY || opt == O_ECHO || opt == O_SGA;
}

/* Answer a request only when it changes the state, so no loops. */
static void negotiate(uint8_t v, uint8_t opt) {
    switch (v) {
    case DO:
        if (we_can(opt)) {
            if (!us[opt]) {
                us[opt] = true;
                send_cmd(WILL, opt);
            }
            if (opt == O_NAWS)
                send_naws();
            if (opt == O_BINARY)
                binary_out = true;
        } else {
            send_cmd(WONT, opt);
        }
        break;
    case DONT:
        if (us[opt]) {
            us[opt] = false;
            send_cmd(WONT, opt);
        }
        if (opt == O_BINARY)
            binary_out = false;
        break;
    case WILL:
        if (they_can(opt)) {
            if (!them[opt]) {
                them[opt] = true;
                send_cmd(DO, opt);
            }
        } else {
            send_cmd(DONT, opt);
        }
        break;
    case WONT:
        if (them[opt]) {
            them[opt] = false;
            send_cmd(DONT, opt);
        }
        break;
    }
}

static void subneg(void) {
    if (sb_len >= 2 && sb[0] == O_TTYPE && sb[1] == 1 /* SEND */) {
        static const uint8_t b[] = {IAC, SB, O_TTYPE, 0 /* IS */, 'D', 'M', 'D', IAC, SE};
        send_bytes(b, sizeof(b));
    }
}

/* Takes telnet commands out of buf in place. Returns the data length. */
static int filter_in(uint8_t *buf, int n) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        switch (st) {
        case S_CR:
            st = S_DATA;
            if (c == 0)
                break; /* CR NUL = CR */
            /* fall through */
        case S_DATA:
            if (c == IAC)
                st = S_IAC;
            else {
                buf[o++] = c;
                if (c == '\r' && !them[O_BINARY])
                    st = S_CR;
            }
            break;
        case S_IAC:
            if (c == IAC) {
                buf[o++] = IAC;
                st = S_DATA;
            } else if (c >= WILL && c <= DONT) {
                verb = c;
                st = S_OPT;
            } else if (c == SB) {
                sb_len = 0;
                st = S_SB;
            } else {
                st = S_DATA; /* NOP, GA, AYT ... ignored */
            }
            break;
        case S_OPT:
            negotiate(verb, c);
            st = S_DATA;
            break;
        case S_SB:
            if (c == IAC)
                st = S_SB_IAC;
            else if (sb_len < (int)sizeof(sb))
                sb[sb_len++] = c;
            break;
        case S_SB_IAC:
            if (c == SE) {
                subneg();
                st = S_DATA;
            } else {
                if (sb_len < (int)sizeof(sb))
                    sb[sb_len++] = c;
                st = S_SB;
            }
            break;
        }
    }
    return o;
}

static int net_read(uint8_t *buf, int max) {
    if (!sock_readable(net))
        return 0;
    int n = sock_recv(net, buf, (size_t)max);
    if (n <= 0)
        return -1;
    return telnet ? filter_in(buf, n) : n;
}

static bool net_write(const uint8_t *buf, int n) {
    if (!telnet)
        return sock_send_all(net, buf, (size_t)n);
    uint8_t out[512];
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (o > (int)sizeof(out) - 4) {
            if (!sock_send_all(net, out, (size_t)o))
                return false;
            o = 0;
        }
        out[o++] = buf[i];
        if (buf[i] == IAC)
            out[o++] = IAC;
        else if (buf[i] == '\r' && !binary_out)
            out[o++] = 0;
    }
    return sock_send_all(net, out, (size_t)o);
}

static void net_close(void) {
    if (net != SOCK_BAD)
        sock_close(net);
    net = SOCK_BAD;
}

static const struct host_ops net_ops = {net_read, net_write, net_close};

bool host_open_net(const char *hostport, bool use_telnet) {
    char host_s[256], err[300] = "";
    const char *port = use_telnet ? "23" : NULL;

    /* host:port, [v6addr]:port, or host (telnet only) */
    snprintf(host_s, sizeof(host_s), "%s", hostport);
    char *h = host_s;
    char *colon;
    if (h[0] == '[' && (colon = strchr(h, ']')) != NULL) {
        *colon = 0;
        h++;
        if (colon[1] == ':')
            port = colon + 2;
    } else if ((colon = strrchr(h, ':')) != NULL && strchr(h, ':') == colon) {
        *colon = 0;
        port = colon + 1;
    }
    if (!port || !*port || !*h) {
        fprintf(stderr, "Give the host as HOST:PORT (got \"%s\").\n", hostport);
        return false;
    }

    net = sock_connect_tcp(h, port, err, sizeof(err));
    if (net == SOCK_BAD) {
        fprintf(stderr, "%s\n", err);
        return false;
    }
    telnet = use_telnet;
    if (telnet) {
        /* Say what we want first; most servers ask anyway. */
        us[O_TTYPE] = us[O_NAWS] = true;
        send_cmd(WILL, O_TTYPE);
        send_cmd(WILL, O_NAWS);
        them[O_SGA] = true;
        send_cmd(DO, O_SGA);
    }
    host = &net_ops;
    return true;
}
