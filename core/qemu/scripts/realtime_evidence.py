#!/usr/bin/env python3
"""Review check for the realtime throttle and the non-blocking sockets (review1, 2026-09-26).

Against a running core (control 31264, pilot 31263): realtime on/off at several speeds, measured
virtual/host rate vs the core's timing.realtime_factor, and the control server's responsiveness
while throttled (ping -> pong round trip in host ms: the throttle sleeps with the BQL released, so
the main loop keeps answering). Then a pilot client that floods commands and never reads its replies:
the core must keep running and log the dropped output.
"""
import os, socket, statistics, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))
from core_client import Core  # noqa: E402

port = int(sys.argv[1]) if len(sys.argv) > 1 else 31264
pilot = int(sys.argv[2]) if len(sys.argv) > 2 else 31263
c = Core(port)
print(f"hello t={c.hello['t']} realtime={c.state and c.state['timing']['realtime']}", flush=True)
c.wait_t(c.t() + 2.0)


def ping_rtt():
    t = time.time()
    c.send({"type": "ping"})
    m = c.wait_for(lambda m: m["type"] == "pong", 5)
    return (time.time() - t) * 1000 if m else float("inf")


for rt, sp in [(False, 1.0), (True, 1.0), (False, 1.0), (True, 1.0), (True, 0.5), (True, 0.25), (True, 0.1), (False, 1.0)]:
    c.send({"type": "time", "realtime": rt, "speed": sp})
    time.sleep(2.0)
    tv, th = c.t(), time.time()
    rtts = []
    while time.time() - th < 10.0:
        rtts.append(ping_rtt())
        time.sleep(0.2)
    dv, dh = c.t() - tv, time.time() - th
    print(f"realtime={rt} speed={sp}: {dv:.3f} virtual s in {dh:.2f} host s = {dv / dh:.3f}x, "
          f"core says {c.state['timing']['realtime_factor']}; ping rtt median {statistics.median(rtts):.1f} ms, "
          f"max {max(rtts):.1f} ms ({len(rtts)} pings)", flush=True)

# a pilot client that sends and never reads: its replies fill its socket, then the core's 16 KiB
# ring, then are dropped (counted in the core's log). With the old blocking write the whole
# emulator would stop here. Then the client just sits there, unread, while the core runs on.
s = socket.create_connection(("127.0.0.1", pilot))
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
for _ in range(250):
    s.sendall(b"{}\n" * 2000)          # 500k commands -> ~9 MB of "ok t=..." replies
    time.sleep(0.05)
time.sleep(1.0)
tv, th = c.t(), time.time()
rtts = []
while time.time() - th < 10.0:
    rtts.append(ping_rtt())
    time.sleep(0.2)
dv, dh = c.t() - tv, time.time() - th
print(f"pilot client sent 500000 commands and reads nothing: over the next {dh:.1f} host s the core ran "
      f"{dv:.2f} virtual s ({dv / dh:.3f}x); ping rtt median {statistics.median(rtts):.1f} ms, "
      f"max {max(rtts):.1f} ms", flush=True)
s.close()
time.sleep(1.0)
print(f"after close: ping rtt {ping_rtt():.1f} ms, t={c.t():.2f}", flush=True)
