/*
 * MCP bridge: `dmd5620 --mcp-bridge` is a stdio MCP server. It finds
 * the running emulators (local sockets in /tmp/dmd5620-$UID, or
 * %TEMP%\dmd5620 on Windows) and passes each
 * tool call to the one named in `vm`.
 *
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#include "ctl.h"
#include "json.h"
#include "plat.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(x) #x
#define XS(x) S(x)
#define BRIDGE_VERSION XS(VERSION_MAJOR) "." XS(VERSION_MINOR) "." XS(VERSION_BUILD)

static const char *INSTRUCTIONS =
    "You operate an emulated AT&T / Teletype DMD 5620 terminal: a portrait "
    "800x1024 one-bit screen, a keyboard and a three-button mouse.\n"
    "- A person or you must start the emulator first, from a shell: "
    "`dmd5620` (window) or `dmd5620 --headless` (no window). Without -s or -d "
    "it runs your login shell. vm-list shows the running ones.\n"
    "- Look before you act: take a screenshot first. Inputs take "
    "screenshot=true to return a screenshot after wait-ms (default 300).\n"
    "- Coordinates are screen pixels, origin top-left, the same in screenshot, "
    "region and all mouse tools.\n"
    "- type-text types ASCII text (\\n = Return). key presses one key: a "
    "character, Return, Tab, BackSpace, Escape, Delete, Up, Down, Left, Right, "
    "F1-F8, Setup, or ctrl+X.\n"
    "- Mouse buttons: 1 left, 2 middle, 3 right. DMD menus pop up while a "
    "button is held: mouse-button down, mouse-move, then mouse-button up.\n"
    "- screenshot region + zoom make small text easier to read.\n"
    "- Every tool takes vm (a name or PID from vm-list); leave it out when only "
    "one emulator runs.";

#define VM_PROP "\"vm\":{\"type\":\"string\",\"description\":\"VM name or PID from vm-list. " \
                "Leave out when only one runs.\"}"
#define SNAP_PROPS                                                                         \
    "\"screenshot\":{\"type\":\"boolean\",\"description\":\"Return a screenshot after " \
    "the input\"},\"wait-ms\":{\"type\":\"integer\",\"description\":\"Wait before the "   \
    "screenshot (default 300)\"}"
#define XY_PROPS                                                                           \
    "\"x\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":799},"                       \
    "\"y\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1023}"
#define BUTTON_PROP                                                                        \
    "\"button\":{\"type\":\"integer\",\"enum\":[1,2,3],\"description\":\"1 left, 2 "    \
    "middle, 3 right (default 1)\"}"

static const char *TOOLS =
    "{\"tools\":["
    "{\"name\":\"vm-list\",\"description\":\"List the running DMD 5620 emulators: name, PID, "
    "socket.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"

    "{\"name\":\"vm-info\",\"description\":\"Firmware, connection, uptime, mouse position, "
    "CPU PC of one emulator.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP
    "}}},"

    "{\"name\":\"screenshot\",\"description\":\"PNG of the 800x1024 screen, exactly as the "
    "terminal shows it. region crops (screen pixels), zoom enlarges 1-8x.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP ","
    "\"region\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{"
    "\"type\":\"integer\"},\"w\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"}}},"
    "\"zoom\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8},"
    "\"wait-ms\":{\"type\":\"integer\",\"description\":\"Wait before the screenshot\"}}}},"

    "{\"name\":\"key\",\"description\":\"Press one key: a character, Return, Tab, BackSpace, "
    "Escape, Delete, Up, Down, Left, Right, F1-F8, Setup, ctrl+X, shift+X. code sends a raw "
    "DMD keyboard code (0-255) instead.\",\"inputSchema\":{\"type\":\"object\","
    "\"properties\":{" VM_PROP ",\"key\":{\"type\":\"string\"},\"code\":{\"type\":"
    "\"integer\",\"minimum\":0,\"maximum\":255},\"repeat\":{\"type\":\"integer\",\"minimum\":1,"
    "\"maximum\":100}," SNAP_PROPS "}}},"

    "{\"name\":\"type-text\",\"description\":\"Type ASCII text on the DMD keyboard. \\\\n is "
    "Return, \\\\t is Tab.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP
    ",\"text\":{\"type\":\"string\"},\"delay-ms\":{\"type\":\"integer\",\"description\":"
    "\"Pause between characters (default 10)\"}," SNAP_PROPS "},\"required\":[\"text\"]}},"

    "{\"name\":\"mouse-move\",\"description\":\"Move the mouse to x,y (screen pixels, origin "
    "top-left).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP "," XY_PROPS
    "," SNAP_PROPS "},\"required\":[\"x\",\"y\"]}},"

    "{\"name\":\"mouse-click\",\"description\":\"Click a mouse button, at x,y if given.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP "," XY_PROPS
    "," BUTTON_PROP ",\"hold-ms\":{\"type\":\"integer\",\"description\":\"How long the "
    "button stays down (default 100)\"}," SNAP_PROPS "}}},"

    "{\"name\":\"mouse-button\",\"description\":\"Press (down) or release (up) a mouse button, "
    "at x,y if given. For DMD menus: down, mouse-move to the item, up.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP "," XY_PROPS
    "," BUTTON_PROP ",\"action\":{\"type\":\"string\",\"enum\":[\"down\",\"up\"]},"
    SNAP_PROPS "},\"required\":[\"action\"]}},"

    "{\"name\":\"vm-power\",\"description\":\"reset restarts the terminal firmware. quit "
    "ends the emulator. Do not quit an emulator you did not start.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{" VM_PROP ",\"action\":{"
    "\"type\":\"string\",\"enum\":[\"reset\",\"quit\"]}},\"required\":[\"action\"]}}"
    "]}";

static void send_line(sbuf *b) {
    sb_puts(b, "\n");
    fwrite(b->buf, 1, b->len, stdout);
    fflush(stdout);
}

static void reply(const jnode *id, const char *result_json) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_putn(&b, id->raw, id->rawlen);
    sb_puts(&b, ",\"result\":");
    sb_puts(&b, result_json);
    sb_puts(&b, "}");
    send_line(&b);
    sb_free(&b);
}

static void reply_error(const jnode *id, int code, const char *msg) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id)
        sb_putn(&b, id->raw, id->rawlen);
    else
        sb_puts(&b, "null");
    sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_json_str(&b, msg);
    sb_puts(&b, "}}");
    send_line(&b);
    sb_free(&b);
}

static void tool_text(const jnode *id, const char *text, bool is_error) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"content\":[{\"type\":\"text\",\"text\":");
    sb_json_str(&b, text);
    sb_printf(&b, "}],\"isError\":%s}", is_error ? "true" : "false");
    reply(id, b.buf);
    sb_free(&b);
}

static void vm_list(const jnode *id) {
    vm_entry v[64];
    int n = ctl_list_vms(v, 64);
    sbuf t;
    sb_init(&t);
    if (n == 0) {
        sb_puts(&t, "No DMD 5620 emulator is running. Start one from a shell: dmd5620 "
                    "(window) or dmd5620 --headless.");
    } else {
        for (int i = 0; i < n; i++)
            sb_printf(&t, "%s  pid %d  %s\n", v[i].name, v[i].pid, v[i].path);
    }
    tool_text(id, t.buf, false);
    sb_free(&t);
}

/* Finds the emulator for `vm` (name or PID). Fills err on failure. */
static bool pick_vm(const jnode *args, vm_entry *out, sbuf *err) {
    vm_entry v[64];
    int n = ctl_list_vms(v, 64);
    const jnode *want = json_get(args, "vm");
    char key[64] = "";

    if (want && want->type == J_STR)
        snprintf(key, sizeof(key), "%s", want->str);
    else if (want && want->type == J_NUM)
        snprintf(key, sizeof(key), "%d", (int)want->num);

    if (n == 0) {
        sb_puts(err, "No DMD 5620 emulator is running. Start one from a shell: dmd5620 "
                     "(window) or dmd5620 --headless.");
        return false;
    }
    if (!key[0]) {
        if (n == 1) {
            *out = v[0];
            return true;
        }
        sb_puts(err, "More than one emulator runs. Give vm:");
        for (int i = 0; i < n; i++)
            sb_printf(err, " %s (pid %d)", v[i].name, v[i].pid);
        return false;
    }
    for (int i = 0; i < n; i++) {
        char pid[16];
        snprintf(pid, sizeof(pid), "%d", v[i].pid);
        if (!strcmp(v[i].name, key) || !strcmp(pid, key)) {
            *out = v[i];
            return true;
        }
    }
    sb_printf(err, "No emulator \"%s\". Use vm-list.", key);
    return false;
}

