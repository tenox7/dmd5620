/*
 * This file is part of the SDL DMD 5620 Emultor.
 *
 * Copyright 2025, Seth Morabito <web@loomcom.com>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#if defined __APPLE__
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_keycode.h>

#include "ctl.h"
#include "dmd_5620.h"
#include "version.h"

#define KEYSYM(e, u, s) (((e)->keysym.mod & KMOD_SHIFT) ? (s) : (u))
#define CKEYSYM(e, u, s, c)                                                             \
    (((e)->keysym.mod & KMOD_CTRL) ? (c) : ((e)->keysym.mod & KMOD_SHIFT) ? (s) : (u))

#ifndef MIN
#define MIN(a, b) ((a) <= (b) ? (a) : (b))
#endif

#define LDBG(f, ...)                                                                    \
    do {                                                                                \
        if (debug_log)                                                                  \
            fprintf(stdout, ("[DEBUG] " f "\n")__VA_OPT__(, ) __VA_ARGS__);             \
    } while (0)
#define LERR(f, ...) fprintf(stderr, ("[ERROR] " f "\n")__VA_OPT__(, ) __VA_ARGS__)

const char *FIRMWARE_873 = "8;7;3";
const char *FIRMWARE_875 = "8;7;5";
const size_t MAX_STEPS = 2000000;
#define FRAME_MS 16

SDL_Window *sdl_window = NULL;
SDL_Renderer *sdl_renderer = NULL;
SDL_Texture *sdl_texture = NULL;

char VERSION_STRING[64];
char *nvram = NULL;
int pty_master, pty_slave;
int sigint_count = 0;
int tty_fd = -1;
pid_t shell_pid;
struct pollfd fds[2];
bool debug_log;
bool headless = false;
bool mcp_on = true;
double window_scale = 0; /* 0 = pick one that fits the screen */

volatile bool running = true;

void int_handler(int signal) {
    if (sigint_count) {
        LERR("Shutting down immediately. Good bye.");
        exit(1);
    }

    running = false;

    sigint_count++;
}

/*
 * Environment for the shell. Without --inherit the shell gets a small
 * clean environment. TERM is always "dmd", because that is what the
 * shell talks to.
 */
static char **shell_env(bool inherit, char *envp[], const char *shell) {
    static const char *keep[] = {"HOME", "USER", "LOGNAME", "PATH", "TMPDIR", "LANG", NULL};
    size_t n = 0, cap = 16;
    char **env = calloc(cap, sizeof(char *));

    if (!env)
        return envp;

    if (inherit && envp) {
        for (char **e = envp; *e; e++) {
            if (strncmp(*e, "TERM=", 5) == 0)
                continue;
            if (n + 3 >= cap) {
                cap *= 2;
                char **ne = realloc(env, cap * sizeof(char *));
                if (!ne)
                    return envp;
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

/*
 * Initialize a shell PTY
 */
void pty_init(const char *shell, char *envp[]) {
    char pty_name[64];
    /* The DMD's own terminal is 88x70 characters (terminfo "dmd"). */
    struct winsize ws = {70, 88, WIDTH, HEIGHT};

    /* Set up our PTY */
    if (openpty(&pty_master, &pty_slave, pty_name, NULL, &ws) < 0) {
        perror("Could not open terminal pty: ");
        exit(-1);
    }

    /* Fork the shell process */

    fds[0].fd = pty_master;
    fds[0].events = POLLIN;

    shell_pid = fork();

    if (shell_pid < 0) {
        perror("Could not fork child shell: ");
        exit(-1);
    } else if (shell_pid == 0) {
        /* Child */
        int retval;
        close(pty_master);

        setsid();

        if (ioctl(pty_slave, TIOCSCTTY, NULL) == -1) {
            perror("Ioctl erorr: ");
            exit(-1);
        }

        dup2(pty_slave, 0);
        dup2(pty_slave, 1);
        dup2(pty_slave, 2);
        close(pty_slave);

        retval = execle(shell, "-", NULL, envp);

        /* Child process is now replaced, nothing beyond this point
           will ever be reached unless there's an error. */
        if (retval < 0) {
            perror("Could not start shell process: ");
            exit(-1);
        }
    }

    close(pty_slave);
}

/*
 * Open and initialize a TTY device (e.g. "/dev/ttyS0", "/dev/pts/1", etc.)
 */
int tty_init(int fd) {
    struct termios tty;

    memset(&tty, 0, sizeof tty);

    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "error %d from tcgetattr", errno);
        return -1;
    }

    fds[0].fd = fd;
    fds[0].events = POLLIN;

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; /* 8-bit characters */
    tty.c_iflag &= ~IGNBRK;                     /* No break */
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);

    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "error %d from tcsetattr", errno);
        return -1;
    }

    return 0;
}

