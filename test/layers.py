#!/usr/bin/env python3
"""layers test: run layers in a headless dmd5620 and use it.

usage: layers.py PATH/TO/dmd5620

dmd5620 must find layers next to itself (make layers, or a release).
The test starts layers from the shell, opens a window with the mouse
menu, runs jwin, ismpx and xts in it (xts asks layers through its agent
pipe), exits the shell in the window (layers then ends), and checks that
the first shell works again.
"""

import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

exe = os.path.abspath(sys.argv[1])
name = "layers%d" % os.getpid()
tmp = tempfile.mkdtemp(prefix="dmdlayers")
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


env = dict(os.environ)
for k in ("DMD", "DMDSYS"):
    env.pop(k, None)
emu = subprocess.Popen([exe, "--headless", "--name", name, "-s", "/bin/sh"],
                       env=env, cwd=tmp,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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


def lit_fraction():
    """Fraction of lit pixels on the whole screen."""
    _, _, png = tool("screenshot")
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(png):
        n, = struct.unpack(">I", png[pos:pos + 4])
        typ, data = png[pos + 4:pos + 8], png[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", data[:8])
        if typ == b"IDAT":
            idat += data
        pos += 12 + n
    raw = zlib.decompress(idat)
    rb = (w + 7) // 8
    lit = sum(bin(b).count("1") for y in range(h) for b in raw[y * (rb + 1) + 1:(y + 1) * (rb + 1)])
    return lit / float(w * h)


def wait_for(cond, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.5)
    return False


def file_has(path, text):
    try:
        with open(path) as f:
            return text in f.read()
    except OSError:
        return False


try:
    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "layers-test", "version": "1"}})
    bridge.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    bridge.stdin.flush()
    check(wait_for(lambda: name in tool("vm-list")[1], 10), "emulator runs")
    time.sleep(5)  # firmware boot, shell start

    # The shell finds layers on its PATH.
    out1 = os.path.join(tmp, "path.txt")
    tool("type-text", text="command -v layers > %s; echo \"$DMD\" >> %s\n" % (out1, out1))
    check(wait_for(lambda: file_has(out1, "/bin/layers"), 15),
          "layers is on the shell's PATH")
    with open(out1) as f:
        print("     " + f.read().replace("\n", "\n     ").rstrip())

    # layers boots the ROM window system: the screen turns grey (25% lit).
    tool("type-text", text="layers\n")
    check(wait_for(lambda: lit_fraction() > 0.2, 30), "layers starts (grey screen)")
    time.sleep(6)  # layers asks the terminal's ID and waits 3 s; mouse input before that is lost
    grey = lit_fraction()

    # Right button menu, "New", then sweep a window.
    tool("mouse-move", x=200, y=200, **{"wait-ms": 500})
    tool("mouse-button", action="down", button=3, **{"wait-ms": 800})
    tool("mouse-button", action="up", button=3, **{"wait-ms": 800})
    tool("mouse-move", x=60, y=60, **{"wait-ms": 600})
    tool("mouse-button", action="down", button=3, **{"wait-ms": 600})
    tool("mouse-move", x=600, y=500, **{"wait-ms": 600})
    tool("mouse-button", action="up", button=3, **{"wait-ms": 600})
    check(wait_for(lambda: lit_fraction() < grey - 0.03, 15), "new window drawn")
    time.sleep(3)  # the window's shell starts

    # jwin reads the window size, xts asks layers through the agent
    # pipe in $DMD/xt.
    out2 = os.path.join(tmp, "agent.txt")
    tool("type-text", text="(jwin; ismpx; xts; echo end) > %s 2>&1\n" % out2)
    check(wait_for(lambda: file_has(out2, "end"), 20), "commands run in the window")
    text = open(out2).read() if os.path.exists(out2) else ""
    print("     " + text.replace("\n", "\n     ").rstrip())
    check("bytes" in text and "bits" in text, "jwin gets the window size from layers")
    check("yes" in text, "ismpx says yes")
    check("packets" in text, "xts gets the statistics from layers (agent pipe)")

    # Exit the shell in the last window: layers ends.
    tool("type-text", text="exit\n")
    time.sleep(4)
    out3 = os.path.join(tmp, "back.txt")
    tool("type-text", text="echo back > %s\n" % out3)
    check(wait_for(lambda: file_has(out3, "back"), 15), "layers ends, the first shell works again")

    tool("vm-power", action="quit")
    try:
        emu.wait(timeout=10)
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
    shutil.rmtree(tmp, ignore_errors=True)

print("%d failures" % len(failures))
sys.exit(1 if failures else 0)
