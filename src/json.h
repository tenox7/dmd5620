/*
 * Minimal JSON reader and writer for the MCP bridge and control socket.
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#ifndef __JSON_H__
#define __JSON_H__

#include <stdarg.h>
#include <stddef.h>

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jnode {
    int type;
    const char *raw; /* span in the source text */
    size_t rawlen;
    double num;
    int boolean;
    char *str; /* decoded string (J_STR) */
    char *key; /* member name, if inside an object */
    struct jnode *child, *next;
} jnode;

jnode *json_parse(const char *text, size_t len);
void json_free(jnode *n);
jnode *json_get(const jnode *obj, const char *key);
const char *json_str(const jnode *n, const char *dflt);
double json_num(const jnode *n, double dflt);
int json_bool(const jnode *n, int dflt);

/* Growable string buffer */
typedef struct {
    char *buf;
    size_t len, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_putn(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_printf(sbuf *b, const char *fmt, ...);
void sb_json_str(sbuf *b, const char *s); /* quoted + escaped */

#endif