enum { OPT_HEADLESS = 1000, OPT_NAME, OPT_NO_MCP, OPT_BRIDGE, OPT_SCALE };

struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'v'},
    {"inherit", no_argument, 0, 'i'},
    {"firmware", required_argument, 0, 'f'},
    {"shell", required_argument, 0, 's'},
    {"device", required_argument, 0, 'd'},
    {"nvram", required_argument, 0, 'n'},
    {"headless", no_argument, 0, OPT_HEADLESS},
    {"name", required_argument, 0, OPT_NAME},
    {"no-mcp", no_argument, 0, OPT_NO_MCP},
    {"mcp-bridge", no_argument, 0, OPT_BRIDGE},
    {"scale", required_argument, 0, OPT_SCALE},
    {"debug", no_argument, 0, 'b'}, /* Hidden and undocumented */
    {0, 0, 0, 0}};

void usage() {
    printf("Usage: dmd5620 [-h] [-v] [-i] [-f VER] [-n FILE] [-s SHELL|-d DEV]\n");
    printf("               [--scale N] [--headless] [--name NAME] [--no-mcp]\n");
    printf("       dmd5620 --mcp-bridge\n");
    printf("AT&T DMD 5620 Terminal emulator.\n\n");
    printf("-h, --help              display help and exit\n");
    printf("-v, --version           display version and exit\n");
    printf("-i, --inherit           inherit parent environment\n");
    printf("-f, --firmware VER      Firmware version (\"8;7;3\" or \"8;7;5\")\n");
    printf("-n, --nvram FILE        store nvram state in FILE\n");
    printf("-s, --shell SHELL       execute SHELL instead of default user "
           "shell ($SHELL)\n");
    printf("-d, --device DEV        serial port name\n");
    printf("    --scale N           window size: 1 = 800x1024 (default: fit the "
           "screen)\n");
    printf("    --headless          no window (for agents; MCP still sees the "
           "screen)\n");
    printf("    --name NAME         VM name for MCP tools (default: dmd5620)\n");
    printf("    --no-mcp            do not open the MCP control socket "
           "(also: DMD_MCP=0)\n");
    printf("    --mcp-bridge        run as the MCP server (stdio) for agents\n");
}

/*
 * PTY implemntation of read and write polling
 */
void pty_io_poll() {
    uint8_t txc;
    char tx_buf[TX_BUF_LEN];
    int b_read;

    if (poll(fds, 1, 0) > 0) {
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            b_read = read(pty_master, tx_buf, TX_BUF_LEN);

            if (b_read <= 0) {
                /* The shell has exited. */
                running = false;
                return;
            }

            for (int i = 0; i < b_read; i++) {
                dmd_rs232_rx(tx_buf[i] & 0xff);
            }
        }
    }

    while (dmd_rs232_tx(&txc) == 0) {
        if (write(pty_master, &txc, 1) < 0) {
            LERR("Error %d from write: %s", errno, strerror(errno));
        }
    }
}

