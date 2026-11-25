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
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_keycode.h>

#include "ctl.h"
#include "dmd_5620.h"
#include "host.h"
#include "plat.h"
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
            fprintf(stdout, "[DEBUG] " f "\n", ##__VA_ARGS__);             \
    } while (0)
#define LERR(f, ...) fprintf(stderr, "[ERROR] " f "\n", ##__VA_ARGS__)

const char *FIRMWARE_873 = "8;7;3";
const char *FIRMWARE_875 = "8;7;5";
const size_t MAX_STEPS = 2000000;
#define FRAME_MS 16

SDL_Window *sdl_window = NULL;
SDL_Renderer *sdl_renderer = NULL;
SDL_Texture *sdl_texture = NULL;

char VERSION_STRING[64];
char *nvram = NULL;
int sigint_count = 0;
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

enum { OPT_HEADLESS = 1000, OPT_NAME, OPT_NO_MCP, OPT_BRIDGE, OPT_SCALE, OPT_TCP, OPT_BAUD };

struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'v'},
    {"inherit", no_argument, 0, 'i'},
    {"firmware", required_argument, 0, 'f'},
    {"shell", required_argument, 0, 's'},
    {"device", required_argument, 0, 'd'},
    {"baud", required_argument, 0, OPT_BAUD},
    {"telnet", required_argument, 0, 't'},
    {"tcp", required_argument, 0, OPT_TCP},
    {"nvram", required_argument, 0, 'n'},
    {"headless", no_argument, 0, OPT_HEADLESS},
    {"name", required_argument, 0, OPT_NAME},
    {"no-mcp", no_argument, 0, OPT_NO_MCP},
    {"mcp-bridge", no_argument, 0, OPT_BRIDGE},
    {"scale", required_argument, 0, OPT_SCALE},
    {"debug", no_argument, 0, 'b'}, /* Hidden and undocumented */
    {0, 0, 0, 0}};

void usage() {
    printf("Usage: dmd5620 [-h] [-v] [-i] [-f VER] [-n FILE] [--scale N] [--headless]\n");
    printf("               [--name NAME] [--no-mcp]\n");
    printf("               [-s SHELL | -d DEV [--baud N] | -t HOST[:PORT] | --tcp HOST:PORT]\n");
    printf("       dmd5620 --mcp-bridge\n");
    printf("AT&T DMD 5620 Terminal emulator.\n\n");
    printf("-h, --help              display help and exit\n");
    printf("-v, --version           display version and exit\n");
    printf("-f, --firmware VER      firmware version (\"8;7;3\" or \"8;7;5\")\n");
    printf("-n, --nvram FILE        store nvram state in FILE\n");
    printf("    --scale N           window size: 1 = 800x1024 (default: fit the "
           "screen)\n");
    printf("    --headless          no window (for agents; MCP still sees the "
           "screen)\n");
    printf("    --name NAME         VM name for MCP tools (default: dmd5620)\n");
    printf("    --no-mcp            do not open the MCP control socket "
           "(also: DMD_MCP=0)\n");
    printf("    --mcp-bridge        run as the MCP server (stdio) for agents\n");
    printf("\nHost connection (one of these):\n");
#ifndef _WIN32
    printf("-s, --shell SHELL       run SHELL (default: $SHELL)\n");
    printf("-i, --inherit           give the shell the whole environment\n");
#endif
    printf("-d, --device DEV        serial port (/dev/ttyS0, /dev/cu.usbserial, COM1)\n");
    printf("    --baud N            serial port speed (default 9600)\n");
    printf("-t, --telnet HOST[:PORT] telnet server (default port 23)\n");
    printf("    --tcp HOST:PORT     raw TCP connection\n");
}

/*
 * Move bytes between the host line and the DMD's RS-232 port. We read at
 * most TX_BUF_LEN bytes per frame; the DUART then hands them to the
 * firmware at the baud rate set in the DMD's Setup.
 */
