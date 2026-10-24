#!/usr/bin/env python3
"""USART1 with two masters (review1, 2026-09-26): a host MSP frame whose payload contains 0x23
('#') must not be taken for a CLI entry (that paused the core's MSP poller for 1 s), and the
core keeps polling between the host's frames while a host tool streams requests.

  uart_evidence.py [control port 31264] [msp port 31260]

Measures how long after an `rc` change the core's own MSP_RC poll shows it in state.fc.rc_us,
(a) with nothing on the MSP socket, (b) right after host frames carrying '#' bytes, (c) while a
host tool sends MSP_STATUS requests back to back and reads the replies (checked for sanity).
"""
import os, socket, struct, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
from core_client import Core  # noqa: E402

port = int(sys.argv[1]) if len(sys.argv) > 1 else 31264
msp_port = int(sys.argv[2]) if len(sys.argv) > 2 else 31260
c = Core(port)
c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"] and "rc_us" in m["fc"], 60)


def frame(cmd, payload=b""):
    body = bytes([len(payload), cmd]) + payload
    x = 0
    for b in body:
        x ^= b
    return b"$M<" + body + bytes([x])


def rc_latency(value):
    """virtual s from sending rc (roll = value) until fc.rc_us[0] shows it"""
    t0 = c.t()
    c.send({"type": "rc", "ch_us": [value, 1500, 1500, 1500, 1000, 1000]})
    m = c.wait_for(lambda m: m["type"] == "state" and m["fc"].get("rc_us", [0])[0] == value, 30)
    return m["t"] - t0 if m else None


def cli_state():
    return c.state["fc"]["cli"]


print(f"(a) quiet socket: rc -> fc.rc_us in {rc_latency(1400):.3f} virtual s", flush=True)

s = socket.create_connection(("127.0.0.1", msp_port))
s.settimeout(0.2)
# MSP_STATUS with a one-byte payload 0x23 (the firmware ignores the payload of a read request)
hashes = frame(101, b"#") * 5 + frame(108, b"##")
s.sendall(hashes)
lat = rc_latency(1600)
got = b""
try:
    while True:
        got += s.recv(4096)
except socket.timeout:
    pass
print(f"(b) 6 host frames with 0x23 in the payload: rc -> fc.rc_us in {lat:.3f} virtual s "
      f"(a CLI-entry pause would be >= 1 s); fc.cli={cli_state()}; host got {got.count(b'$M>')} replies",
      flush=True)

stop = False
counts = {"sent": 0, "replies": 0, "bad": 0}


def streamer():
    buf = b""
    while not stop:
        s.sendall(frame(101))
        counts["sent"] += 1
        try:
            buf += s.recv(4096)
        except socket.timeout:
            pass
        while True:
            i = buf.find(b"$M>")
            if i < 0 or len(buf) < i + 6 or len(buf) < i + 6 + buf[i + 3]:
                break
            n = buf[i + 3]
            f = buf[i:i + 6 + n]
            x = 0
            for b in f[3:-1]:
                x ^= b
            if x != f[-1] or f[4] != 101:
                counts["bad"] += 1
            counts["replies"] += 1
            buf = buf[i + 6 + n:]


th = threading.Thread(target=streamer)
th.start()
time.sleep(1.0)
lats = [rc_latency(v) for v in (1450, 1550, 1450, 1550)]
stop = True
th.join()
print(f"(c) host streams MSP_STATUS: {counts['sent']} requests, {counts['replies']} replies, "
      f"{counts['bad']} not MSP_STATUS/bad checksum; meanwhile rc -> fc.rc_us in "
      + ", ".join("not within 30 host s" if v is None else f"{v:.3f}" for v in lats) + " virtual s",
      flush=True)
s.close()