void tty_io_poll() {
    uint8_t txc;
    char tx_buf[TX_BUF_LEN];
    int b_read, i;

    if (poll(fds, 1, 0) > 0) {
        if (fds[0].revents & POLLIN) {

            b_read = read(tty_fd, tx_buf, TX_BUF_LEN);

            for (i = 0; i < b_read; i++) {
                dmd_rs232_rx(tx_buf[i] & 0xff);
            }
        }
    }

    while (dmd_rs232_tx(&txc) == 0) {
        if (write(tty_fd, &txc, 1) < 0) {
            fprintf(stderr, "error %d during write: %s\n", errno, strerror(errno));
        }
    }
}

/* Integer scale: crisp pixels. Otherwise smooth them. */
static void update_scale_mode() {
    int ow, oh;
    if (SDL_GetRendererOutputSize(sdl_renderer, &ow, &oh) != 0)
        return;
    double s = MIN((double)ow / WIDTH, (double)oh / HEIGHT);
    double frac = s - (double)(long)s;
    bool integer = s >= 1.0 && (frac < 0.01 || frac > 0.99);
    SDL_SetTextureScaleMode(sdl_texture, integer ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
}

/*
 * Initialize the SDL system and get the window, renderer and texture.
 */
bool sdl_init(const char *title) {
#ifdef __linux__
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");

    SDL_setenv("SDL_VIDEO_X11_WMCLASS", "com.loomcom.dmd5620", 1);
    SDL_setenv("SDL_VIDEO_WAYLAND_WMCLASS", "com.loomcom.dmd5620", 1);
#endif
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        LERR("Error initializing SDL: %s", SDL_GetError());
        return false;
    }

    /* Window size: the biggest whole scale that fits the screen. */
    double scale = window_scale;
    if (scale <= 0) {
        SDL_Rect usable;
        scale = 1;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.h > 0) {
            int room = usable.h - 40; /* title bar */
            for (int s = 3; s >= 1; s--) {
                if (HEIGHT * s <= room) {
                    scale = s;
                    break;
                }
                scale = (double)room / HEIGHT;
            }
        }
    }

    sdl_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  (int)(WIDTH * scale), (int)(HEIGHT * scale),
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                                      SDL_WINDOW_ALLOW_HIGHDPI);

    if (!sdl_window) {
        LERR("Error creating window: %s", SDL_GetError());
        return false;
    }

    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer)
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdl_renderer) {
        LERR("Error creating renderer: %s", SDL_GetError());
        return false;
    }

    /* The DMD screen is 800x1024 whatever the window size. SDL scales
     * the picture and the mouse coordinates for us. */
    SDL_RenderSetLogicalSize(sdl_renderer, WIDTH, HEIGHT);

    sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
    if (!sdl_texture) {
        LERR("Error creating texture: %s", SDL_GetError());
        return false;
    }
    update_scale_mode();

    /* The DMD draws its own mouse cursor. */
    SDL_ShowCursor(SDL_DISABLE);

    return true;
}

/*
 * TODO: Support keyboard layouts other than US English!!!
 *
 * This is really ugly at the moment... We really need a way to detect
 * keyboard layouts. Here in the US, my keyboard has "@" as "2"+"SHIFT",
 * "<" as "."+"SHIFT", ">" as ","+"SHIFT", but that will NOT be true on
 * all layouts. For example, French AZERTY layout will send "?" when you
 * press ","+"SHIFT", and number keys are always shifted! To get a "2",
 * you'd have to press "é"+"SHIFT".
 *
 * And that's just the emulator input side. I only own a US English
 * DMD5620 keyboard, so I don't even know what non-US keyboards looked
 * like for it, nor what character sets non-US terminals supported! If
 * anyone knows, please drop me a line (Seth Morabito, <web@loomcom.com>)
 *
 */
