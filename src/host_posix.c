/*
 * Host line on POSIX systems: a shell on a pseudo terminal, or a serial
 * port with termios.
 *
 * Copyright 2025, Seth Morabito <web@loomcom.com>
 * MIT license, same as the rest of the DMD 5620 emulator.
 */

#ifndef _WIN32

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <pty.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

extern char **environ;

const struct host_ops *host;

static int line_fd = -1;
static pid_t shell_pid;

static int fd_read(uint8_t *buf, int max) {
    struct pollfd p = {line_fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0)
        return 0;
    if (!(p.revents & (POLLIN | POLLHUP | POLLERR)))
        return 0;
    ssize_t n = read(line_fd, buf, (size_t)max);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    return n <= 0 ? -1 : (int)n;
}

static bool fd_write(const uint8_t *buf, int n) {
    while (n > 0) {
        ssize_t k = write(line_fd, buf, (size_t)n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            fprintf(stderr, "host line: write: %s\n", strerror(errno));
            return false;
        }
        buf += k;
        n -= (int)k;
    }
    return true;
}

static void fd_close(void) {
    if (line_fd >= 0)
        close(line_fd);
    line_fd = -1;
}

/* A serial port read that gives nothing is not the end of the line. */
static int serial_read(uint8_t *buf, int max) {
    int n = fd_read(buf, max);
    return n < 0 ? 0 : n;
}

static const struct host_ops pty_ops = {fd_read, fd_write, fd_close};
static const struct host_ops serial_ops = {serial_read, fd_write, fd_close};

/* The folder this program is in, without the last '/'. */
static bool exe_dir(char *buf, size_t size) {
    char path[4096];

#if defined(__APPLE__)
    uint32_t n = sizeof(path);
    if (_NSGetExecutablePath(path, &n) != 0)
        return false;
#elif defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t n = sizeof(path);
    if (sysctl(mib, 4, path, &n, NULL, 0) != 0)
        return false;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0)
        return false;
    path[n] = '\0';
#endif
    if (!realpath(path, buf) || strlen(buf) >= size)
        return false;
    char *slash = strrchr(buf, '/');
    if (!slash)
        return false;
    *slash = '\0';
    return true;
}

/* mkdir DIR 0700, or use it if it is already ours, private and not a link. */
static bool private_dir(const char *dir) {
    struct stat sb;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    return lstat(dir, &sb) == 0 && S_ISDIR(sb.st_mode) && sb.st_uid == getuid() &&
           (sb.st_mode & 077) == 0;
}

/*
 * layers, the AT&T window system host program, comes with the emulator
 * in "layers" next to the program (DMD5620.app: Contents/Resources/layers;
 * make install: PREFIX/lib/dmd5620/layers). If it is there, the shell
 * gets its bin on PATH, DMDSYS (where layers finds its helper programs)
 * and DMD, a private folder for the pipes that layers makes in DMD/xt.
 * A DMD or DMDSYS that the user set stays as it is.
 */
static void layers_env(void) {
    static const char *where[] = {"layers", "../Resources/layers", "../lib/dmd5620/layers", NULL};
    char dir[4096], lyr[4096], buf[4096];

    if (!exe_dir(dir, sizeof(dir)))
        return;
    for (int i = 0;; i++) {
        if (!where[i])
            return;
        if (snprintf(lyr, sizeof(lyr), "%s/%s", dir, where[i]) >= (int)sizeof(lyr) ||
            snprintf(buf, sizeof(buf), "%s/bin/layers", lyr) >= (int)sizeof(buf))
            return;
        if (access(buf, X_OK) == 0 && realpath(lyr, buf))
            break;
    }
    memcpy(lyr, buf, sizeof(lyr));

    const char *path = getenv("PATH");
    if (snprintf(buf, sizeof(buf), "%s/bin", lyr) >= (int)sizeof(buf))
        return;
    if (!path || !*path) {
        setenv("PATH", buf, 1);
    } else if (!strstr(path, buf)) {
        char *np = malloc(strlen(path) + strlen(buf) + 2);
        if (np) {
            sprintf(np, "%s:%s", path, buf);
            setenv("PATH", np, 1);
            free(np);
        }
    }
    if (!getenv("DMDSYS"))
        setenv("DMDSYS", lyr, 1);
    if (!getenv("DMD")) {
        const char *base = getenv("XDG_RUNTIME_DIR");
        if (!base || !*base)
            base = getenv("TMPDIR");
        if (!base || !*base)
            base = "/tmp";
        if (snprintf(dir, sizeof(dir), "%s/dmd5620-%u", base, (unsigned)getuid()) >=
                (int)sizeof(dir) ||
            snprintf(buf, sizeof(buf), "%s/xt", dir) >= (int)sizeof(buf))
            return;
        if (private_dir(dir) && private_dir(buf))
            setenv("DMD", dir, 1);
    }
}

