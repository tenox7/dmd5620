# DMD 5620 emulator

An emulator of the AT&T / Teletype DMD 5620, the commercial version of
the Bell Labs Blit: a bitmap terminal with an 800x1024 one-bit portrait
screen, a WE 32100 CPU, a keyboard and a three-button mouse. It runs the
original firmware, versions 8;7;3 and 8;7;5.

It runs on macOS, Linux, FreeBSD and Windows. It needs only SDL2.

It also has a built-in [MCP](https://modelcontextprotocol.io/) server, so
an AI agent can look at the screen and use the keyboard and the mouse.
See [MCP.md](MCP.md).

This is a fork of Seth Morabito's
[dmd_sdl](https://git.loomcom.com/seth/dmd_sdl). His emulator core,
[dmd_core](https://git.loomcom.com/seth/dmd_core), is written in Rust.
Here it is ported to plain C with the same C API (folder `core/`). A
test runs both cores side by side and compares them after every
instruction (see [Testing](#testing)). The C core is faster, and it has
one fix, so firmware 8;7;3 works too.

## Download

Ready binaries are on the [Releases](../../releases) page:

| File | For |
|---|---|
| `dmd5620-VERSION-macos.zip` | macOS 10.13 and later, Apple silicon and Intel |
| `dmd5620-VERSION-linux-x86_64.tar.gz` | Linux on x86_64 (needs the SDL2 package) |
| `dmd5620-VERSION-linux-arm64.tar.gz` | Linux on arm64 (needs the SDL2 package) |
| `dmd5620-VERSION-freebsd-amd64.tar.gz` | FreeBSD 14 on amd64 (needs `pkg install sdl2`) |
| `dmd5620-VERSION-windows-x64.zip` | Windows 10 and later, x64 (SDL2 is built in) |

## Use

```
dmd5620 [-f VER] [-n FILE] [--scale N] [--headless] [--name NAME] [--no-mcp]
        [-s SHELL | -d DEV [--baud N] | -t HOST[:PORT] | --tcp HOST:PORT]
```

The terminal must talk to a host. Give one of these:

| Option | Host |
|---|---|
| `-s SHELL`, `--shell` | A local shell on a pseudo terminal. This is the default, with your login shell. Not on Windows. |
| `-d DEV`, `--device` | A serial port: `/dev/ttyUSB0`, `/dev/cu.usbserial-1`, `COM3`. `--baud N` sets the speed (default 9600). |
| `-t HOST[:PORT]`, `--telnet` | A telnet server. The default port is 23. |
| `--tcp HOST:PORT` | A raw TCP connection, with no telnet protocol. |

Other options:

| Option | What it does |
|---|---|
| `-f VER`, `--firmware` | Firmware `"8;7;3"` or `"8;7;5"` (default) |
| `-n FILE`, `--nvram` | Keep the terminal's settings (its NVRAM) in FILE |
| `-i`, `--inherit` | Give the shell your whole environment, not a small clean one |
| `--scale N` | Window size; 1 is 800x1024. Default: the biggest size that fits the screen |
| `--headless` | No window. MCP still sees the screen |
| `--name NAME` | Name of this terminal in the MCP tools |
| `--no-mcp` | No MCP control socket |

Examples:

```
dmd5620 -n ~/.dmd5620_nvram
dmd5620 -f "8;7;3" -d /dev/ttyS0 --baud 19200
dmd5620 -t localhost:9100
```

### Connect to a 3B2

The [OpenSIMH 3B2 emulator](https://loomcom.com/3b2/emulator/) gives its
serial lines as telnet ports. In the 3B2 emulator:

```
ATTACH CONTTY 9100
```

Then connect the DMD to it:

```
dmd5620 -t localhost:9100
```

A real 3B2 or other Unix host can use a serial port (`-d`), or telnet
(`-t`). The emulator tells a telnet server that its terminal type is `DMD`
and its size is 88x70.

### Keys

| Key | DMD key |
|---|---|
| F1 to F8 | F1 to F8 |
| F9 | Setup |
| Shift+F9 | Reset |
| Cmd+V (Mac), Shift+Insert, Ctrl+Shift+V | Paste the clipboard as typed text |

Press F9 (Setup) to change the settings: baud rate, screen colors, key
click, and more. In firmware 8;7;3 use Tab or Backspace to move and Space
to change a value. In firmware 8;7;5 use the function keys or the mouse.

The shell gets `TERM=dmd`. Most terminfo databases have a `dmd` entry.

The DMD manuals are at
[archives.loomcom.com](https://archives.loomcom.com/3b2/documents/DMD_Terminal/).
More about the original emulator is at
[loomcom.com](https://loomcom.com/3b2/dmd5620-emulator/).

## Build

You need a C99 compiler, GNU make, pkg-config and SDL2.

| System | Packages | Build |
|---|---|---|
| macOS | `brew install sdl2 pkg-config` | `make` |
| Debian, Ubuntu | `apt install build-essential pkg-config libsdl2-dev` | `make` |
| Fedora | `dnf install gcc make pkgconf SDL2-devel` | `make` |
| FreeBSD | `pkg install gmake pkgconf sdl2` | `gmake` |
| Windows (MSYS2 UCRT64) | `pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2` | `make` |

`make install` puts the program, the man page, the icons and the desktop
file under `/usr/local` (set `PREFIX` for another place).

Other make settings:

- `make STATIC=1` links SDL2 statically.
- `make SDL_CONFIG=/path/to/sdl2-config` uses that SDL2 and not the one
  pkg-config finds.
- `make CC=x86_64-w64-mingw32-gcc SDL_CONFIG=...` builds `dmd5620.exe`
  on Linux or macOS with MinGW-w64 and the SDL2 MinGW package
  (`SDL2-devel-2.x.x-mingw.tar.gz`).

## Testing

`test/difftest/run.sh` checks the C core against the Rust core. It gets
dmd_core 0.7.2, adds the test hooks in `test/difftest/rust_core.patch`,
and builds both into one program. Then it runs random instruction streams
and boots both firmware versions. After every instruction it compares the
registers and the I/O; it compares all memory every 65536 instructions.
It needs git, cargo (Rust) and a C compiler.

## Notes

- Firmware 8;7;3 works here. In the Rust core it stops at "I/O TEST":
  its DUART does not receive a break in loopback mode as a 2681 does, so
  the power-on self test repeats forever. The C core has this fix;
  `dmd_set_rust_compat(1)` turns it off.
- On Windows there is no local shell, because Windows consoles do not
  speak the DMD's terminal codes. Use telnet or a serial port.

## License

MIT. See [LICENSE.md](LICENSE.md).

- Copyright 2018-2025 Seth J. Morabito: the original emulator and the
  Rust core.
- Copyright 2026 the dmd5620 authors: the C core and the changes in this
  fork.

The firmware ROM images in `core/rom.c` come from dmd_core.