void handle_keydown_event(SDL_KeyboardEvent *event) {
    uint8_t c = 0;
    SDL_Keycode code = event->keysym.sym;

    /* Command key combinations belong to the Mac, not to the DMD.
     * Cmd-V pastes the clipboard as typed text. */
    if (event->keysym.mod & KMOD_GUI) {
        if (code == SDLK_v && SDL_HasClipboardText()) {
            char *text = SDL_GetClipboardText();
            if (text) {
                ctl_paste(text);
                SDL_free(text);
            }
        }
        return;
    }

    switch (code) {
    case SDLK_F1:
        c = 0xe8;
        break;
    case SDLK_F2:
        c = 0xe9;
        break;
    case SDLK_F3:
        c = 0xea;
        break;
    case SDLK_F4:
        c = 0xeb;
        break;
    case SDLK_F5:
        c = 0xec;
        break;
    case SDLK_F6:
        c = 0xed;
        break;
    case SDLK_F7:
        c = 0xee;
        break;
    case SDLK_F8:
        c = 0xef;
        break;
    case SDLK_F9:
        c = KEYSYM(event, 0xae, 0x8e);
        break;
    case SDLK_ESCAPE:
        c = 0x1b;
        break;
    case SDLK_DELETE:
        c = 0xfe;
        break;
    case SDLK_UP:
        c = 0xc1;
        break;
    case SDLK_DOWN:
        c = 0xc2;
        break;
    case SDLK_RIGHT:
        c = 0xc3;
        break;
    case SDLK_LEFT:
        c = 0xc4;
        break;
    case SDLK_BACKSPACE:
        c = 0xd1;
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        c = 0xe7;
        break;
    case SDLK_TAB:
        c = 0xd0;
        break;

    case SDLK_BACKQUOTE:
        c = KEYSYM(event, '`', '~');
        break;
    case SDLK_QUOTE:
        c = KEYSYM(event, '\'', '"');
        break;
    case SDLK_SLASH:
        c = KEYSYM(event, '/', '?');
        break;
    case SDLK_EQUALS:
        c = KEYSYM(event, '=', '+');
        break;
    case SDLK_MINUS:
        c = KEYSYM(event, '-', '_');
        break;
    case SDLK_LEFTBRACKET:
        c = CKEYSYM(event, '[', '{', 0x1b);
        break;
    case SDLK_BACKSLASH:
        c = CKEYSYM(event, '\\', '|', 0x1c);
        break;
    case SDLK_RIGHTBRACKET:
        c = CKEYSYM(event, ']', '}', 0x1d);
        break;
    case SDLK_SEMICOLON:
        c = KEYSYM(event, ';', ':');
        break;
    case SDLK_COMMA:
        c = KEYSYM(event, ',', '<');
        break;
    case SDLK_PERIOD:
        c = KEYSYM(event, '.', '>');
        break;
    case SDLK_SPACE:
        c = ' ';
        break;

    /* We should have handled these already on a standard US keyboard, but
     * just in case... */
    case SDLK_QUESTION:
        c = '?';
        break;
    case SDLK_PLUS:
        c = '+';
        break;
    case SDLK_UNDERSCORE:
        c = '_';
        break;
    case SDLK_COLON:
        c = ':';
        break;
    case SDLK_LESS:
        c = '<';
        break;
    case SDLK_GREATER:
        c = '>';
        break;

    /* Another US-specific config, sigh. */
    case SDLK_1:
        c = KEYSYM(event, '1', '!');
        break;
    case SDLK_2:
        c = KEYSYM(event, '2', '@');
        break;
    case SDLK_3:
        c = KEYSYM(event, '3', '#');
        break;
    case SDLK_4:
        c = KEYSYM(event, '4', '$');
        break;
    case SDLK_5:
        c = KEYSYM(event, '5', '%');
        break;
    case SDLK_6:
        c = KEYSYM(event, '6', '^');
        break;
    case SDLK_7:
        c = KEYSYM(event, '7', '&');
        break;
    case SDLK_8:
        c = KEYSYM(event, '8', '*');
        break;
    case SDLK_9:
        c = KEYSYM(event, '9', '(');
        break;
    case SDLK_0:
        c = KEYSYM(event, '0', ')');
        break;

    /* Just the alphabet */
    case SDLK_a:
    case SDLK_b:
    case SDLK_c:
    case SDLK_d:
    case SDLK_e:
    case SDLK_f:
    case SDLK_g:
    case SDLK_h:
    case SDLK_i:
    case SDLK_j:
    case SDLK_k:
    case SDLK_l:
    case SDLK_m:
    case SDLK_n:
    case SDLK_o:
    case SDLK_p:
    case SDLK_q:
    case SDLK_r:
    case SDLK_s:
    case SDLK_t:
    case SDLK_u:
    case SDLK_v:
    case SDLK_w:
    case SDLK_x:
    case SDLK_y:
    case SDLK_z:
        c = event->keysym.sym;
        c = CKEYSYM(event, c, c & 0xdf, c & 0x1f);
        break;
    default:
        return;
    }

    LDBG("[KEYDOWN] keysym.sym: 0x%02x (%c)  |  modified: 0x%02x (%c)", code,
         code >= 0x20 && code < 0x7f ? code : '.', c, c >= 0x20 && c < 0x7f ? c : '.');

    dmd_keyboard_rx(c);
}

