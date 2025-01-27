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
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL2_framerate.h>
#include <SDL_keycode.h>

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

SDL_Window *sdl_window = NULL;
SDL_Surface *sdl_window_surface = NULL;
SDL_Surface *sdl_pixel_surface = NULL;

char VERSION_STRING[64];
char *nvram = NULL;
int pty_master, pty_slave;
int sigint_count = 0;
int tty_fd = -1;
Uint64 previous_ticks = 0;
pid_t shell_pid;
struct pollfd fds[2];
bool debug_log;

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
 * Initialize a shell PTY
 */
void pty_init(const char *shell, char *envp[]) {
    char pty_name[64];

    /* Set up our PTY */
    if (openpty(&pty_master, &pty_slave, pty_name, NULL, NULL) < 0) {
        perror("Could not open terminal pty: ");
        exit(-1);
    }

    /* Fork the shell process */

    fds[0].fd = pty_master;
    fds[0].events = POLLIN;
    fds[1].fd = pty_slave;
    fds[1].events = POLLOUT;

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

        if (shell) {
            retval = execle(shell, "-", NULL, envp);
        } else {
            retval = execle("/bin/sh", "-", NULL, envp);
        }

        /* Child process is now replaced, nothing beyond this point
           will ever be reached unless there's an error. */
        if (retval < 0) {
            perror("Could not start shell process: ");
            exit(-1);
        }
        close(pty_master);
    }

    close(pty_slave);
}

/*
 * Open and initialize a TTY device (e.g. "/dev/ttyS0", "/dev/pts/1", etc.)
 */
int
tty_init(int fd)
{
    struct termios tty;

    memset(&tty, 0, sizeof tty);

    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "error %d from tcgetattr", errno);
        return -1;
    }

    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[1].fd = fd;
    fds[1].events = POLLOUT;

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;  /* 8-bit characters */
    tty.c_iflag &= ~IGNBRK;                      /* No break */
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);

    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr (fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "error %d from tcsetattr", errno);
        return -1;
    }

    return 0;
}

struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'v'},
    {"inherit", no_argument, 0, 'i'},
    {"firmware", required_argument, 0, 'f'},
    {"shell", required_argument, 0, 's'},
    {"device", required_argument, 0, 'd'},
    {"nvram", required_argument, 0, 'n'},
    {"debug", no_argument, 0, 'b'}, /* Hidden and undocumented */
    {0, 0, 0, 0}};

void usage() {
    printf("Usage: dmd5620 [-h] [-v] [-i] [-f VER] [-n FILE] -s SHELL|-d DEV\n");
    printf("AT&T DMD 5620 Terminal emulator.\n\n");
    printf("-h, --help              display help and exit\n");
    printf("-v, --version           display version and exit\n");
    printf("-i, --inherit           inherit parent environment\n");
    printf("-f, --firmware VER      Firmware version (\"8;7;3\" or \"8;7;5\")\n");
    printf("-n, --nvram FILE        store nvram state in FILE\n");
    printf("-s, --shell SHELL       execute SHELL instead of default user "
           "shell\n");
    printf("-d, --device DEV        serial port name\n");
}

/*
 * PTY implemntation of read and write polling
 */
void pty_io_poll() {
    uint8_t txc;
    char tx_buf[TX_BUF_LEN];
    int b_read;

    if (poll(fds, 2, 0) > 0) {
        if (fds[0].revents & POLLIN) {
            b_read = read(pty_master, tx_buf, TX_BUF_LEN);

            if (b_read <= 0) {
                perror("Nothing to read from child: ");
                exit(-1);
            }

            for (int i = 0; i < b_read; i++) {
                dmd_rs232_rx(tx_buf[i] & 0xff);
            }
        }
    }

    while (dmd_rs232_tx(&txc) == 0) {
        if (write(pty_master, &txc, 1) < 0) {
            LERR("Error %d from write: %s\n", errno, strerror(errno));
        }
    }
}

void tty_io_poll() {
    uint8_t txc;
    char tx_buf[TX_BUF_LEN];
    int b_read, i;

    if (poll(fds, 2, 100) > 0) {
        if (fds[0].revents & POLLIN) {

            b_read = read(tty_fd, tx_buf, TX_BUF_LEN);

            for (i = 0; i < b_read; i++) {
                dmd_rs232_rx(tx_buf[i] & 0xff);
            }
        }
    }

    i = 0;
    while (dmd_rs232_tx(&txc) == 0) {
        if (write(tty_fd, &txc, 1) < 0) {
            fprintf(stderr, "error %d during write: %s\n", errno, strerror(errno));
        }
    }
}

