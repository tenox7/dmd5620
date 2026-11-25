/*
 * The host line: what the DMD's RS-232 port is connected to.
 *   shell   a local shell on a pseudo terminal (POSIX only)
 *   serial  a real serial port (/dev/tty..., COM1)
 *   telnet  a telnet server (for example a 3B2 emulator console)
 *   tcp     a raw TCP stream
 * MIT license, same as the rest of the DMD 5620 emulator.
 */

#ifndef HOST_H
#define HOST_H

#include <stdbool.h>
#include <stdint.h>

struct host_ops {
    int (*read)(uint8_t *buf, int max); /* bytes; 0 = none now; -1 = line closed */
    bool (*write)(const uint8_t *buf, int n);
    void (*close)(void);
};
extern const struct host_ops *host;

/* Each returns false and prints why on stderr. */
bool host_open_shell(const char *shell, bool inherit);
bool host_open_serial(const char *dev, int baud);
bool host_open_net(const char *hostport, bool telnet);

/* POSIX: $SHELL, else the user's login shell, else /bin/sh. */
const char *host_default_shell(void);

/* POSIX: the shell's process, to hang it up at exit. */
void host_hangup(void);

#endif
