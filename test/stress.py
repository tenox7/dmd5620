#!/usr/bin/env python3
"""Stress test: fast typing while the host floods the screen.

usage: stress.py PATH/TO/dmd5620 [FIRMWARE]

The host (a TCP server in this script) sends text without a pause, so the
firmware is busy scrolling. Meanwhile the MCP bridge types fast. The host
must get exactly the typed text: no lost keys, no repeated keys, and no
endless stream of one key (the DUART receive shift register bug).
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time

exe = os.path.abspath(sys.argv[1])
firmware = sys.argv[2] if len(sys.argv) > 2 else "8;7;5"
name = "stress%d" % os.getpid()
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
received = bytearray()
conn_box = []
flood = threading.Event()


def host():
    c, _ = srv.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn_box.append(c)

    def reader():
        while True:
            try:
                d = c.recv(4096)
            except OSError:
                break
            if not d:
                break
            received.extend(d)

    threading.Thread(target=reader, daemon=True).start()
    line = 0
    while True:
        if flood.is_set():
            line += 1
            try:
                c.sendall(b"flood line %06d the quick brown fox jumps over the lazy dog\r\n" % line)
            except OSError:
                break
        else:
            time.sleep(0.01)


threading.Thread(target=host, daemon=True).start()

emu = subprocess.Popen([exe, "--headless", "--name", name, "-f", firmware,
                        "--tcp", "127.0.0.1:%d" % port],
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
    return r.get("isError", False), "".join(c.get("text", "") for c in r["content"]
                                           if c["type"] == "text")


try:
    rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                       "clientInfo": {"name": "stress", "version": "1"}})
    for _ in range(100):
        if name in tool("vm-list")[1] and conn_box:
            break
        time.sleep(0.1)
    check(bool(conn_box), "emulator connected to the host")
    time.sleep(4)  # firmware boot

    for delay in (10, 5, 1, 0):
        text = "fast-typing-%d-abcdefghijklmnopqrstuvwxyz-0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ" % delay
        del received[:]
        flood.set()
        time.sleep(0.5)
        tool("type-text", text=text, **{"delay-ms": delay})
        time.sleep(1.5)
        flood.clear()
        time.sleep(1.5)
        got = bytes(received)
        n1 = len(got)
        time.sleep(1)
        n2 = len(received)
        check(n2 == n1, "delay %d ms: no endless key stream (%d -> %d bytes)" % (delay, n1, n2))
        check(got == text.encode(), "delay %d ms: host got exactly the typed text%s"
              % (delay, "" if got == text.encode() else ": %r" % got[:120]))
        del received[:]
        tool("type-text", text="slow", **{"delay-ms": 80})
        time.sleep(1)
        check(bytes(received) == b"slow", "delay %d ms: slow typing works after it (%r)"
              % (delay, bytes(received)[:40]))

    err, info = tool("vm-info")
    check(not err, "emulator still answers")
    tool("vm-power", action="quit")
    emu.wait(timeout=10)
finally:
    if emu.poll() is None:
        emu.kill()
    bridge.stdin.close()
    bridge.wait(timeout=10)

print("%d failures" % len(failures))
sys.exit(1 if failures else 0)
