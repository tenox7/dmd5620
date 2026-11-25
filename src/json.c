/*
 * Minimal JSON reader and writer for the MCP bridge and control socket.
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p, *end;
    int depth;
} jparser;

static void skip_ws(jparser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static jnode *new_node(int type, const char *start) {
    jnode *n = calloc(1, sizeof(jnode));
    if (n) {
        n->type = type;
        n->raw = start;
    }
    return n;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int read_u4(jparser *ps, unsigned *out) {
    unsigned v = 0;
    if (ps->end - ps->p < 4)
        return -1;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0)
            return -1;
        v = (v << 4) | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void put_utf8(sbuf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) {
        t[0] = (char)cp;
        sb_putn(b, t, 1);
    } else if (cp < 0x800) {
        t[0] = (char)(0xc0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3f));
        sb_putn(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xe0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        t[2] = (char)(0x80 | (cp & 0x3f));
        sb_putn(b, t, 3);
    } else {
        t[0] = (char)(0xf0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        t[3] = (char)(0x80 | (cp & 0x3f));
        sb_putn(b, t, 4);
    }
}

/* Parses a string at ps->p (which points at the opening quote). */
static char *parse_string(jparser *ps) {
    sbuf b;
    sb_init(&b);
    ps->p++; /* opening quote */
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            sb_putn(&b, "", 0);
            if (!b.buf) {
                b.buf = calloc(1, 1);
            }
            return b.buf;
        }
        if ((unsigned char)c < 0x20)
            break;
        if (c != '\\') {
            sb_putn(&b, &c, 1);
            continue;
        }
        if (ps->p >= ps->end)
            break;
        c = *ps->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            sb_putn(&b, &c, 1);
            break;
        case 'b':
            sb_putn(&b, "\b", 1);
            break;
        case 'f':
            sb_putn(&b, "\f", 1);
            break;
        case 'n':
            sb_putn(&b, "\n", 1);
            break;
        case 'r':
            sb_putn(&b, "\r", 1);
            break;
        case 't':
            sb_putn(&b, "\t", 1);
            break;
        case 'u': {
            unsigned cp, lo;
            if (read_u4(ps, &cp) < 0)
                goto fail;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    if (read_u4(ps, &lo) < 0 || lo < 0xdc00 || lo > 0xdfff)
                        goto fail;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                } else {
                    goto fail;
                }
            }
            put_utf8(&b, cp);
            break;
        }
        default:
            goto fail;
        }
    }
fail:
    sb_free(&b);
    return NULL;
}

static jnode *parse_value(jparser *ps);

static jnode *parse_value(jparser *ps) {
    jnode *n = NULL;
    skip_ws(ps);
    if (ps->p >= ps->end || ++ps->depth > 64)
        return NULL;
    const char *start = ps->p;
    char c = *ps->p;

    if (c == '{' || c == '[') {
        int is_obj = (c == '{');
        char close = is_obj ? '}' : ']';
        jnode *last = NULL;
        n = new_node(is_obj ? J_OBJ : J_ARR, start);
        if (!n)
            return NULL;
        ps->p++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
        } else {
            for (;;) {
                char *key = NULL;
                skip_ws(ps);
                if (is_obj) {
                    if (ps->p >= ps->end || *ps->p != '"')
                        goto fail;
                    key = parse_string(ps);
                    if (!key)
                        goto fail;
                    skip_ws(ps);
                    if (ps->p >= ps->end || *ps->p != ':') {
                        free(key);
                        goto fail;
                    }
                    ps->p++;
                }
                jnode *v = parse_value(ps);
                if (!v) {
                    free(key);
                    goto fail;
                }
                v->key = key;
                if (last)
                    last->next = v;
                else
                    n->child = v;
                last = v;
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') {
                    ps->p++;
                    continue;
                }
                if (ps->p < ps->end && *ps->p == close) {
                    ps->p++;
                    break;
                }
                goto fail;
            }
        }
    } else if (c == '"') {
        n = new_node(J_STR, start);
        if (!n)
            return NULL;
        n->str = parse_string(ps);
        if (!n->str)
            goto fail;
    } else if (c == 't' && ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
        n = new_node(J_BOOL, start);
        if (!n)
            return NULL;
        n->boolean = 1;
        ps->p += 4;
    } else if (c == 'f' && ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
        n = new_node(J_BOOL, start);
        if (!n)
            return NULL;
        ps->p += 5;
    } else if (c == 'n' && ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
        n = new_node(J_NULL, start);
        if (!n)
            return NULL;
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t k = 0;
        while (ps->p < ps->end && k < sizeof(tmp) - 1 &&
               strchr("+-0123456789.eE", *ps->p)) {
            tmp[k++] = *ps->p++;
        }
        tmp[k] = 0;
        char *endp;
        double d = strtod(tmp, &endp);
        if (endp == tmp || *endp)
            return NULL;
        n = new_node(J_NUM, start);
        if (!n)
            return NULL;
        n->num = d;
    } else {
        return NULL;
    }
    n->rawlen = (size_t)(ps->p - start);
    ps->depth--;
    return n;
fail:
    json_free(n);
    return NULL;
}

jnode *json_parse(const char *text, size_t len) {
    jparser ps = {text, text + len, 0};
    jnode *n = parse_value(&ps);
    if (!n)
        return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        json_free(n);
        return NULL;
    }
    return n;
}

void json_free(jnode *n) {
    while (n) {
        jnode *next = n->next;
        json_free(n->child);
        free(n->str);
        free(n->key);
        free(n);
        n = next;
    }
}

jnode *json_get(const jnode *obj, const char *key) {
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (jnode *c = obj->child; c; c = c->next) {
        if (c->key && !strcmp(c->key, key))
            return c;
    }
    return NULL;
}

const char *json_str(const jnode *n, const char *dflt) {
    return (n && n->type == J_STR) ? n->str : dflt;
}

double json_num(const jnode *n, double dflt) {
    return (n && n->type == J_NUM) ? n->num : dflt;
}

int json_bool(const jnode *n, int dflt) {
    return (n && n->type == J_BOOL) ? n->boolean : dflt;
}

/* ---- string buffer ---- */

void sb_init(sbuf *b) {
    b->buf = NULL;
    b->len = b->cap = 0;
}

void sb_free(sbuf *b) {
    free(b->buf);
    sb_init(b);
}

void sb_putn(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *nb = realloc(b->buf, cap);
        if (!nb) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        b->buf = nb;
        b->cap = cap;
    }
    if (n)
        memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

void sb_puts(sbuf *b, const char *s) {
    sb_putn(b, s, strlen(s));
}

void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(tmp)) {
        sb_putn(b, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_putn(b, big, (size_t)n);
    free(big);
}

void sb_json_str(sbuf *b, const char *s) {
    sb_putn(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            sb_putn(b, "\\\"", 2);
            break;
        case '\\':
            sb_putn(b, "\\\\", 2);
            break;
        case '\n':
            sb_putn(b, "\\n", 2);
            break;
        case '\r':
            sb_putn(b, "\\r", 2);
            break;
        case '\t':
            sb_putn(b, "\\t", 2);
            break;
        default:
            if (*p < 0x20) {
                sb_printf(b, "\\u%04x", *p);
            } else {
                sb_putn(b, (const char *)p, 1);
            }
        }
    }
    sb_putn(b, "\"", 1);
}
