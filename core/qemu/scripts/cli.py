#!/usr/bin/env python3
"""Enter the RotorFlight CLI over a TCP-bridged UART and run commands.

usage: cli.py <port> [wait_s] [command ...]     (default command: status)
Waits wait_s seconds (boot), sends '#', then each command, printing the replies.
"""
import socket, time, sys

port = int(sys.argv[1])
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 8
cmds = sys.argv[3:] or ["status"]
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", port)); break
    except OSError:
        time.sleep(0.1)
s.settimeout(0.5)


def drain(t):
    out = b""; end = time.time() + t
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d:
                break
            out += d
        except socket.timeout:
            pass
    return out


time.sleep(wait)
pre = drain(0.5)
s.sendall(b"#")
a = drain(2)
print("after #:", a.decode(errors="replace").strip())
# one byte at a time with a small gap (CLI_CHAR_MS): whole lines sent at once
# occasionally lost a character while the firmware was still printing
gap = float(__import__("os").environ.get("CLI_CHAR_MS", "3")) / 1000
for c in cmds:
    for ch in (c + "\n").encode():
        s.sendall(bytes([ch]))
        time.sleep(gap)
    print(f"--- {c}")
    print(drain(float(__import__("os").environ.get("CLI_DRAIN", "3"))).decode(errors="replace"))
