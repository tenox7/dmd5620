/*
 * Control socket: lets the MCP bridge (dmd5620 --mcp-bridge) drive a
 * running emulator: screenshots, keyboard, mouse.
 * MIT license, same as the rest of the SDL DMD 5620 emulator.
 */

#ifndef __CTL_H__
#define __CTL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;       /* VM name for the tools; NULL = "dmd5620" */
    const char *firmware;   /* "8;7;3" or "8;7;5" */
    uint8_t fw_version;     /* 1 or 2, for dmd_init() on reset */
    const char *connection; /* e.g. "shell /bin/zsh" */
    bool headless;
} ctl_info;

/* Emulator side. ctl_service() and ctl_note_*() run on the main thread. */
int ctl_start(const ctl_info *info);
void ctl_service(uint64_t now_ms);
void ctl_stop(void);
bool ctl_take_redraw(void);
void ctl_human_mouse(int x, int y);       /* host pointer at x,y (DMD pixels) */
void ctl_mouse_firmware(uint8_t fw_version); /* 1 = 8;7;3, 2 = 8;7;5 */

/* Queue text typed by the user (e.g. Cmd-V paste). Main thread only. */
void ctl_paste(const char *text);

/* Running emulators, from the socket folder. Removes stale sockets. */
typedef struct {
    char name[64];
    int pid;
    char path[400];
} vm_entry;
int ctl_list_vms(vm_entry *out, int max);

/* Bridge side (mcp.c) */
int mcp_bridge_main(void);

uint64_t ctl_now_ms(void);

#endif
