#!/usr/bin/env python3
"""Probe a firmware's MSP/CLI port over TCP (e.g. the core's MSP port).

  msp_probe.py [host] [port] [--cli "status"]

Without --cli: sends MSP v1 MSP_API_VERSION (1) and MSP_FC_VARIANT (2), prints the replies.
With --cli:    enters the CLI with '#', sends the command, prints what comes back.
               WARNING: the port then STAYS in CLI mode and stops answering MSP (configurator will not
               connect) until 'exit' is sent, which reboots the firmware. Pass --exit to do that at the end.
"""
import socket, struct, sys, time

def msp_v1(cmd, payload=b""):
    body = bytes([len(payload), cmd]) + payload
    crc = 0
    for b in body:
        crc ^= b
    return b"$M<" + body + bytes([crc])

def read_msp(sock, timeout=3.0):
    sock.settimeout(timeout)
    buf = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            chunk = sock.recv(256)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        i = buf.find(b"$M>")
        if i >= 0 and len(buf) >= i + 5:
            n = buf[i + 3]
            if len(buf) >= i + 5 + n + 1:
                cmd = buf[i + 4]
                payload = buf[i + 5:i + 5 + n]
                return cmd, payload
    return None, buf

def main():
    host = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 5761
    cli = None
    if "--cli" in sys.argv:
        cli = sys.argv[sys.argv.index("--cli") + 1]
    s = socket.create_connection((host, port), timeout=5)
    print(f"connected {host}:{port}")
    if cli is None:
        for cmd, name in ((1, "MSP_API_VERSION"), (2, "MSP_FC_VARIANT"), (3, "MSP_FC_VERSION"), (4, "MSP_BOARD_INFO")):
            s.sendall(msp_v1(cmd))
            rcmd, payload = read_msp(s)
            if rcmd is None:
                print(f"{name}: no reply (got {payload!r})")
            else:
                text = payload.decode("ascii", "replace") if cmd in (2,) else payload.hex()
                print(f"{name}: cmd={rcmd} payload={text}")
    else:
        s.sendall(b"#")
        time.sleep(0.5)
        s.sendall(cli.encode() + b"\n")
        s.settimeout(2.0)
        out = b""
        t0 = time.time()
        while time.time() - t0 < 20:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                if out.rstrip().endswith(b"#") or b"CLI>" in out:
                    break
                continue
            if not chunk:
                break
            out += chunk
            if out.rstrip().endswith(b"# ") or out.rstrip().endswith(b"#"):
                break
        print(out.decode("utf-8", "replace"))
        if "--exit" in sys.argv:
            s.sendall(b"exit\n")   # leaves CLI mode; on RotorFlight this reboots the board
            time.sleep(0.5)
    s.close()

if __name__ == "__main__":
    main()
