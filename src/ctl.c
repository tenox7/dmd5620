/*
 * Control socket: lets the MCP bridge (dmd5620 --mcp-bridge) drive a
 * running emulator: screenshots, keyboard, mouse.
 *
 * One local (AF_UNIX) socket per emulator in /tmp/dmd5620-$UID (mode
 * 0700), or %TEMP%\dmd5620 on Windows.
 * Protocol: the client sends one JSON line {"tool": NAME, "args": {...}}
 * and gets one JSON line back: {"ok": bool, "text": "...", "png": "<base64>"}.
 *
 * A thread accepts the connections. It turns each tool call into a list
 * of actions. The main loop runs the actions (ctl_service), so only the
 * main thread ever touches the emulator core.
 *
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#include "ctl.h"
#include "dmd_5620.h"
#include "json.h"
#include "png.h"

#include "plat.h"

#include <SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern volatile bool running;

#define VRAM_SIZE   (WIDTH_IN_BYTES * HEIGHT)
#define MAX_REQUEST (1 << 20)
#define MAX_ACTIONS 20000

enum { A_KEY, A_HOME, A_MOVE, A_DOWN, A_UP, A_WAIT, A_SNAP, A_INFO, A_RESET, A_QUIT };

/*
 * The DMD mouse is relative: the firmware reads two counters at each
 * vertical blank and moves its cursor by the change, and stops it at the
 * screen edges. Y counts up. To put the cursor at an exact x,y we first
 * "home" it: a big move up and left pushes it into the top-left corner,
 * then we move by x,y from there.
 */
#define HOME_DELTA 2000

typedef struct {
    int op, a, b;
} act_t;

typedef struct {
    act_t *acts;
    int n, cap, pc;
    uint64_t resume_at;
    bool done;
    /* results, filled on the main thread */
    bool have_snap;
    uint8_t snap[VRAM_SIZE];
    uint8_t oport;
    uint32_t cpu_pc;
    int mx, my, buttons;
    int fwx, fwy; /* the firmware's cursor, -1 if unknown */
} req_t;

static SDL_mutex *mu;
static SDL_cond *cv;
static req_t *pending; /* guarded by mu */
static bool redraw;    /* guarded by mu */

/* main thread state */
static int mouse_x, mouse_y, mouse_buttons; /* where the cursor should be */
static bool mouse_known;                    /* false until the first homing */
static int ctr_x, ctr_y;                    /* mouse counters given to the core */
static char *paste_buf;
static size_t paste_len, paste_pos;
static uint64_t paste_next;

static ctl_info info;
static char vm_name[64];
static char sock_path[400];
static sock_t listen_fd = SOCK_BAD;
static uint64_t start_ms;

uint64_t ctl_now_ms(void) {
    return plat_ms();
}

/* ------------------------------------------------------------------ */
/* Keyboard codes, as the SDL front end sends them                     */
/* ------------------------------------------------------------------ */

struct keyname {
    const char *name;
    int code;
};

static const struct keyname KEYS[] = {
    {"return", 0xe7},    {"enter", 0xe7},  {"tab", 0xd0},    {"backspace", 0xd1},
    {"bs", 0xd1},        {"escape", 0x1b}, {"esc", 0x1b},    {"delete", 0xfe},
    {"del", 0xfe},       {"up", 0xc1},     {"down", 0xc2},   {"right", 0xc3},
    {"left", 0xc4},      {"space", ' '},   {"setup", 0xae},  {"f9", 0xae},
    {"f1", 0xe8},        {"f2", 0xe9},     {"f3", 0xea},     {"f4", 0xeb},
    {"f5", 0xec},        {"f6", 0xed},     {"f7", 0xee},     {"f8", 0xef},
    {"pf1", 0xe8},       {"pf2", 0xe9},    {"pf3", 0xea},    {"pf4", 0xeb},
    {"pf5", 0xec},       {"pf6", 0xed},    {"pf7", 0xee},    {"pf8", 0xef},
    {NULL, 0}};

