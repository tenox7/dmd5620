/*
 * Small platform layer. See plat.h.
 * MIT license, same as the rest of the DMD 5620 emulator.
 */

#include "plat.h"

#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <afunix.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

uint64_t plat_ms(void) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GetTicks64();
#else
    return SDL_GetTicks(); /* wraps after 49 days */
#endif
}

void plat_sleep_ms(unsigned ms) {
    SDL_Delay(ms);
}

/* ------------------------------------------------------------------ */
#ifdef _WIN32

void plat_init(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    /* A GUI program has no console. Use the one of the shell that
     * started us, so --help and errors show. Pipes (the MCP bridge)
     * are already valid handles and stay as they are. */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE || !err || err == INVALID_HANDLE_VALUE) &&
        AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!out || out == INVALID_HANDLE_VALUE)
            freopen("CONOUT$", "w", stdout);
        if (!err || err == INVALID_HANDLE_VALUE)
            freopen("CONOUT$", "w", stderr);
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

int plat_pid(void) {
    return (int)GetCurrentProcessId();
}

bool plat_pid_alive(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED; /* exists, not ours */
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

bool plat_sock_dir(char *buf, size_t n, bool make) {
    /* %TEMP% is per user and private to the user. */
    char tmp[MAX_PATH];
    DWORD len = GetTempPathA(sizeof(tmp), tmp);
    if (len == 0 || len >= sizeof(tmp))
        snprintf(tmp, sizeof(tmp), "C:\\Windows\\Temp\\");
    if (len > 0 && tmp[len - 1] == '\\')
        tmp[len - 1] = 0;
    snprintf(buf, n, "%s\\dmd5620", tmp);
    if (make && _mkdir(buf) < 0 && errno != EEXIST) {
        fprintf(stderr, "mcp: cannot make %s: %s\n", buf, strerror(errno));
        return false;
    }
    return true;
}

void plat_remove(const char *path) {
    DeleteFileA(path);
}

const char *sock_error(void) {
    static char msg[256];
    int e = WSAGetLastError();
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                        (DWORD)e, 0, msg, sizeof(msg), NULL))
        snprintf(msg, sizeof(msg), "socket error %d", e);
    size_t l = strlen(msg);
    while (l && (msg[l - 1] == '\n' || msg[l - 1] == '\r' || msg[l - 1] == ' '))
        msg[--l] = 0;
    return msg;
}

void sock_close(sock_t s) {
    shutdown(s, SD_BOTH);
    closesocket(s);
}

void sock_set_timeout(sock_t s, int ms) {
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
}

int sock_recv(sock_t s, void *buf, size_t n) {
    return recv(s, buf, (int)n, 0);
}

int sock_send(sock_t s, const void *buf, size_t n) {
    return send(s, buf, (int)n, 0);
}

#else /* POSIX */

void plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
}

int plat_pid(void) {
    return (int)getpid();
}

bool plat_pid_alive(int pid) {
    return !(kill(pid, 0) < 0 && errno == ESRCH);
}

bool plat_sock_dir(char *buf, size_t n, bool make) {
    struct stat st;
    snprintf(buf, n, "/tmp/dmd5620-%u", (unsigned)getuid());
    if (!make)
        return true;
    if (mkdir(buf, 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "mcp: cannot make %s: %s\n", buf, strerror(errno));
        return false;
    }
    if (lstat(buf, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        fprintf(stderr, "mcp: %s is not a folder of this user; MCP is off\n", buf);
        return false;
    }
    chmod(buf, 0700);
    return true;
}

void plat_remove(const char *path) {
    unlink(path);
}

const char *sock_error(void) {
    return strerror(errno);
}

void sock_close(sock_t s) {
    shutdown(s, SHUT_RDWR);
    close(s);
}

void sock_set_timeout(sock_t s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int sock_recv(sock_t s, void *buf, size_t n) {
    return (int)recv(s, buf, n, 0);
}

int sock_send(sock_t s, const void *buf, size_t n) {
#ifdef MSG_NOSIGNAL
    return (int)send(s, buf, n, MSG_NOSIGNAL);
#else
    return (int)send(s, buf, n, 0);
#endif
}

#endif

/* ------------------------------------------------------------------ */
/* Shared socket code                                                  */

static void no_sigpipe(sock_t s) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)s;
#endif
}

static bool local_addr(const char *path, struct sockaddr_un *sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa->sun_path))
        return false;
    strcpy(sa->sun_path, path);
    return true;
}

sock_t sock_listen_local(const char *path) {
    struct sockaddr_un sa;
    if (!local_addr(path, &sa)) {
        fprintf(stderr, "mcp: socket path too long: %s\n", path);
        return SOCK_BAD;
    }
    sock_t s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == SOCK_BAD) {
        fprintf(stderr, "mcp: socket: %s\n", sock_error());
        return SOCK_BAD;
    }
    plat_remove(path);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(s, 4) < 0) {
        fprintf(stderr, "mcp: cannot listen on %s: %s\n", path, sock_error());
        sock_close(s);
        return SOCK_BAD;
    }
#ifndef _WIN32
    chmod(path, 0600);
#endif
    return s;
}

sock_t sock_connect_local(const char *path) {
    struct sockaddr_un sa;
    if (!local_addr(path, &sa))
        return SOCK_BAD;
    sock_t s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == SOCK_BAD)
        return SOCK_BAD;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        sock_close(s);
        return SOCK_BAD;
    }
    no_sigpipe(s);
    return s;
}

sock_t sock_accept(sock_t s) {
    sock_t c = accept(s, NULL, NULL);
    if (c != SOCK_BAD)
        no_sigpipe(c);
    return c;
}

sock_t sock_connect_tcp(const char *host, const char *port, char *err, size_t errlen) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "cannot find %s: %s", host, gai_strerror(rc));
        return SOCK_BAD;
    }
    sock_t s = SOCK_BAD;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_BAD)
            continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        snprintf(err, errlen, "cannot connect to %s port %s: %s", host, port, sock_error());
        sock_close(s);
        s = SOCK_BAD;
    }
    freeaddrinfo(res);
    if (s != SOCK_BAD) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        no_sigpipe(s);
    }
    return s;
}

bool sock_readable(sock_t s) {
    fd_set rd;
    struct timeval tv = {0, 0};
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    return select((int)s + 1, &rd, NULL, NULL, &tv) > 0;
}

bool sock_send_all(sock_t s, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        int k = sock_send(s, p, n);
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}