/* Copies video RAM into the window. */
static void render() {
    uint8_t *vram = dmd_video_ram();
    uint8_t oport = 0;
    void *pixels;
    int pitch;

    if (!vram) {
        LERR("Unable to access video ram!");
        return;
    }

    dmd_get_duart_output_port(&oport);

    const struct color *fg = (oport & 0x2) ? &COLOR_DARK : &COLOR_LIGHT;
    const struct color *bg = (oport & 0x2) ? &COLOR_LIGHT : &COLOR_DARK;
    uint32_t fgc = 0xff000000u | ((uint32_t)fg->r << 16) | ((uint32_t)fg->g << 8) | fg->b;
    uint32_t bgc = 0xff000000u | ((uint32_t)bg->r << 16) | ((uint32_t)bg->g << 8) | bg->b;

    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) != 0)
        return;
    for (int y = 0; y < HEIGHT; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * (size_t)pitch);
        const uint8_t *src = vram + y * WIDTH_IN_BYTES;
        for (int x = 0; x < WIDTH_IN_BYTES; x++) {
            uint8_t b = src[x];
            for (int i = 0; i < 8; i++)
                row[x * 8 + i] = ((b >> (7 - i)) & 1) ? fgc : bgc;
        }
    }
    SDL_UnlockTexture(sdl_texture);
}

static void present() {
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}

static void handle_events(bool *need_present) {
    SDL_Event ev;

    /*
     * Some notes on event handling:
     *
     * - The DMD mouse is relative and counts Y up (see ctl.c).
     *
     * - SDL mouse buttons are numbered 1, 2, 3, whereas the DMD
     *   expects buttons to be numbered 0, 1, 2.
     *
     * - SDL_RenderSetLogicalSize() gives us mouse coordinates in DMD
     *   pixels, whatever the window size or the screen's pixel density.
     */
    while (SDL_PollEvent(&ev) != 0) {
        switch (ev.type) {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_KEYDOWN:
            handle_keydown_event(&ev.key);
            break;
        case SDL_MOUSEMOTION:
            /* The DMD mouse is relative: pass on how far it moved. */
            if (ev.motion.xrel || ev.motion.yrel)
                ctl_human_mouse(ev.motion.xrel, ev.motion.yrel);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button > 0 && ev.button.button <= 3) {
                dmd_mouse_down((uint8_t)(ev.button.button - 1));
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button > 0 && ev.button.button <= 3) {
                dmd_mouse_up((uint8_t)(ev.button.button - 1));
            }
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                update_scale_mode();
                *need_present = true;
            } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                *need_present = true;
            }
            break;
        default:
            /* Do nothing */
            break;
        }
    }
}

