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

#include <getopt.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#include "dmd_5620.h"
#include "version.h"

const char *FIRMWARE_873 = "8;7;3";
const char *FIRMWARE_875 = "8;7;5";

SDL_Surface *surface = NULL;
SDL_Window *main_window = NULL;
char VERSION_STRING[64];
int pty_master, pty_slave;
int sigint_count = 0;
pid_t shell_pid;
struct pollfd fds[2];

volatile bool running = true;

void int_handler(int signal) {
    if (sigint_count) {
        fprintf(stderr, "Shutting down immediately. Good bye.\n");
        exit(1);
    }

    if (main_window != NULL) {
        SDL_DestroyWindow(main_window);
        SDL_Quit();
        running = false;
    }

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
    printf(
        "Usage: dmd5620 [-h] [-v] [-i] [-f VER] [-n FILE] -s SHELL|-d DEV\n");
    printf("AT&T DMD 5620 Terminal emulator.\n\n");
    printf("-h, --help              display help and exit\n");
    printf("-v, --version           display version and exit\n");
    printf("-i, --inherit           inherit parent environment\n");
    printf(
        "-f, --firmware VER      Firmware version (\"8;7;3\" or \"8;7;5\")\n");
    printf("-n, --nvram FILE        store nvram state in FILE\n");
    printf("-s, --shell SHELL       execute SHELL instead of default user "
           "shell\n");
    printf("-d, --device DEV        serial port name\n");
}

bool sdl_init() {
    if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
        fprintf(stderr, "Error initializing SDL: %s\n", SDL_GetError());
        return false;
    }

    main_window = SDL_CreateWindow("DMD 5620", SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED, WIDTH, HEIGHT,
                                   SDL_WINDOW_SHOWN);

    if (!main_window) {
        fprintf(stderr, "Error creating window: %s\n", SDL_GetError());
        return false;
    }

    surface = SDL_GetWindowSurface(main_window);

    if (!surface) {
        fprintf(stderr, "Error getting surface: %s\n", SDL_GetError());
        return false;
    }

    SDL_FillRect(surface, NULL, SDL_MapRGB(surface->format, COLOR_LIGHT.r, COLOR_LIGHT.g, COLOR_LIGHT.b));
    SDL_UpdateWindowSurface(main_window);

    return true;
}

void sdl_main_loop() {
    SDL_Event ev;

    while (running) {
        while (SDL_PollEvent(&ev) != 0) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                printf("keydown: %d\n", ev.key.keysym.sym);
                break;
            case SDL_KEYUP:
                printf("keyup: %d\n", ev.key.keysym.sym);
                break;
            }
        }

        if (running) {
            /* TODO: Main logic here */
            SDL_Delay(33);
        }
    }

    SDL_DestroyWindow(main_window);
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
            /* TODO: Implement Debug flag */
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
        fprintf(
            stderr,
            "Cannot specify both --shell or --device. Only one is allowed.\n");
        return -1;
    }

    if (device == NULL) {
        if (stat(shell, &sb) != 0 || (sb.st_mode & S_IXUSR) == 0) {
            fprintf(stderr,
                    "Cannot open %s as shell, or file is not executable.\n",
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
                fprintf(stderr,
                        "NVRAM file %s does not seem to be valid. Skipping.\n",
                        nvram);
            } else {
                if (fread(nvram_buf, NVRAM_SIZE, 1, fp) != 1) {
                    fprintf(stderr, "Unable to read NVRAM file %s. Skipping.\n",
                            nvram);
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

    return EXIT_SUCCESS;
}
