/*
 * Host line on Windows: a serial port (COM1 ...). There is no local
 * shell: Windows consoles do not speak the DMD's terminal codes. Use
 * --telnet to reach a Unix host or an emulator.
 * MIT license, same as the rest of the DMD 5620 emulator.
 */

#ifdef _WIN32

#include "host.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

const struct host_ops *host;

static HANDLE com = INVALID_HANDLE_VALUE;

static const char *win_error(void) {
    static char msg[256];
    DWORD e = GetLastError();
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
                        0, msg, sizeof(msg), NULL))
        snprintf(msg, sizeof(msg), "error %lu", (unsigned long)e);
    size_t l = strlen(msg);
    while (l && (msg[l - 1] == '\n' || msg[l - 1] == '\r' || msg[l - 1] == ' '))
        msg[--l] = 0;
    return msg;
}

static int com_read(uint8_t *buf, int max) {
    DWORD got = 0;
    if (!ReadFile(com, buf, (DWORD)max, &got, NULL))
        return 0;
    return (int)got;
}

static bool com_write(const uint8_t *buf, int n) {
    while (n > 0) {
        DWORD put = 0;
        if (!WriteFile(com, buf, (DWORD)n, &put, NULL) || put == 0) {
            fprintf(stderr, "serial: write: %s\n", win_error());
            return false;
        }
        buf += put;
        n -= (int)put;
    }
    return true;
}

static void com_close(void) {
    if (com != INVALID_HANDLE_VALUE)
        CloseHandle(com);
    com = INVALID_HANDLE_VALUE;
}

static const struct host_ops com_ops = {com_read, com_write, com_close};

bool host_open_shell(const char *shell, bool inherit) {
    (void)shell;
    (void)inherit;
    fprintf(stderr, "Windows has no shell for the DMD. Use --telnet HOST[:PORT] or "
                    "--device COMn.\n");
    return false;
}

void host_hangup(void) {
}

const char *host_default_shell(void) {
    return NULL;
}

bool host_open_serial(const char *dev, int baud) {
    char path[300];
    /* COM10 and up need the \\.\ form; it works for all. */
    if (strncmp(dev, "\\\\.\\", 4) == 0)
        snprintf(path, sizeof(path), "%s", dev);
    else
        snprintf(path, sizeof(path), "\\\\.\\%s", dev);

    com = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (com == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Cannot open %s: %s\n", dev, win_error());
        return false;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(com, &dcb);
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(com, &dcb)) {
        fprintf(stderr, "Cannot set up %s at %d baud: %s\n", dev, baud, win_error());
        com_close();
        return false;
    }

    /* Reads return at once with what is there. */
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;
    to.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(com, &to);

    host = &com_ops;
    return true;
}

#endif /* _WIN32 */