void sdl_main_loop() {
    uint64_t previous_ms = ctl_now_ms();
    uint64_t next_frame = previous_ms;
    bool need_present = true;

    while (running) {
        if (!headless)
            handle_events(&need_present);

        if (!running)
            break;

        uint64_t now = ctl_now_ms();

        if (mcp_on)
            ctl_service(now);

        if (!headless) {
            bool redraw = ctl_take_redraw();
            if (dmd_video_ram_dirty() || redraw) {
                render();
                need_present = true;
            }
            if (need_present) {
                present();
                need_present = false;
            }
        }

        /*
         * Poll for serial i/o
         */
        if (tty_fd < 0)
            pty_io_poll();
        else
            tty_io_poll();

        /* TODO: We need to produce a beep sound here, just like the
         * original terminal did. We also need a way to turn that off if a
         * user doesn't want it!
         */

        /* We take 13,888 simulated steps per millisecond of wall clock
         * time, based on a 7.2 MHz WE 32100 CPU.
         */
        size_t delta = (size_t)(now - previous_ms);
        size_t steps = MIN((size_t)(13888 * delta), MAX_STEPS);
        previous_ms = now;

        dmd_step_loop(steps);

        /* Wait for the next frame (about 60 per second) */
        next_frame += FRAME_MS;
        now = ctl_now_ms();
        if (next_frame > now) {
            uint64_t wait = next_frame - now;
            struct timespec ts = {0, (long)(wait * 1000000)};
            nanosleep(&ts, NULL);
        } else if (now - next_frame > 100) {
            next_frame = now; /* we fell behind; do not try to catch up */
        }

        LDBG("[MAIN LOOP] Executed %zu steps in %zu ms. rate ~= %.2f MHz", steps, delta,
             delta ? ((float)steps / (float)delta) / 1000.0f : 0.0f);
    }
}

void sdl_teardown() {
    uint8_t buf[NVRAM_SIZE];
    FILE *fp;

    if (mcp_on)
        ctl_stop();

    if (nvram && dmd_get_nvram(buf) == 0) {
        fp = fopen(nvram, "w+");
        if (fp == NULL) {
            fprintf(stderr, "Could not open %s for writing. Skipping\n", nvram);
        } else {
            if (fwrite(buf, NVRAM_SIZE, 1, fp) != 1) {
                fprintf(stderr, "Could not write full NVRAM file %s\n", nvram);
            }
            fclose(fp);
        }
    }

    if (!headless) {
        if (sdl_texture)
            SDL_DestroyTexture(sdl_texture);
        if (sdl_renderer)
            SDL_DestroyRenderer(sdl_renderer);
        if (sdl_window)
            SDL_DestroyWindow(sdl_window);
        SDL_Quit();
    }
}