/* ASCII character typed as text -> DMD keyboard code, or -1. */
static int char_code(unsigned char c) {
    switch (c) {
    case '\n':
    case '\r':
        return 0xe7;
    case '\t':
        return 0xd0;
    case '\b':
        return 0xd1;
    case 0x7f:
        return 0xfe;
    }
    if (c < 0x80)
        return c; /* printable ASCII and control codes go as they are */
    return -1;
}

/* "Return", "ctrl+c", "shift+setup", "a", ... -> code, or -1 */
static int key_code(const char *spec) {
    bool ctrl = false, shift = false;
    const char *p = spec;

    if (strlen(p) == 1)
        return char_code((unsigned char)*p);

    for (;;) {
        if (!strncasecmp(p, "ctrl+", 5)) {
            ctrl = true;
            p += 5;
        } else if (!strncasecmp(p, "control+", 8)) {
            ctrl = true;
            p += 8;
        } else if (!strncasecmp(p, "shift+", 6)) {
            shift = true;
            p += 6;
        } else {
            break;
        }
    }

    if (strlen(p) == 1) {
        unsigned char c = (unsigned char)*p;
        if (ctrl) {
            if (isalpha(c))
                return tolower(c) & 0x1f;
            switch (c) {
            case '[':
                return 0x1b;
            case '\\':
                return 0x1c;
            case ']':
                return 0x1d;
            case '^':
            case '6':
                return 0x1e;
            case '_':
            case '-':
                return 0x1f;
            case '@':
            case '2':
            case ' ':
                return 0x00;
            }
            return -1;
        }
        if (shift && isalpha(c))
            return toupper(c);
        return char_code(c);
    }

    for (const struct keyname *k = KEYS; k->name; k++) {
        if (!strcasecmp(p, k->name)) {
            if (k->code == 0xae && shift)
                return 0x8e; /* shifted Setup */
            return k->code;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Main thread                                                        */
/* ------------------------------------------------------------------ */

static void set_counters(int dx, int dy_down) {
    ctr_x += dx;
    ctr_y -= dy_down; /* the DMD counts Y up */
    dmd_mouse_move((uint16_t)ctr_x, (uint16_t)ctr_y);
}

/*
 * The host mouse is absolute: the DMD cursor goes where the person's
 * pointer is, pixel for pixel. We give the counters the difference between
 * the pointer and the cursor (mouse_x, mouse_y). When the mouse rests, we
 * read the firmware's own cursor from RAM and take it as the truth, so we
 * correct drift and a cursor that the firmware moved itself. A move to an
 * edge pushes past it: the firmware stops the cursor at the edge, so that
 * axis is exact again even without the RAM (a downloaded terminal program
 * can keep its cursor somewhere else).
 */
#define EDGE_PUSH 64
#define REST_MS   50 /* 3 vertical blanks: the firmware took the counters */

static uint8_t fw_ver;
static bool human_owns;      /* the last move came from the host mouse */
static int human_x, human_y; /* the host pointer, in DMD pixels */
static uint64_t last_send_ms;
static bool moved_since_check;
static int fw_prev_x = -1, fw_prev_y = -1;
static bool fw_live; /* the RAM cursor follows our moves */

void ctl_mouse_firmware(uint8_t fw_version) {
    fw_ver = fw_version;
}

/* The firmware's cursor (found by search: a Point the vertical blank
 * interrupt updates from the counters). */
static bool fw_cursor(int *x, int *y) {
    uint32_t a = fw_ver == 2 ? 0x71a7a8 : fw_ver == 1 ? 0x71951c : 0;
    uint8_t b[4];

    if (!a)
        return false;
    for (int i = 0; i < 4; i++)
        if (dmd_read_byte(a + (uint32_t)i, &b[i]) != DMD_SUCCESS)
            return false;
    *x = b[0] << 8 | b[1];
    *y = b[2] << 8 | b[3];
    return *x < WIDTH && *y < HEIGHT;
}

static void move_to(int x, int y, uint64_t now) {
    int dx = x - mouse_x, dy = y - mouse_y;

    dx += x == 0 ? -EDGE_PUSH : x == WIDTH - 1 ? EDGE_PUSH : 0;
    dy += y == 0 ? -EDGE_PUSH : y == HEIGHT - 1 ? EDGE_PUSH : 0;
    if (!dx && !dy)
        return;
    set_counters(dx, dy);
    last_send_ms = now;
    if (x != mouse_x || y != mouse_y)
        moved_since_check = true;
    mouse_x = x;
    mouse_y = y;
}

/* The host pointer is at x,y over the window (DMD pixels). */
void ctl_human_mouse(int x, int y) {
    human_x = x < 0 ? 0 : x >= WIDTH ? WIDTH - 1 : x;
    human_y = y < 0 ? 0 : y >= HEIGHT ? HEIGHT - 1 : y;
    human_owns = true;
    move_to(human_x, human_y, plat_ms());
}

/* At rest: compare with the firmware's cursor. */
static void service_mouse(uint64_t now) {
    int fx, fy;

    if (now - last_send_ms < REST_MS || !fw_cursor(&fx, &fy))
        return;
    if (fx != mouse_x || fy != mouse_y) {
        if (moved_since_check && fx == fw_prev_x && fy == fw_prev_y) {
            fw_live = false; /* it did not follow our moves */
        } else if (fw_live || fw_prev_x < 0 || moved_since_check) {
            fw_live = true;
            mouse_x = fx;
            mouse_y = fy;
            if (human_owns)
                move_to(human_x, human_y, now);
        }
    } else {
        fw_live = true;
    }
    fw_prev_x = fx;
    fw_prev_y = fy;
    moved_since_check = false;
}

void ctl_paste(const char *text) {
    size_t n = strlen(text);
    char *nb = malloc(paste_len - paste_pos + n + 1);
    if (!nb)
        return;
    memcpy(nb, paste_buf ? paste_buf + paste_pos : "", paste_len - paste_pos);
    memcpy(nb + (paste_len - paste_pos), text, n + 1);
    free(paste_buf);
    paste_buf = nb;
    paste_len = paste_len - paste_pos + n;
    paste_pos = 0;
}

static void service_paste(uint64_t now) {
    while (paste_buf && paste_pos < paste_len && now >= paste_next) {
        int code = char_code((unsigned char)paste_buf[paste_pos++]);
        if (code >= 0) {
            dmd_keyboard_rx((uint8_t)code);
            paste_next = now + 10;
            break;
        }
    }
    if (paste_buf && paste_pos >= paste_len) {
        free(paste_buf);
        paste_buf = NULL;
        paste_len = paste_pos = 0;
    }
}

bool ctl_take_redraw(void) {
    if (!mu)
        return false;
    SDL_LockMutex(mu);
    bool r = redraw;
    redraw = false;
    SDL_UnlockMutex(mu);
    return r;
}

void ctl_service(uint64_t now) {
    service_paste(now);
    if (!mu) {
        service_mouse(now);
        return; /* no MCP: only the paste and the mouse */
    }

    SDL_LockMutex(mu);
    req_t *r = pending;
    if (!r || r->done)
        service_mouse(now); /* not in the middle of an agent's moves */
    if (r && !r->done && now >= r->resume_at) {
        while (r->pc < r->n) {
            act_t *a = &r->acts[r->pc++];
            switch (a->op) {
            case A_KEY:
                dmd_keyboard_rx((uint8_t)a->a);
                break;
            case A_HOME:
                /* Not while a button is down: that would drag through
                 * a menu. Then we trust the last known position. */
                human_owns = false;
                if (mouse_buttons == 0 || !mouse_known) {
                    set_counters(-HOME_DELTA, -HOME_DELTA);
                    if (mouse_x || mouse_y)
                        moved_since_check = true;
                    mouse_x = mouse_y = 0;
                    mouse_known = true;
                    last_send_ms = now;
                }
                break;
            case A_MOVE:
                human_owns = false;
                set_counters(a->a - mouse_x, a->b - mouse_y);
                if (a->a != mouse_x || a->b != mouse_y)
                    moved_since_check = true;
                mouse_x = a->a;
                mouse_y = a->b;
                last_send_ms = now;
                break;
            case A_DOWN:
                dmd_mouse_down((uint8_t)a->a);
                mouse_buttons |= 1 << a->a;
                break;
            case A_UP:
                dmd_mouse_up((uint8_t)a->a);
                mouse_buttons &= ~(1 << a->a);
                break;
            case A_WAIT:
                if (a->a > 0) {
                    r->resume_at = now + (uint64_t)a->a;
                    goto out;
                }
                break;
            case A_SNAP: {
                uint8_t *vram = dmd_video_ram();
                if (vram) {
                    memcpy(r->snap, vram, VRAM_SIZE);
                    r->have_snap = true;
                }
                dmd_get_duart_output_port(&r->oport);
                redraw = true; /* reading vram clears the dirty flag */
                break;
            }
            case A_INFO:
                dmd_get_pc(&r->cpu_pc);
                if (!fw_cursor(&r->fwx, &r->fwy))
                    r->fwx = r->fwy = -1;
                break;
            case A_RESET:
                dmd_init(info.fw_version);
                break;
            case A_QUIT:
                running = false;
                ctl_stop(); /* vm-list must not show it any more */
                break;
            }
        }
        r->mx = mouse_x;
        r->my = mouse_y;
        r->buttons = mouse_buttons;
        r->done = true;
        SDL_CondBroadcast(cv);
    }
out:
    SDL_UnlockMutex(mu);
}

/* ------------------------------------------------------------------ */
/* Control thread                                                     */
/* ------------------------------------------------------------------ */

static void add(req_t *r, int op, int a, int b) {
    if (r->n >= MAX_ACTIONS)
        return;
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 32;
        act_t *na = realloc(r->acts, sizeof(act_t) * (size_t)cap);
        if (!na)
            return;
        r->acts = na;
        r->cap = cap;
    }
    r->acts[r->n++] = (act_t){op, a, b};
}

/* Homes the cursor, then moves it to x,y (see HOME_DELTA). */
static void add_move(req_t *r, int x, int y) {
    add(r, A_HOME, 0, 0);
    add(r, A_WAIT, 40, 0);
    add(r, A_MOVE, x, y);
    add(r, A_WAIT, 50, 0);
}

static int total_wait_ms(const req_t *r) {
    long t = 0;
    for (int i = 0; i < r->n; i++) {
        if (r->acts[i].op == A_WAIT)
            t += r->acts[i].a > 16 ? r->acts[i].a : 16;
        else if (r->acts[i].op == A_KEY)
            t += 1;
    }
    return t > 600000 ? 600000 : (int)t;
}

/* Hands the request to the main loop and waits for it. */
static bool run_req(req_t *r) {
    uint64_t deadline = plat_ms() + (uint64_t)total_wait_ms(r) + 10000;

    SDL_LockMutex(mu);
    r->resume_at = 0;
    r->done = false;
    pending = r;
    while (!r->done) {
        uint64_t now = plat_ms();
        if (now >= deadline)
            break;
        SDL_CondWaitTimeout(cv, mu, (Uint32)(deadline - now));
    }
    bool ok = r->done;
    pending = NULL;
    SDL_UnlockMutex(mu);
    return ok;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int get_int(const jnode *args, const char *key, int dflt) {
    const jnode *n = json_get(args, key);
    if (n && n->type == J_NUM)
        return (int)n->num;
    if (n && n->type == J_STR && n->str[0])
        return atoi(n->str);
    return dflt;
}

/* Builds the PNG of the snapshot (with optional region and zoom). */
static char *snap_png(const req_t *r, const jnode *args, char *note, size_t notelen) {
    int x = 0, y = 0, w = WIDTH, h = HEIGHT;
    const jnode *reg = json_get(args, "region");
    if (reg && reg->type == J_OBJ) {
        x = clampi(get_int(reg, "x", 0), 0, WIDTH - 1);
        y = clampi(get_int(reg, "y", 0), 0, HEIGHT - 1);
        w = clampi(get_int(reg, "w", WIDTH - x), 1, WIDTH - x);
        h = clampi(get_int(reg, "h", HEIGHT - y), 1, HEIGHT - y);
    }
    int zoom = clampi(get_int(args, "zoom", 1), 1, 8);
    int ow = w * zoom, oh = h * zoom;
    if ((long)ow * oh > 4L * WIDTH * HEIGHT) {
        zoom = 1;
        ow = w;
        oh = h;
    }

    int stride = (ow + 7) / 8;
    uint8_t *rows = calloc((size_t)stride * (size_t)oh, 1);
    if (!rows)
        return NULL;
    for (int oy = 0; oy < oh; oy++) {
        int sy = y + oy / zoom;
        const uint8_t *src = r->snap + (size_t)sy * WIDTH_IN_BYTES;
        uint8_t *dst = rows + (size_t)oy * (size_t)stride;
        for (int ox = 0; ox < ow; ox++) {
            int sx = x + ox / zoom;
            if ((src[sx >> 3] >> (7 - (sx & 7))) & 1)
                dst[ox >> 3] |= (uint8_t)(0x80 >> (ox & 7));
        }
    }

    /* Same colors as the window: bit set = foreground. */
    uint32_t light = ((uint32_t)COLOR_LIGHT.r << 16) | ((uint32_t)COLOR_LIGHT.g << 8) |
                     COLOR_LIGHT.b;
    uint32_t dark =
        ((uint32_t)COLOR_DARK.r << 16) | ((uint32_t)COLOR_DARK.g << 8) | COLOR_DARK.b;
    uint32_t fg = (r->oport & 0x2) ? dark : light;
    uint32_t bg = (r->oport & 0x2) ? light : dark;

    size_t plen = 0;
    uint8_t *png = png_encode_1bit(rows, ow, oh, stride, bg, fg, &plen);
    free(rows);
    if (!png)
        return NULL;
    char *b64 = base64_encode(png, plen);
    free(png);

    if (zoom > 1 || w != WIDTH || h != HEIGHT)
        snprintf(note, notelen, "region x=%d y=%d w=%d h=%d, zoom %d (%dx%d px). ", x, y,
                 w, h, zoom, ow, oh);
    else
        snprintf(note, notelen, "screen 800x1024. ");
    return b64;
}

static const char *btn_list(int b, char *buf, size_t n) {
    buf[0] = 0;
    for (int i = 0; i < 3; i++) {
        if (b & (1 << i)) {
            size_t l = strlen(buf);
            snprintf(buf + l, n - l, "%s%d", l ? "," : "", i + 1);
        }
    }
    if (!buf[0])
        snprintf(buf, n, "none");
    return buf;
}

/* Optional screenshot after an input. */
static void then_snap(req_t *r, const jnode *args) {
    if (json_bool(json_get(args, "screenshot"), 0)) {
        add(r, A_WAIT, clampi(get_int(args, "wait-ms", 300), 0, 60000), 0);
        add(r, A_SNAP, 0, 0);
    }
}

static int get_button(const jnode *args, sbuf *err) {
    const jnode *n = json_get(args, "button");
    int b = 1;
    if (n && n->type == J_STR) {
        if (!strcasecmp(n->str, "left"))
            b = 1;
        else if (!strcasecmp(n->str, "middle"))
            b = 2;
        else if (!strcasecmp(n->str, "right"))
            b = 3;
        else
            b = atoi(n->str);
    } else if (n && n->type == J_NUM) {
        b = (int)n->num;
    }
    if (b < 1 || b > 3) {
        sb_puts(err, "button must be 1 (left), 2 (middle) or 3 (right)");
        return -1;
    }
    return b - 1;
}

static bool get_xy(const jnode *args, int *x, int *y, sbuf *err) {
    const jnode *jx = json_get(args, "x"), *jy = json_get(args, "y");
    if (!jx && !jy)
        return false;
    if (!jx || !jy || jx->type != J_NUM || jy->type != J_NUM) {
        sb_puts(err, "give both x and y as numbers");
        return false;
    }
    *x = (int)jx->num;
    *y = (int)jy->num;
    if (*x < 0 || *x >= WIDTH || *y < 0 || *y >= HEIGHT) {
        sb_printf(err, "x must be 0-%d and y 0-%d", WIDTH - 1, HEIGHT - 1);
        return false;
    }
    return true;
}

/* Runs one tool call. Fills text, *png, and returns ok. */
static bool do_tool(const char *tool, const jnode *args, sbuf *text, char **png) {
    req_t *r = calloc(1, sizeof(req_t));
    sbuf err;
    bool ok = true;
    char note[160] = "";
    char bl[16];

    if (!r) {
        sb_puts(text, "out of memory");
        return false;
    }
    sb_init(&err);

    if (!strcmp(tool, "screenshot")) {
        add(r, A_WAIT, clampi(get_int(args, "wait-ms", 0), 0, 60000), 0);
        add(r, A_SNAP, 0, 0);
    } else if (!strcmp(tool, "key")) {
        const char *k = json_str(json_get(args, "key"), NULL);
        int code = get_int(args, "code", -1);
        int rep = clampi(get_int(args, "repeat", 1), 1, 100);
        if (code < 0 && k)
            code = key_code(k);
        if (code < 0 || code > 255) {
            sb_printf(&err, "unknown key \"%s\". Use a character, Return, Tab, BackSpace, "
                            "Escape, Delete, Up, Down, Left, Right, F1-F8, Setup, "
                            "ctrl+X, shift+X, or code 0-255",
                      k ? k : "");
        } else {
            for (int i = 0; i < rep; i++) {
                add(r, A_KEY, code, 0);
                add(r, A_WAIT, 30, 0);
            }
            sb_printf(text, "key 0x%02x x%d. ", code, rep);
        }
        then_snap(r, args);
    } else if (!strcmp(tool, "type-text")) {
        const char *t = json_str(json_get(args, "text"), NULL);
        int delay = clampi(get_int(args, "delay-ms", 10), 0, 1000);
        if (!t) {
            sb_puts(&err, "text is missing");
        } else {
            int n = 0;
            for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
                int code = char_code(*p);
                if (code < 0) {
                    sb_puts(&err, "text must be ASCII (the DMD has no other characters)");
                    break;
                }
                add(r, A_KEY, code, 0);
                add(r, A_WAIT, delay, 0);
                n++;
            }
            if (!err.len)
                sb_printf(text, "typed %d characters. ", n);
        }
        then_snap(r, args);
    } else if (!strcmp(tool, "mouse-move")) {
        int x = 0, y = 0;
        if (get_xy(args, &x, &y, &err)) {
            add_move(r, x, y);
            sb_printf(text, "mouse at %d,%d. ", x, y);
        } else if (!err.len) {
            sb_puts(&err, "x and y are missing");
        }
        then_snap(r, args);
    } else if (!strcmp(tool, "mouse-click")) {
        int x = 0, y = 0, b = get_button(args, &err);
        bool have_xy = get_xy(args, &x, &y, &err);
        if (!err.len) {
            if (have_xy) {
                add_move(r, x, y);
            }
            add(r, A_DOWN, b, 0);
            add(r, A_WAIT, clampi(get_int(args, "hold-ms", 100), 20, 10000), 0);
            add(r, A_UP, b, 0);
            add(r, A_WAIT, 50, 0);
            sb_printf(text, "clicked button %d. ", b + 1);
        }
        then_snap(r, args);
    } else if (!strcmp(tool, "mouse-button")) {
        int b = get_button(args, &err);
        const char *act = json_str(json_get(args, "action"), "");
        int x = 0, y = 0;
        bool have_xy = get_xy(args, &x, &y, &err);
        if (!err.len && strcmp(act, "down") && strcmp(act, "up"))
            sb_puts(&err, "action must be \"down\" or \"up\"");
        if (!err.len) {
            if (have_xy) {
                add_move(r, x, y);
            }
            add(r, !strcmp(act, "down") ? A_DOWN : A_UP, b, 0);
            add(r, A_WAIT, 50, 0);
            sb_printf(text, "button %d %s. ", b + 1, act);
        }
        then_snap(r, args);
    } else if (!strcmp(tool, "vm-info")) {
        add(r, A_INFO, 0, 0);
    } else if (!strcmp(tool, "vm-power")) {
        const char *act = json_str(json_get(args, "action"), "");
        if (!strcmp(act, "reset")) {
            add(r, A_RESET, 0, 0);
            sb_puts(text, "terminal reset (firmware restarts). ");
        } else if (!strcmp(act, "quit")) {
            add(r, A_QUIT, 0, 0);
            sb_puts(text, "emulator quits (NVRAM is saved if -n was given). ");
        } else {
            sb_puts(&err, "action must be \"reset\" or \"quit\"");
        }
    } else {
        sb_printf(&err, "unknown tool %s", tool);
    }

    if (err.len) {
        sb_free(text);
        sb_puts(text, err.buf);
        ok = false;
    } else if (!run_req(r)) {
        sb_puts(text, "timeout: the emulator main loop did not answer");
        ok = false;
    } else {
        if (r->have_snap) {
            *png = snap_png(r, args, note, sizeof(note));
            if (*png)
                sb_puts(text, note);
        }
        if (!strcmp(tool, "vm-info")) {
            sb_printf(text,
                      "name: %s\npid: %d\nfirmware: %s\nconnection: %s\n"
                      "window: %s\nuptime: %llu s\nscreen: 800x1024, 1 bit, "
                      "origin top-left\nmouse: %d,%d, buttons down: %s\n"
                      "firmware cursor: %d,%d%s\ncpu pc: 0x%08x\nsocket: %s",
                      vm_name, plat_pid(), info.firmware, info.connection,
                      info.headless ? "no (headless)" : "yes",
                      (unsigned long long)((ctl_now_ms() - start_ms) / 1000), r->mx, r->my,
                      btn_list(r->buttons, bl, sizeof(bl)), r->fwx, r->fwy,
                      fw_live ? "" : " (not in use)", r->cpu_pc, sock_path);
        } else if (strcmp(tool, "vm-power")) {
            sb_printf(text, "mouse %d,%d, buttons down: %s.", r->mx, r->my,
                      btn_list(r->buttons, bl, sizeof(bl)));
        }
    }

    sb_free(&err);
    free(r->acts);
    free(r);
    return ok;
}

static void handle_conn(sock_t fd) {
    sbuf in, out, text;
    char buf[4096];
    char *png = NULL;

    sock_set_timeout(fd, 10000);

    sb_init(&in);
    sb_init(&out);
    sb_init(&text);

    for (;;) {
        int n = sock_recv(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        sb_putn(&in, buf, (size_t)n);
        if (memchr(buf, '\n', (size_t)n) || in.len > MAX_REQUEST)
            break;
    }

    bool ok = false;
    jnode *req = in.len ? json_parse(in.buf, strcspn(in.buf, "\n")) : NULL;
    const char *tool = json_str(json_get(req, "tool"), NULL);
    if (!tool) {
        sb_puts(&text, "bad request");
    } else {
        ok = do_tool(tool, json_get(req, "args"), &text, &png);
    }
    json_free(req);

    sb_printf(&out, "{\"ok\":%s,\"text\":", ok ? "true" : "false");
    sb_json_str(&out, text.buf ? text.buf : "");
    if (png) {
        sb_puts(&out, ",\"png\":\"");
        sb_puts(&out, png);
        sb_puts(&out, "\"");
    }
    sb_puts(&out, "}\n");

    sock_send_all(fd, out.buf, out.len);

    free(png);
    sb_free(&in);
    sb_free(&out);
    sb_free(&text);
}

static int ctl_thread(void *arg) {
    (void)arg;
    for (;;) {
        sock_t fd = sock_accept(listen_fd);
        if (fd == SOCK_BAD) {
            if (!running)
                break;
            plat_sleep_ms(10); /* EINTR, aborted connection */
            continue;
        }
        handle_conn(fd);
        sock_close(fd);
    }
    return 0;
}

static bool name_ok(const char *s) {
    if (!*s || strlen(s) > 32)
        return false;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
            return false;
    }
    return true;
}

int ctl_list_vms(vm_entry *out, int max) {
    char dir[300];
    int n = 0;
    plat_sock_dir(dir, sizeof(dir), false);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len < 8 || strcmp(e->d_name + len - 5, ".sock"))
            continue;
        char base[256];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 5), e->d_name);
        char *dot = strrchr(base, '.');
        if (!dot || dot == base)
            continue;
        int pid = atoi(dot + 1);
        *dot = 0;
        if (pid <= 0 || strlen(base) >= sizeof(out[n].name))
            continue;
        char path[sizeof(out[n].path)];
        snprintf(path, sizeof(path), "%s" PLAT_SEP "%s", dir, e->d_name);
        if (!plat_pid_alive(pid)) {
            plat_remove(path); /* left over from an emulator that died */
            continue;
        }
        snprintf(out[n].name, sizeof(out[n].name), "%s", base);
        snprintf(out[n].path, sizeof(out[n].path), "%s", path);
        out[n].pid = pid;
        n++;
    }
    closedir(d);
    return n;
}