static void call_vm(const jnode *id, const char *tool, const jnode *args) {
    vm_entry vm;
    sbuf err, req, resp;
    char buf[65536];

    sb_init(&err);
    sb_init(&req);
    sb_init(&resp);

    if (!pick_vm(args, &vm, &err)) {
        tool_text(id, err.buf, true);
        goto done;
    }

    sock_t fd = sock_connect_local(vm.path);
    if (fd == SOCK_BAD) {
        sb_printf(&err, "Cannot reach emulator %s (pid %d): %s", vm.name, vm.pid,
                  sock_error());
        tool_text(id, err.buf, true);
        goto done;
    }
    sock_set_timeout(fd, 660000); /* the longest tool call can wait 10 minutes */

    sb_puts(&req, "{\"tool\":");
    sb_json_str(&req, tool);
    sb_puts(&req, ",\"args\":");
    if (args && args->type == J_OBJ)
        sb_putn(&req, args->raw, args->rawlen);
    else
        sb_puts(&req, "{}");
    sb_puts(&req, "}\n");

    sock_send_all(fd, req.buf, req.len);
    for (;;) {
        int n = sock_recv(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        sb_putn(&resp, buf, (size_t)n);
        if (memchr(buf, '\n', (size_t)n))
            break; /* the answer is one line; do not wait for the close */
    }
    sock_close(fd);

    jnode *r = resp.len ? json_parse(resp.buf, strcspn(resp.buf, "\n")) : NULL;
    if (!r) {
        sb_printf(&err, "No answer from emulator %s (pid %d).", vm.name, vm.pid);
        tool_text(id, err.buf, true);
        goto done;
    }

    bool ok = json_bool(json_get(r, "ok"), 0);
    const char *text = json_str(json_get(r, "text"), "");
    const jnode *png = json_get(r, "png");
    sbuf out;
    sb_init(&out);
    sb_puts(&out, "{\"content\":[");
    if (png && png->type == J_STR) {
        sb_puts(&out, "{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":");
        sb_putn(&out, png->raw, png->rawlen); /* base64 needs no escaping */
        sb_puts(&out, "},");
    }
    sb_puts(&out, "{\"type\":\"text\",\"text\":");
    sb_json_str(&out, text);
    sb_printf(&out, "}],\"isError\":%s}", ok ? "false" : "true");
    reply(id, out.buf);
    sb_free(&out);
    json_free(r);

done:
    sb_free(&err);
    sb_free(&req);
    sb_free(&resp);
}

static void handle(const char *line, size_t len) {
    jnode *msg = json_parse(line, len);
    if (!msg || msg->type != J_OBJ) {
        reply_error(NULL, -32700, "parse error");
        json_free(msg);
        return;
    }
    const jnode *id = json_get(msg, "id");
    const char *method = json_str(json_get(msg, "method"), "");
    const jnode *params = json_get(msg, "params");

    if (!id) {
        /* notification (initialized, cancelled, ...): nothing to answer */
    } else if (!strcmp(method, "initialize")) {
        const char *pv = json_str(json_get(params, "protocolVersion"), "2025-06-18");
        sbuf b;
        sb_init(&b);
        sb_puts(&b, "{\"protocolVersion\":");
        sb_json_str(&b, pv);
        sb_puts(&b, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                    "\"serverInfo\":{\"name\":\"dmd5620\",\"version\":\"" BRIDGE_VERSION
                    "\"},\"instructions\":");
        sb_json_str(&b, INSTRUCTIONS);
        sb_puts(&b, "}");
        reply(id, b.buf);
        sb_free(&b);
    } else if (!strcmp(method, "ping")) {
        reply(id, "{}");
    } else if (!strcmp(method, "tools/list")) {
        reply(id, TOOLS);
    } else if (!strcmp(method, "tools/call")) {
        const char *name = json_str(json_get(params, "name"), "");
        const jnode *args = json_get(params, "arguments");
        static const char *VM_TOOLS[] = {"vm-info",     "screenshot",   "key",
                                         "type-text",   "mouse-move",   "mouse-click",
                                         "mouse-button", "vm-power",    NULL};
        bool known = false;
        for (int i = 0; VM_TOOLS[i]; i++)
            known |= !strcmp(name, VM_TOOLS[i]);
        if (!strcmp(name, "vm-list"))
            vm_list(id);
        else if (known)
            call_vm(id, name, args);
        else
            reply_error(id, -32602, "unknown tool");
    } else {
        reply_error(id, -32601, "method not found");
    }
    json_free(msg);
}

/* One line from stdin, without the line end. NULL at the end. */
static char *read_line(size_t *len) {
    static char *line;
    static size_t cap;
    size_t n = 0;
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (n + 1 >= cap) {
            size_t nc = cap ? cap * 2 : 4096;
            char *nl = realloc(line, nc);
            if (!nl)
                return NULL;
            line = nl;
            cap = nc;
        }
        line[n++] = (char)c;
    }
    if (c == EOF && n == 0)
        return NULL;
    while (n > 0 && line[n - 1] == '\r')
        n--;
    if (!line) {
        line = malloc(1);
        if (!line)
            return NULL;
        cap = 1;
    }
    line[n] = 0;
    *len = n;
    return line;
}

int mcp_bridge_main(void) {
    char *line;
    size_t n;

    while ((line = read_line(&n)) != NULL) {
        if (n > 0)
            handle(line, n);
    }
    return 0;
}
