# Using the DMD 5620 MCP server

The emulator has a built-in [MCP](https://modelcontextprotocol.io/) server.
With it, an AI agent can use the terminal like a person: look at the
screen, type, and use the mouse. It works on macOS, Linux, FreeBSD and
Windows 10 or later.

## How it works

- **Every emulator offers MCP.** It is on by default. Each one opens a
  local socket in `/tmp/dmd5620-$UID/`, a folder only your user can open.
  On Windows the folder is `%TEMP%\dmd5620`.
- **`dmd5620 --mcp-bridge` is the MCP server** that your agent starts. It
  finds all running emulators and passes each tool call to the one you name.
- **Emulators can come and go.** The bridge keeps running.

## Setup

1. Get or build `dmd5620` (see [README.md](README.md)).
2. Add the bridge to your agent once.
   - Claude Code: `claude mcp add dmd5620 -s user -- /path/to/dmd5620 --mcp-bridge`
   - With the macOS app, the program is
     `/Applications/DMD5620.app/Contents/MacOS/dmd5620`.
   - Other MCP clients: a stdio server with that command.
3. Start an emulator from a shell: `dmd5620` (window) or
   `dmd5620 --headless` (no window; the agent still sees the screen).
   On macOS, opening `DMD5620.app` also starts one.
4. In the agent: `vm-list`, then `screenshot`.

## Settings

| Setting | What it does |
|---|---|
| `--name NAME`, `DMD_NAME=` | The VM name in the tools (default `dmd5620`) |
| `--headless`, `DMD_HEADLESS=1` | No window |
| `--no-mcp`, `DMD_MCP=0` | No MCP socket |
| `--scale N` | Window size; 1 = 800x1024, pixel for pixel (default) |

## Tools

Every tool takes `vm`: a name or PID from `vm-list`. You may leave it out
when only one emulator runs. Inputs take `screenshot: true` (and `wait-ms`,
default 300) to return a screenshot after the input.

| Tool | What it does |
|---|---|
| `vm-list` | The running emulators: name, PID, socket |
| `vm-info` | Firmware, connection, uptime, mouse, CPU PC |
| `screenshot` | PNG of the 800x1024 screen. `region` {x,y,w,h} crops, `zoom` 1-8 enlarges |
| `key` | One key: a character, Return, Tab, BackSpace, Escape, Delete, Up, Down, Left, Right, F1-F8, Setup, `ctrl+X`, `shift+X`; or `code` 0-255 |
| `type-text` | ASCII text; `\n` is Return, `\t` is Tab |
| `mouse-move` | Move the cursor to x,y |
| `mouse-click` | Click button 1, 2 or 3 (left, middle, right), at x,y if given |
| `mouse-button` | `down` or `up`, at x,y if given. For menus: down, move, up |
| `vm-power` | `reset` restarts the firmware, `quit` ends the emulator |

Coordinates are screen pixels, origin top-left, in all tools.

## Notes

- **The DMD mouse is relative.** The firmware moves its cursor by the
  change of two counters and stops it at the screen edges. So each
  `mouse-move` first pushes the cursor into the top-left corner, then
  moves it by x,y. While a button is down there is no homing, so a menu
  stays open.
- **Keys go through the keyboard port** at its baud rate, like a real
  keyboard. `type-text` waits `delay-ms` (default 10) between characters.
- **Paste:** Cmd-V on a Mac, Shift-Insert or Ctrl-Shift-V elsewhere,
  types the clipboard. Other Cmd keys stay with macOS (Cmd-Q quits).
- **Windows** needs Windows 10 version 1803 or later for the local
  sockets. Wine does not have them, so MCP is off there.
- The shell gets `TERM=dmd` and an 88x70 window size.