/*
 * Environment for the shell. Without --inherit the shell gets a small
 * clean environment. TERM is always "dmd", because that is what the
 * shell talks to.
 */
static char **shell_env(bool inherit, const char *shell) {
    static const char *keep[] = {"HOME", "USER", "LOGNAME", "PATH", "TMPDIR", "LANG",
                                 "DMD", "DMDSYS", NULL};
    size_t n = 0, cap = 16;
    char **env = calloc(cap, sizeof(char *));

    if (!env)
        return environ;
    layers_env();

    if (inherit && environ) {
        for (char **e = environ; *e; e++) {
            if (strncmp(*e, "TERM=", 5) == 0)
                continue;
            if (n + 3 >= cap) {
                cap *= 2;
                char **ne = realloc(env, cap * sizeof(char *));
                if (!ne)
                    return environ;
                env = ne;
            }
            env[n++] = *e;
        }
    } else {
        for (int i = 0; keep[i]; i++) {
            const char *v = getenv(keep[i]);
            if (v) {
                size_t len = strlen(keep[i]) + strlen(v) + 2;
                char *s = malloc(len);
                if (s) {
                    snprintf(s, len, "%s=%s", keep[i], v);
                    env[n++] = s;
                }
            }
        }
        size_t len = strlen(shell) + 7;
        char *s = malloc(len);
        if (s) {
            snprintf(s, len, "SHELL=%s", shell);
            env[n++] = s;
        }
    }
    env[n++] = "TERM=dmd";
    env[n] = NULL;
    return env;
}

const char *host_default_shell(void) {
    const char *sh = getenv("SHELL");
    if (sh && *sh)
        return sh;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && *pw->pw_shell)
        return pw->pw_shell;
    return "/bin/sh";
}

bool host_open_shell(const char *shell, bool inherit) {
    struct stat sb;
    int master, slave;
    /* The DMD's own terminal is 88x70 characters (terminfo "dmd"). */
    struct winsize ws = {70, 88, 800, 1024};

    if (stat(shell, &sb) != 0 || (sb.st_mode & S_IXUSR) == 0) {
        fprintf(stderr, "Cannot open %s as shell, or file is not executable.\n", shell);
        return false;
    }
    char **envp = shell_env(inherit, shell);

    if (openpty(&master, &slave, NULL, NULL, &ws) < 0) {
        perror("Could not open terminal pty");
        return false;
    }

    shell_pid = fork();
    if (shell_pid < 0) {
        perror("Could not fork child shell");
        return false;
    }
    if (shell_pid == 0) {
        /* Child */
        close(master);
        setsid();
        if (ioctl(slave, TIOCSCTTY, NULL) == -1) {
            perror("ioctl TIOCSCTTY");
            _exit(127);
        }
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)
            close(slave);
        signal(SIGPIPE, SIG_DFL);
        /* Started from a desktop (Finder, a menu): begin at home. */
        char cwd[8];
        const char *home = getenv("HOME");
        if (home && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0 && chdir(home) != 0)
            perror(home); /* the shell starts in / then */
        execle(shell, "-", (char *)NULL, envp);
        perror("Could not start shell process");
        _exit(127);
    }

    close(slave);
    line_fd = master;
    host = &pty_ops;
    return true;
}

void host_hangup(void) {
    if (shell_pid > 0) {
        kill(shell_pid, SIGHUP);
        waitpid(shell_pid, NULL, WNOHANG);
    }
}

static speed_t baud_code(int baud) {
    switch (baud) {
    case 300: return B300;
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
#ifdef B57600
    case 57600: return B57600;
#endif
#ifdef B115200
    case 115200: return B115200;
#endif
    }
    return 0;
}

bool host_open_serial(const char *dev, int baud) {
    struct termios tty;
    speed_t sp = baud_code(baud);

    if (!sp) {
        fprintf(stderr, "Baud rate %d is not supported.\n", baud);
        return false;
    }
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", dev, strerror(errno));
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    memset(&tty, 0, sizeof tty);
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "%s is not a serial port: %s\n", dev, strerror(errno));
        close(fd);
        return false;
    }

    cfsetospeed(&tty, sp);
    cfsetispeed(&tty, sp);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; /* 8-bit characters */
    tty.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | IGNCR | ISTRIP | PARMRK);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "Cannot set up %s: %s\n", dev, strerror(errno));
        close(fd);
        return false;
    }

    line_fd = fd;
    host = &serial_ops;
    return true;
}

#endif /* !_WIN32 */
