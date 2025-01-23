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
const size_t MAX_STEPS = 350000;

SDL_Window *sdl_window = NULL;
SDL_Surface *sdl_window_surface = NULL;
SDL_Surface *sdl_pixel_surface = NULL;

char VERSION_STRING[64];
int pty_master, pty_slave;
int sigint_count = 0;
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
    int b_read, i;

    if (poll(fds, 2, 0) > 0) {
        if (fds[0].revents & POLLIN) {
            b_read = read(pty_master, tx_buf, TX_BUF_LEN);

            if (b_read <= 0) {
                perror("Nothing to read from child: ");
                exit(-1);
            }

            for (i = 0; i < b_read; i++) {
                dmd_rs232_rx(tx_buf[i] & 0xff);
            }
        }
    }

    i = 0;
    while (dmd_rs232_tx(&txc) == 0) {
        if (write(pty_master, &txc, 1) < 0) {
            LERR("Error %d from write: %s\n", errno, strerror(errno));
        }
    }
}

/*
 * Initialize the SDL system and get the window, surface, and renderer.
 */
bool sdl_init() {
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
    SDL_Keycode modified = SDL_GetKeyFromScancode(event->keysym.scancode);

    LDBG("[KEYDOWN] keysym.sym: 0x%02x (%c)  |  modified: 0x%02x (%c)", code,
         code >= 0x20 && code < 0x7f ? code : '.', modified,
         modified >= 0x20 && modified < 0x7f ? modified : '.');

    switch (code) {
    case SDLK_BACKSPACE:
        c = 0xd1;
        break;
    case SDLK_RETURN:
        c = 0xe7;
        break;
    case SDLK_TAB:
        c = 0xd0;
        break;
    case SDLK_SPACE:
    case SDLK_EXCLAIM:
    case SDLK_QUOTEDBL:
    case SDLK_HASH:
    case SDLK_DOLLAR:
    case SDLK_PERCENT:
    case SDLK_AMPERSAND:
    case SDLK_QUOTE:
    case SDLK_LEFTPAREN:
    case SDLK_RIGHTPAREN:
    case SDLK_ASTERISK:
    case SDLK_PLUS:
    case SDLK_COMMA:
    case SDLK_MINUS:
    case SDLK_PERIOD:
    case SDLK_SLASH:
    case SDLK_COLON:
    case SDLK_SEMICOLON:
    case SDLK_LESS:
    case SDLK_GREATER:
    case SDLK_QUESTION:
    case SDLK_LEFTBRACKET:
    case SDLK_KP_VERTICALBAR:
    case SDLK_RIGHTBRACKET:
    case SDLK_EQUALS:
        c = (uint8_t)(event->keysym.sym & 0xff);
        break;

    case SDLK_1:
    case SDLK_2:
    case SDLK_3:
    case SDLK_4:
    case SDLK_5:
    case SDLK_6:
    case SDLK_7:
    case SDLK_8:
    case SDLK_9:
    case SDLK_0:
        c = (uint8_t)(event->keysym.sym & 0xff);
        if (event->keysym.mod & KMOD_SHIFT) {
            c = c - 0x10;
        }
        break;

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
        c = (uint8_t)(event->keysym.sym & 0xff);
        if (event->keysym.mod & KMOD_CTRL) {
            c = c & 0x0f;
        } else if (event->keysym.mod & KMOD_SHIFT) {
            c = c & 0xdf;
        }
        break;
    default:
        return;
    }

    LDBG("KEY PRESS: 0x%02x (%c)", c, c >= 0x20 && c < 0x7f ? c : '.');

    dmd_keyboard_rx(c);
}

void sdl_main_loop() {
    SDL_Event ev;
    FPSmanager fpsmanager;
    int x, y;
    uint8_t *vram;
    uint8_t kbc;
    Uint64 ticks;
    size_t steps;

    SDL_initFramerate(&fpsmanager);
    SDL_setFramerate(&fpsmanager, 30);

    /* Clear pixels */
    SDL_FillRect(sdl_pixel_surface, NULL, 0);

    while (running) {
        while (SDL_PollEvent(&ev) != 0) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                handle_keydown_event(&ev.key);
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
        }

        SDL_UnlockSurface(sdl_pixel_surface);
        SDL_BlitSurface(sdl_pixel_surface, NULL, sdl_window_surface, NULL);
        SDL_UpdateWindowSurface(sdl_window);

        /* Execute the simulator */
        pty_io_poll();

        /*
         * Poll for output to the keyboard (i.e. system beep)
         */
        if (dmd_keyboard_tx(&kbc) == 0) {
            if (kbc & 0x08) {
                LDBG("BEEP!!");
            }
        }

        size_t delta;

        ticks = SDL_GetTicks64();

        if (ticks < previous_ticks) {
            /* Panic?? */
            LERR("We went backward in time!");
            return;
        }

        delta = ticks - previous_ticks;
        steps = MIN((size_t)(7.2 * delta), MAX_STEPS);

        LDBG("[MAIN LOOP] executing %lu steps in %lu us. rate ~= %.2f MHz", steps, delta,
             (float)steps / (float)delta);

        dmd_step_loop(steps);

        /* Wait for the next frame */
        SDL_framerateDelay(&fpsmanager);
    }
}

void sdl_teardown() {
    if (sdl_window)
        SDL_DestroyWindow(sdl_window);
    SDL_Quit();
}

int main(int argc, char **argv, char **envp) {
    int c, errflg = 0;

    char *nvram = NULL;
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
        /* TODO */
        fprintf(stderr, "[TODO] Device support not implemented yet.\n");
        return -1;
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
