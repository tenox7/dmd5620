/*
 * Small platform layer: time, processes, local sockets, folders.
 * One implementation for POSIX (macOS, Linux, BSD) and one for Windows.
 * MIT license, same as the rest of the DMD 5620 emulator.
 */

#ifndef PLAT_H
#define PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#else
typedef int sock_t;
#define SOCK_BAD (-1)
#endif

#ifdef _WIN32
#define PLAT_SEP "\\"
#else
#define PLAT_SEP "/"
#endif

/* Call once at start: sockets (Windows), console for --help (Windows). */
void plat_init(void);

uint64_t plat_ms(void); /* monotonic milliseconds */
void plat_sleep_ms(unsigned ms);
int plat_pid(void);
bool plat_pid_alive(int pid);

/* Private per-user folder for the control sockets. Made if missing.
 * Returns false (with a message on stderr) if it is not safe to use. */
bool plat_sock_dir(char *buf, size_t n, bool make);

/* Local (AF_UNIX) stream sockets. Windows 10 1803 and later have them. */
sock_t sock_listen_local(const char *path);
sock_t sock_connect_local(const char *path);
/* TCP client to host:port. Blocking connect. */
sock_t sock_connect_tcp(const char *host, const char *port, char *err, size_t errlen);
sock_t sock_accept(sock_t s);
int sock_recv(sock_t s, void *buf, size_t n); /* -1 error, 0 closed */
int sock_send(sock_t s, const void *buf, size_t n);
bool sock_send_all(sock_t s, const void *buf, size_t n);
void sock_close(sock_t s);
void sock_set_timeout(sock_t s, int ms); /* receive timeout */
bool sock_readable(sock_t s); /* data or end of stream waits; does not block */
const char *sock_error(void);
void plat_remove(const char *path);

#endif