/*
 * Initialize the SDL system and get the window, surface, and renderer.
 */
bool sdl_init() {
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");

    /* Initialize audio (for keyboard beeps), video, and event handling */
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        LERR("Error initializing SDL: %s", SDL_GetError());
        return false;
    }

    sdl_window =
        SDL_CreateWindow("DMD 5620", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                         WIDTH, HEIGHT, SDL_WINDOW_SHOWN);

    if (!sdl_window) {
        LERR("Error creating window: %s", SDL_GetError());
        return false;
    }

    sdl_window_surface = SDL_GetWindowSurface(sdl_window);

    if (!sdl_window_surface) {
        LERR("Error getting window surface: %s", SDL_GetError());
        return false;
    }

    sdl_pixel_surface =
        SDL_CreateRGBSurfaceWithFormat(0, WIDTH, HEIGHT, 32, SDL_PIXELFORMAT_RGBA8888);

    if (!sdl_pixel_surface) {
        LERR("Error getting pixel surface: %s", SDL_GetError());
        return false;
    }

    /* Record starting time */
    previous_ticks = SDL_GetTicks64();
    LDBG("[INIT] previous_ticks = %lu", previous_ticks);

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

void sdl_main_loop() {
    SDL_Event ev;
    FPSmanager fpsmanager;
    int x, y;
    uint8_t *vram;
    Uint64 ticks;
    size_t steps;

    SDL_initFramerate(&fpsmanager);
    SDL_setFramerate(&fpsmanager, 60);

    /* Clear pixels */
    SDL_FillRect(sdl_pixel_surface, NULL, 0);

    while (running) {
        /*
         * Some notes on event handling:
         *
         * - The DMD considers screen origin to be top-right, so X
         *   coordinates are backward with respect to SDL.
         *
         * - SDL mouse buttons are numbered 1, 2, 3, whereas the DMD
         *   expects buttons to be numbered 0, 1, 2.
         *
         * - TODO: Mouse movement may not be right in high-dpi screens, or
         *   screens with fractional scaling. I need to test this.
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
                if (ev.motion.x >= 0 && ev.motion.x <= WIDTH && ev.motion.y >= 0 &&
                    ev.motion.y <= HEIGHT) {
                    dmd_mouse_move((uint16_t)ev.motion.x,
                                   (uint16_t)(HEIGHT - ev.motion.y));
                }
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
            default:
                /* Do nothing */
                break;
            }
        }

        if (!running)
            break;

        if (dmd_video_ram_dirty()) {
            vram = dmd_video_ram();

            if (!vram) {
                LERR("Unable to access video ram!");
                return;
            }

            SDL_LockSurface(sdl_pixel_surface);
            {
                const struct color *fg_color;
                const struct color *bg_color;

                uint8_t *pixel_data = (uint8_t *)(sdl_pixel_surface->pixels);
                uint8_t oport = 0;
                int pixel_data_index = 0;

                dmd_get_duart_output_port(&oport);

                if (oport & 0x2) {
                    fg_color = &COLOR_DARK;
                    bg_color = &COLOR_LIGHT;
                } else {
                    fg_color = &COLOR_LIGHT;
                    bg_color = &COLOR_DARK;
                }

                for (y = 0; y < HEIGHT; y++) {
                    for (x = 0; x < WIDTH_IN_BYTES; x++) {
                        uint8_t b = vram[y * WIDTH_IN_BYTES + x];
                        for (int i = 0; i < 8; i++) {
                            int bit = (b >> (7 - i)) & 1;
                            if (bit) {
                                pixel_data[pixel_data_index++] = fg_color->a;
                                pixel_data[pixel_data_index++] = fg_color->b;
                                pixel_data[pixel_data_index++] = fg_color->g;
                                pixel_data[pixel_data_index++] = fg_color->r;
                            } else {
                                pixel_data[pixel_data_index++] = bg_color->a;
                                pixel_data[pixel_data_index++] = bg_color->b;
                                pixel_data[pixel_data_index++] = bg_color->g;
                                pixel_data[pixel_data_index++] = bg_color->r;
                            }
                        }
                    }
                }
            }
            SDL_UnlockSurface(sdl_pixel_surface);

            SDL_BlitSurface(sdl_pixel_surface, NULL, sdl_window_surface, NULL);
            SDL_UpdateWindowSurface(sdl_window);
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
        /*
        uint8_t kbc;
        if (dmd_keyboard_tx(&kbc) == 0) {
            if (kbc & 0x08) {
                // Beep here.
            }
        }
        */

        size_t delta;

        ticks = SDL_GetTicks64();

        if (ticks < previous_ticks) {
            /* Panic?? */
            LERR("We went backward in time!");
            return;
        }

        /* We take 13,888 simulated steps per millisecond of wall clock
         * time, based on a 7.2 MHz WE 32100 CPU.
         */
        delta = ticks - previous_ticks;
        steps = MIN((size_t)(13888 * delta), MAX_STEPS);
        previous_ticks = ticks;

        dmd_step_loop(steps);

        /* Wait for the next frame */
        Uint32 delay = SDL_framerateDelay(&fpsmanager);

        LDBG("[MAIN LOOP] delay=%u. Executed %lu steps in %lu ms. rate ~= %.2f MHz",
             delay, steps, delta, ((float)steps / (float)delta) / 1000.0f);
    }
}

