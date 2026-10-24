#!/usr/bin/env python3
"""Capture raw MSP replies from a running core's MSP host socket as test fixtures.

usage: msp_capture.py <msp port> <out.json> [cmd ...]
  cmd: a number or a name below (default: fc_variant fc_version boxnames boxnames:1 boxids boxids:1
  rx_map mode_ranges mode_ranges_extra status inav_status osd_config); `name:N` sends the page byte N.
Requests are MSP v2 ($X<), so replies of any length come whole; an error reply ($X!) is kept as
{"error": true}. The output is {"<name>": {"cmd": N, "page": P|null, "hex": "..."}} — what
core/qemu/tests/test_msp.c decodes (core/qemu/tests/fixtures/).
"""
import json, socket, sys, time

CMDS = {"fc_variant": 2, "fc_version": 3, "board_info": 4, "mode_ranges": 34, "rx_map": 64, "osd_config": 84,
        "status": 101, "boxnames": 116, "boxids": 119, "mode_ranges_extra": 238,
        "inav_status": 0x2000}
DEFAULT = ["fc_variant", "fc_version", "boxnames", "boxnames:1", "boxids", "boxids:1", "rx_map", "mode_ranges",
           "mode_ranges_extra", "status", "inav_status", "osd_config"]


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0xD5) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def v2(cmd, payload=b""):
    body = bytes([0, cmd & 0xFF, cmd >> 8, len(payload) & 0xFF, len(payload) >> 8]) + payload
    return b"$X<" + body + bytes([crc8(body)])


def request(s, cmd, payload=b"", timeout=3.0):
    s.sendall(v2(cmd, payload))
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            buf += s.recv(4096)
        except socket.timeout:
            continue
        for tag in (b"$X>", b"$X!"):
            i = buf.find(tag)
            if i >= 0 and len(buf) >= i + 8:
                n = buf[i + 6] | buf[i + 7] << 8
                if len(buf) >= i + 9 + n and buf[i + 4] | buf[i + 5] << 8 == cmd:
                    if crc8(buf[i + 3:i + 8 + n]) != buf[i + 8 + n]:
                        raise ValueError(f"MSP {cmd}: bad checksum")
                    return tag == b"$X!", buf[i + 8:i + 8 + n]
    raise TimeoutError(f"no reply to MSP {cmd}")


def main():
    port, out = int(sys.argv[1]), sys.argv[2]
    s = socket.create_connection(("127.0.0.1", port))
    s.settimeout(0.2)
    res = {}
    for w in sys.argv[3:] or DEFAULT:
        name, _, page = w.partition(":")
        cmd = CMDS[name] if name in CMDS else int(name, 0)
        payload = bytes([int(page)]) if page else b""
        err, data = request(s, cmd, payload)
        res[w] = {"cmd": cmd, "page": int(page) if page else None, "hex": data.hex()}
        if err:
            res[w]["error"] = True
        print(f"{w}: {'error' if err else len(data)} {data[:48]!r}")
    json.dump(res, open(out, "w"), indent=1)
    print(f"wrote {out}")


main()
