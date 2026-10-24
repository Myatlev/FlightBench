#!/usr/bin/env python3
"""Send HMP monitor commands to a running QEMU and print the replies.

usage: hmp.py <port> "cmd1" ["cmd2" ...]
"""
import socket, sys, time

s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.settimeout(0.3)


def drain():
    out = b""
    while True:
        try:
            d = s.recv(4096)
        except socket.timeout:
            return out.decode(errors="replace")
        if not d:
            return out.decode(errors="replace")
        out += d


drain()
for c in sys.argv[2:]:
    s.sendall(c.encode() + b"\n")
    time.sleep(0.1)
    lines = [l for l in drain().replace("\r", "").split("\n")
             if l.strip() and not l.startswith("(qemu)") and c not in l]
    print(f"{c}: {' | '.join(lines)}")