/* true if another live emulator already uses this name */
static bool name_taken(const char *name) {
    vm_entry v[64];
    int n = ctl_list_vms(v, 64);
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i].name, name) && v[i].pid != plat_pid())
            return true;
    }
    return false;
}

int ctl_start(const ctl_info *in) {
    char dir[300];

    info = *in;
    start_ms = ctl_now_ms();
    if (!plat_sock_dir(dir, sizeof(dir), true))
        return -1;

    const char *name = in->name ? in->name : "dmd5620";
    if (!name_ok(name)) {
        fprintf(stderr, "mcp: name may have only letters, digits, - and _ (max 32)\n");
        return -1;
    }
    if (!in->name && name_taken(name))
        snprintf(vm_name, sizeof(vm_name), "%s-%d", name, plat_pid());
    else
        snprintf(vm_name, sizeof(vm_name), "%s", name);

    snprintf(sock_path, sizeof(sock_path), "%s" PLAT_SEP "%s.%d.sock", dir, vm_name, plat_pid());
    listen_fd = sock_listen_local(sock_path);
    if (listen_fd == SOCK_BAD) {
        sock_path[0] = 0;
        return -1;
    }

    mu = SDL_CreateMutex();
    cv = SDL_CreateCond();
    SDL_Thread *th = mu && cv ? SDL_CreateThread(ctl_thread, "mcp", NULL) : NULL;
    if (!th) {
        fprintf(stderr, "mcp: cannot start the control thread: %s\n", SDL_GetError());
        ctl_stop();
        return -1;
    }
    SDL_DetachThread(th);
    fprintf(stderr, "MCP: VM \"%s\" on %s\n", vm_name, sock_path);
    return 0;
}

void ctl_stop(void) {
    if (sock_path[0])
        plat_remove(sock_path);
    sock_path[0] = 0;
}