static void host_io_poll(void) {
    uint8_t buf[TX_BUF_LEN], txc;
    int n = host->read(buf, TX_BUF_LEN);

    if (n < 0) {
        /* The shell exited or the connection closed. */
        running = false;
        return;
    }
    for (int i = 0; i < n; i++)
        dmd_rs232_rx(buf[i]);

    do {
        n = 0;
        while (n < TX_BUF_LEN && dmd_rs232_tx(&txc) == 0)
            buf[n++] = txc;
        if (n > 0)
            host->write(buf, n);
    } while (n == TX_BUF_LEN);
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
#ifdef SDL_HINT_VIDEODRIVER /* SDL 2.0.22 and later */
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
#endif

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

    /* Paste the clipboard as typed text: Cmd-V on the Mac; Shift-Insert
     * or Ctrl-Shift-V elsewhere. Other Cmd / Windows key combinations
     * belong to the host, not to the DMD. */
    bool paste = ((event->keysym.mod & KMOD_GUI) && code == SDLK_v) ||
                 ((event->keysym.mod & KMOD_SHIFT) && code == SDLK_INSERT) ||
                 ((event->keysym.mod & KMOD_CTRL) && (event->keysym.mod & KMOD_SHIFT) &&
                  code == SDLK_v);
    if (paste) {
        if (SDL_HasClipboardText()) {
            char *text = SDL_GetClipboardText();
            if (text) {
                ctl_paste(text);
                SDL_free(text);
            }
        }
        return;
    }
    if (event->keysym.mod & KMOD_GUI)
        return;

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
    uint64_t previous_ms = plat_ms();
    uint64_t next_frame = previous_ms;
    bool need_present = true;

    while (running) {
        if (!headless)
            handle_events(&need_present);

        if (!running)
            break;

        uint64_t now = plat_ms();

        ctl_service(now); /* MCP requests, pasted text */

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
        host_io_poll();

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
        now = plat_ms();
        if (next_frame > now) {
            plat_sleep_ms((unsigned)(next_frame - now));
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
        fp = fopen(nvram, "wb");
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

int main(int argc, char **argv) {
    int c, errflg = 0;

    char *shell = NULL;
    char *device = NULL;
    char *netaddr = NULL;
    bool telnet = false;
    int baud = 9600;
    char *firmware = NULL;
    char *vm_name = NULL;
    long size;
    char connection[300];
    uint8_t fw_version;

    uint8_t nvram_buf[NVRAM_SIZE];
    FILE *fp;

    bool inherit = false; /* Inherit parent environment */

    int option_index = 0;

    plat_init();
    snprintf(VERSION_STRING, 64, "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_BUILD);

    while ((c = getopt_long(argc, argv, "hivbd:n:t:s:f:", long_options, &option_index)) !=
           -1) {
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
        case 't':
            netaddr = optarg;
            telnet = true;
            break;
        case OPT_TCP:
            netaddr = optarg;
            telnet = false;
            break;
        case OPT_BAUD:
            baud = atoi(optarg);
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
                return 1;
            }
            break;
        case '?':
            errflg++;
            break;
        }
    }

    if (errflg || optind < argc) {
        usage();
        return 1;
    }

    if (getenv("DMD_HEADLESS") && strcmp(getenv("DMD_HEADLESS"), "0"))
        headless = true;
    if (getenv("DMD_MCP"))
        mcp_on = strcmp(getenv("DMD_MCP"), "0") != 0;
    if (!vm_name && getenv("DMD_NAME"))
        vm_name = getenv("DMD_NAME");

    if ((shell != NULL) + (device != NULL) + (netaddr != NULL) > 1) {
        fprintf(stderr, "Give only one of --shell, --device, --telnet and --tcp.\n");
        return 1;
    }

#ifdef _WIN32
    if (shell == NULL && device == NULL && netaddr == NULL) {
        fprintf(stderr, "Give a host connection: --telnet HOST[:PORT], --tcp HOST:PORT or "
                        "--device COMn.\n\n");
        usage();
        return 1;
    }
#else
    if (shell == NULL && device == NULL && netaddr == NULL)
        shell = (char *)host_default_shell();
#endif

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
        return 1;
    }

    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);
#ifdef SIGCHLD
    signal(SIGCHLD, int_handler);
#endif

    if (shell != NULL) {
        if (!host_open_shell(shell, inherit))
            return 1;
        snprintf(connection, sizeof(connection), "shell %s", shell);
    } else if (device != NULL) {
        if (!host_open_serial(device, baud))
            return 1;
        snprintf(connection, sizeof(connection), "serial %s %d baud", device, baud);
    } else {
        if (!host_open_net(netaddr, telnet))
            return 1;
        snprintf(connection, sizeof(connection), "%s %s", telnet ? "telnet" : "tcp", netaddr);
    }

    /* Emulated time comes from the instruction count: 72 ns each, the
     * rate the main loop runs the CPU at (13,888 per ms). */
    dmd_set_ns_per_step(72);
    dmd_init(fw_version);

    /* Load NVRAM, if any */
    if (nvram != NULL) {
        fp = fopen(nvram, "rb");

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

    host->close();
    host_hangup();

    return EXIT_SUCCESS;
}
