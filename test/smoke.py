#!/usr/bin/env python3
"""Smoke test: run dmd5620 headless and drive it through the MCP bridge.

usage: smoke.py PATH/TO/dmd5620 [FIRMWARE]

The host line is a small TCP server in this script (--tcp), so the test
is the same on every system. It checks that the firmware boots, that
host text reaches the screen, that typed keys reach the host, that the
mouse and the Setup key work, and that vm-power quit ends the emulator.
"""

import base64
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

exe = os.path.abspath(sys.argv[1])
firmware = sys.argv[2] if len(sys.argv) > 2 else "8;7;5"
name = "smoke%d" % os.getpid()
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


# ---- the host: a TCP server that records what the terminal sends ----
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
received = bytearray()
conn_box = []


def host():
    c, _ = srv.accept()
    conn_box.append(c)
    while True:
        try:
            d = c.recv(4096)
        except OSError:
            break
        if not d:
            break
        received.extend(d)


threading.Thread(target=host, daemon=True).start()

emu = subprocess.Popen([exe, "--headless", "--name", name, "-f", firmware,
                        "--tcp", "127.0.0.1:%d" % port],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

# ---- the MCP bridge ----
bridge = subprocess.Popen([exe, "--mcp-bridge"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE)
rid = 0


def rpc(method, params=None):
    global rid
    rid += 1
    msg = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        msg["params"] = params
    bridge.stdin.write((json.dumps(msg) + "\n").encode())
    bridge.stdin.flush()
    return json.loads(bridge.stdout.readline())


def tool(name_, **args):
    args.setdefault("vm", name)
    r = rpc("tools/call", {"name": name_, "arguments": args})["result"]
    text = "".join(c.get("text", "") for c in r["content"] if c["type"] == "text")
    png = [base64.b64decode(c["data"]) for c in r["content"] if c["type"] == "image"]
    return r.get("isError", False), text, (png[0] if png else None)


def png_pixels(png):
    """Decode our 1-bit PNG. Returns (w, h, rows of bytes)."""
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(png):
        n, = struct.unpack(">I", png[pos:pos + 4])
        typ, data = png[pos + 4:pos + 8], png[pos + 8:pos + 8 + n]
        crc, = struct.unpack(">I", png[pos + 8 + n:pos + 12 + n])
        assert crc == zlib.crc32(typ + data) & 0xffffffff, "PNG CRC"
        if typ == b"IHDR":
            w, h = struct.unpack(">II", data[:8])
        if typ == b"IDAT":
            idat += data
        pos += 12 + n
    raw = zlib.decompress(idat)
    rb = (w + 7) // 8
    return w, h, [raw[y * (rb + 1) + 1:(y + 1) * (rb + 1)] for y in range(h)]


def lit(rows, y0, y1):
    return sum(bin(b).count("1") for r in rows[y0:y1] for b in r)


try:
    init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "smoke", "version": "1"}})
    check(init["result"]["serverInfo"]["name"] == "dmd5620", "MCP initialize")
    bridge.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    bridge.stdin.flush()

    tools = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
    check("screenshot" in tools and "type-text" in tools, "tools/list: %d tools" % len(tools))

    found = False
    for _ in range(100):
        _, text, _ = tool("vm-list")
        if name in text:
            found = True
            break
        time.sleep(0.1)
    check(found, "vm-list shows the emulator")

    # Let the firmware boot, then send text from the host.
    for _ in range(100):
        if conn_box:
            break
        time.sleep(0.1)
    check(bool(conn_box), "emulator connected to the host")
    time.sleep(4)
    err, text, png = tool("screenshot")
    w, h, rows = png_pixels(png)
    check((w, h) == (800, 1024), "screenshot is 800x1024")
    before = lit(rows, 0, 40)

    conn_box[0].sendall(b"\r\nSMOKE TEST HOST TEXT 0123456789\r\n")
    time.sleep(2)
    err, text, png = tool("screenshot")
    _, _, rows = png_pixels(png)
    after = lit(rows, 0, 60)
    check(after > before + 200, "host text drawn on the screen (%d -> %d lit pixels)" % (before, after))

    err, text, _ = tool("type-text", text="hello\n")
    time.sleep(1.5)
    check(b"hello\r" in bytes(received), "typed keys reach the host: %r" % bytes(received[-20:]))

    err, text, png = tool("key", key="Setup", screenshot=True, **{"wait-ms": 1500})
    _, _, rows = png_pixels(png)
    check(lit(rows, 980, 1024) > 2000, "Setup shows the menu at the bottom")
    tool("key", key="Setup", screenshot=True, **{"wait-ms": 1500})

    err, text, _ = tool("mouse-move", x=400, y=500)
    check(not err and "400,500" in text, "mouse-move: " + text.strip())

    err, text, _ = tool("vm-info")
    check("firmware: " + firmware in text, "vm-info names the firmware")

    tool("vm-power", action="quit")
    try:
        emu.wait(timeout=10)
        check(True, "vm-power quit ends the emulator")
    except subprocess.TimeoutExpired:
        check(False, "vm-power quit ends the emulator")
finally:
    if emu.poll() is None:
        emu.kill()
    bridge.stdin.close()
    bridge.wait(timeout=10)
    out = emu.stdout.read().decode(errors="replace")
    if out.strip():
        print("emulator output:\n" + out)

print("%d failures" % len(failures))
sys.exit(1 if failures else 0)