int main(int argc, char **argv, char **envp) {
    int c, errflg = 0;

    char *shell = NULL;
    char *device = NULL;
    char *firmware = NULL;
    char *vm_name = NULL;
    int size;
    struct stat sb;
    char connection[300];
    uint8_t fw_version;

    uint8_t nvram_buf[NVRAM_SIZE];
    FILE *fp;

    bool inherit = false; /* Inherit parent environment */

    int option_index = 0;
    extern char *optarg;
    extern int optind, optopt;

    snprintf(VERSION_STRING, 64, "%d.%d.%d-mcp", VERSION_MAJOR, VERSION_MINOR,
             VERSION_BUILD);

    while ((c = getopt_long(argc, argv, "hivbd:n:t:p:s:f:", long_options,
                            &option_index)) != -1) {
        switch (c) {
        case 0:
            break;
        case 'h':
            usage();
            exit(0);
        case 'i':
            inherit = true;
            break;
        case 'v':
            printf("Version: %s\n", VERSION_STRING);
            exit(0);
        case 'b': /* Hidden and undocumented */
            debug_log = true;
            break;
        case 'n':
            nvram = optarg;
            break;
        case 's':
            shell = optarg;
            break;
        case 'd':
            device = optarg;
            break;
        case 'f':
            firmware = optarg;
            break;
        case OPT_HEADLESS:
            headless = true;
            break;
        case OPT_NAME:
            vm_name = optarg;
            break;
        case OPT_NO_MCP:
            mcp_on = false;
            break;
        case OPT_BRIDGE:
            return mcp_bridge_main();
        case OPT_SCALE:
            window_scale = atof(optarg);
            if (window_scale < 0.25 || window_scale > 8) {
                fprintf(stderr, "--scale must be between 0.25 and 8\n");
                return -1;
            }
            break;
        case '?':
            errflg++;
            break;
        }
    }

    if (errflg) {
        usage();
        return -1;
    }

    if (getenv("DMD_HEADLESS") && strcmp(getenv("DMD_HEADLESS"), "0"))
        headless = true;
    if (getenv("DMD_MCP"))
        mcp_on = strcmp(getenv("DMD_MCP"), "0") != 0;
    if (!vm_name && getenv("DMD_NAME"))
        vm_name = getenv("DMD_NAME");

    if (shell != NULL && device != NULL) {
        fprintf(stderr,
                "Cannot specify both --shell or --device. Only one is allowed.\n");
        return -1;
    }

    if (shell == NULL && device == NULL) {
        shell = getenv("SHELL");
        if (shell == NULL || shell[0] == 0)
            shell = "/bin/sh";
    }

    /* Initialize the CPU */
    if (firmware == NULL || strncmp(FIRMWARE_875, firmware, 5) == 0) {
        firmware = (char *)FIRMWARE_875;
        fw_version = 2;
    } else if (strncmp(FIRMWARE_873, firmware, 5) == 0) {
        firmware = (char *)FIRMWARE_873;
        fw_version = 1;
    } else {
        fprintf(stderr, "--firmware must be one of either \"%s\" or \"%s\".\n",
                FIRMWARE_873, FIRMWARE_875);
        return -1;
    }

    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);
    signal(SIGCHLD, int_handler);
    signal(SIGPIPE, SIG_IGN);

    if (device == NULL) {
        if (stat(shell, &sb) != 0 || (sb.st_mode & S_IXUSR) == 0) {
            fprintf(stderr, "Cannot open %s as shell, or file is not executable.\n",
                    shell);
            return -1;
        }

        pty_init(shell, shell_env(inherit, envp, shell));
        snprintf(connection, sizeof(connection), "shell %s", shell);
    } else {
        if (stat(device, &sb) != 0) {
            fprintf(stderr, "Cannot open device %s.\n", device);
            exit(1);
        }
        tty_fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);
        if (tty_fd < 0) {
            fprintf(stderr, "error %d opening %s: %s\n", errno, device, strerror(errno));
            return -1;
        }

        tty_init(tty_fd);
        snprintf(connection, sizeof(connection), "device %s", device);
    }

    dmd_init(fw_version);

    /* Load NVRAM, if any */
    if (nvram != NULL) {
        fp = fopen(nvram, "r");

        /* If there's no file yet, don't load anything. */
        if (fp != NULL) {
            /* Validate the file size */
            fseek(fp, 0, SEEK_END);
            size = ftell(fp);
            rewind(fp);

            if (size != NVRAM_SIZE) {
                LERR("NVRAM file %s does not seem to be valid. Skipping.", nvram);
            } else {
                if (fread(nvram_buf, NVRAM_SIZE, 1, fp) != 1) {
                    LERR("Unable to read NVRAM file %s. Skipping.", nvram);
                } else {
                    dmd_set_nvram(nvram_buf);
                }
            }
            fclose(fp);
        }
    }

    if (mcp_on) {
        ctl_info info = {vm_name, firmware, fw_version, connection, headless};
        if (ctl_start(&info) != 0)
            mcp_on = false;
    }

    if (!headless) {
        char title[96];
        snprintf(title, sizeof(title), "DMD 5620%s%s", vm_name ? " - " : "",
                 vm_name ? vm_name : "");
        if (!sdl_init(title)) {
            sdl_teardown();
            return EXIT_FAILURE;
        }
    }

    sdl_main_loop();

    sdl_teardown();

    if (shell_pid > 0)
        kill(shell_pid, SIGHUP);

    return EXIT_SUCCESS;
}