void sdl_teardown() {
    uint8_t buf[NVRAM_SIZE];
    FILE *fp;

    if (nvram && dmd_get_nvram(buf) == 0) {
        fp = fopen(nvram, "w+");
        if (fp == NULL) {
            fprintf(stderr, "Could not open %s for writing. Skipping\n", nvram);
        } else {
            if (fwrite(buf, NVRAM_SIZE, 1, fp) != 1) {
                fprintf(stderr, "Could not write full NVRAM file %s\n", nvram);
            }
        }
    }

    if (sdl_window)
        SDL_DestroyWindow(sdl_window);

    SDL_Quit();
}

int main(int argc, char **argv, char **envp) {
    int c, errflg = 0;

    char *shell = NULL;
    char *device = NULL;
    char *firmware = NULL;
    int size;
    struct stat sb;

    uint8_t nvram_buf[NVRAM_SIZE];
    FILE *fp;

    bool inherit = false; /* Inherit parent environment */

    int option_index = 0;
    extern char *optarg;
    extern int optind, optopt;

    snprintf(VERSION_STRING, 64, "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR,
             VERSION_BUILD);

    signal(SIGINT, int_handler);
    signal(SIGCHLD, int_handler);

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
        case '?':
            fprintf(stderr, "Unrecognized option: -%c\n", optopt);
            errflg++;
            break;
        }
    }

    if (errflg) {
        usage();
        return -1;
    }

    if (shell == NULL && device == NULL) {
        fprintf(stderr, "Either --shell or --device is required.\n");
        return -1;
    }

    if (shell != NULL && device != NULL) {
        fprintf(stderr,
                "Cannot specify both --shell or --device. Only one is allowed.\n");
        return -1;
    }

    if (device == NULL) {
        if (stat(shell, &sb) != 0 || (sb.st_mode & S_IXUSR) == 0) {
            fprintf(stderr, "Cannot open %s as shell, or file is not executable.\n",
                    shell);
            return -1;
        }

        pty_init(shell, inherit ? envp : NULL);
    } else {
        if (stat(device, &sb) != 0) {
            fprintf(stderr, "Cannot open device %s.\n", device);
            exit(1);
        }
        tty_fd = open(device, O_RDWR|O_NOCTTY|O_SYNC);
        if (tty_fd < 0) {
            fprintf(stderr, "error %d opening %s: %s\n", errno, device, strerror(errno));
            return -1;
        }

        tty_init(tty_fd);
    }

    /* Initialize the CPU */
    if (firmware == NULL || strncmp(FIRMWARE_875, firmware, 5) == 0) {
        dmd_init(2);
    } else if (strncmp(FIRMWARE_873, firmware, 5) == 0) {
        dmd_init(1);
    } else {
        fprintf(stderr, "--firmware must be one of either \"%s\" or \"%s\".\n",
                FIRMWARE_873, FIRMWARE_875);
        return -1;
    }

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
        }
    }

    if (!sdl_init()) {
        return EXIT_FAILURE;
    }

    sdl_main_loop();

    sdl_teardown();

    return EXIT_SUCCESS;
}
