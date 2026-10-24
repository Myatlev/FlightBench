#!/usr/bin/env python3
"""OSD evidence helpers (core/qemu/scripts/osd_evidence.sh runs them against its own core).

  osd_evidence.py boot --port P --until T [--cli] [--msp M]
        `osd` messages from connect to virtual T: first message full?, full/delta counts, message
        rate (Hz, per 1 s of virtual time), cells per delta; the last canvas as text art; with
        --cli: CLI serial / get of the OSD settings / tasks (the CLI's exit reboots); with --msp:
        MSP_OSD_CANVAS (189) over the MSP socket = the canvas the firmware keeps (osdConfig)
  osd_evidence.py flight --port P --until T [--pilot Q]
        listens while flight.py flies the liftoff plan (with --pilot: sends the plan itself):
        canvas before arming, after arming, spooled up, in the hover; the values of the element
        regions over time (mode, timers, DISARMED, rpm, vbat, warnings, debug); message rate
  osd_evidence.py reset --port P
        `reset firmware`: the reboot event, then the first `osd` message (clear:true?) and when
  osd_evidence.py fault --port P
        msp_osd.link_lost on for 1 s: messages stop; off: the next message is full
  osd_evidence.py sub --port P
        subscribe osd:false -> no messages; osd:true -> a full frame at once
  osd_evidence.py none --port P --until T
        a session without the device: no `osd` message, osd.frames absent/NaN, fault -> not wired,
        arming flags; CLI status (no errors)
Art: one line per row between '|', printable ASCII as is, other codes as \\xNN (4 columns wide,
so a row with symbols is wider than the canvas); attr != 0 cells are listed after the art.
"""
import argparse, math, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from core_client import Core  # noqa: E402
import flight  # noqa: E402


class Canvas:
    """what a client keeps: the app's OsdBuffer semantics (docs/protocol.md `osd`)"""

    def __init__(self):
        self.rows = self.cols = 0
        self.ch, self.at = [], []

    def apply(self, m):
        if (m["rows"], m["cols"]) != (self.rows, self.cols):
            self.rows, self.cols = m["rows"], m["cols"]
            self.ch = [[" "] * self.cols for _ in range(self.rows)]
            self.at = [[0] * self.cols for _ in range(self.rows)]
        if m.get("clear"):
            for r in range(self.rows):
                self.ch[r] = [" "] * self.cols
                self.at[r] = [0] * self.cols
        for cell in m["cells"]:
            for i, x in enumerate(cell["s"]):
                c = cell["c"] + i
                if 0 <= cell["r"] < self.rows and 0 <= c < self.cols:
                    self.ch[cell["r"]][c] = x
                    self.at[cell["r"]][c] = cell["attr"]

    def text(self, r, c0=0, n=None):
        return "".join(self.ch[r][c0:c0 + (n or self.cols)])

    def art(self, indent="  "):
        out = []
        for r in range(self.rows):
            s = "".join(x if 0x20 <= ord(x) < 0x7F else f"\\x{ord(x):02x}" for x in self.ch[r])
            out.append(f"{indent}{r:2d} |{s}|")
        attrs = [(r, c, self.at[r][c]) for r in range(self.rows) for c in range(self.cols) if self.at[r][c]]
        if attrs:
            out.append(f"{indent}attr != 0: " + ", ".join(f"({r},{c})={a:#x}" for r, c, a in attrs[:20]))
        return "\n".join(out)

    def copy(self):
        k = Canvas()
        k.rows, k.cols = self.rows, self.cols
        k.ch = [row[:] for row in self.ch]
        k.at = [row[:] for row in self.at]
        return k


def osd_msgs(c, since=0):
    return [m for m in c.msgs[since:] if m["type"] == "osd"]


def rate_report(ms, t0, t1, label=""):
    ts = [m["t"] for m in ms if t0 <= m["t"] < t1]
    if len(ts) < 2:
        print(f"  {label}{len(ts)} messages in t {t0:.1f}..{t1:.1f}")
        return
    per_s = {}
    for t in ts:
        per_s[int(t)] = per_s.get(int(t), 0) + 1
    full_s = [n for s, n in per_s.items() if t0 <= s and s + 1 <= t1]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    print(f"  {label}{len(ts)} messages in t {ts[0]:.2f}..{ts[-1]:.2f}: {(len(ts) - 1) / (ts[-1] - ts[0]):.2f} Hz; "
          f"per whole virtual second {min(full_s) if full_s else '-'}..{max(full_s) if full_s else '-'}; "
          f"interval {min(gaps) * 1000:.1f}..{max(gaps) * 1000:.1f} ms")


def summary(ms):
    full = [m for m in ms if m["clear"]]
    delta = [m for m in ms if not m["clear"]]
    nc = [sum(len(x["s"]) for x in m["cells"]) for m in delta]
    print(f"  {len(ms)} osd messages: {len(full)} full (clear:true) at t=" +
          ", ".join(f"{m['t']:.2f}" for m in full[:8]) +
          f"; {len(delta)} deltas, changed cells per delta {min(nc) if nc else 0}..{max(nc) if nc else 0} "
          f"(mean {sum(nc) / len(nc) if nc else 0:.1f}), {sum(1 for n in nc if n == 0)} with none")
    if ms:
        print(f"  first message: t={ms[0]['t']:.2f} clear={ms[0]['clear']} rows={ms[0]['rows']} cols={ms[0]['cols']} "
              f"frame={ms[0]['frame']} cells={len(ms[0]['cells'])}")


def replay(ms, until=None):
    k = Canvas()
    for m in ms:
        if until is not None and m["t"] > until:
            break
        k.apply(m)
    return k


def cli_show(c, cmds):
    for cmd in cmds:
        for line in c.cli(cmd):
            line = line.rstrip()
            if line and not line.startswith("#"):
                print(f"  cli {cmd}: {line}")


def msp_canvas(port):
    m = flight.Msp(port)
    p = m.req(189)
    return f"MSP_OSD_CANVAS -> cols {p[0]}, rows {p[1]}" if p and len(p) >= 2 else f"MSP_OSD_CANVAS -> {p!r}"


def cmd_boot(a):
    c = Core(a.port)
    c.wait_t(a.until)
    if a.msp:
        print(f"  {msp_canvas(a.msp)} (t={c.t():.2f})")
    ms = osd_msgs(c)
    summary(ms)
    rate_report(ms, max(ms[0]["t"] + 1, a.until - 8) if ms else 0, a.until, "rate: ")
    ev = [m for m in c.msgs if m["type"] == "event" and m["name"] in ("reboot", "armed", "disarmed")]
    print("  events: " + ", ".join(f"{m['name']}@{m['t']:.2f}{m['data']}" for m in ev))
    if ms:
        k = replay(ms)
        print(f"### canvas at t={ms[-1]['t']:.2f} (frame {ms[-1]['frame']}), {k.rows} x {k.cols}")
        print(k.art())
    # one full frame as sent (subscribe osd off/on -> the whole canvas at once), taken when the
    # blinking battery voltage (0.00 V: the ADC is not emulated) is in its visible phase
    for _ in range(20):
        c.send({"type": "subscribe", "osd": False})
        c.wait_t(c.t() + 0.1)
        k0 = len(c.msgs)
        c.send({"type": "subscribe", "osd": True})
        m = c.wait_for(lambda m: m["type"] == "osd", 10)
        full = next((x for x in osd_msgs(c, k0) if x["clear"]), None)
        if full and any(cell["r"] == a.vbat_row for cell in full["cells"]):
            print(f"### one full frame (clear:true, sent on subscribe osd:true) at t={full['t']:.2f}, frame {full['frame']}:"
                  f" {len(full['cells'])} runs")
            print("  cells: " + " ".join(f"({x['r']},{x['c']},{x['attr']:#x})" for x in full["cells"]))
            print(replay([full]).art())
            break
        del m
    if a.cli:
        cli_show(c, ["serial", "get displayport_msp", "get osd_displayport_device", "get vcd_video_system",
                     "get osd_framerate", "tasks"])


def find_cell(k, text):
    for r in range(k.rows):
        s = k.text(r)
        i = s.find(text)
        if i >= 0:
            return r, i
    return None


def cmd_flight(a):
    c = Core(a.port)
    if a.pilot:
        p = flight.Line(a.pilot)
        for t, ch in flight.plans(1620)["liftoff"]:
            p.send({"set": {str(k): v for k, v in ch.items()}, "at": t})
    c.wait_t(a.until)
    ms = osd_msgs(c)
    ev = [m for m in c.msgs if m["type"] == "event" and m["name"] in ("armed", "disarmed", "reboot")]
    print("  events: " + ", ".join(f"{m['name']}@{m['t']:.2f}" for m in ev))
    t_arm = next((m["t"] for m in ev if m["name"] == "armed"), None)
    summary(ms)
    # canvases at a few moments
    for label, t in (("before arming", (t_arm or 14) - 0.5), ("0.3 s after arming", (t_arm or 14) + 0.3),
                     ("1 s after arming", (t_arm or 14) + 1.0),
                     ("spooled up, on the pad", 30.0), ("in the hover", a.until - 0.1)):
        k = replay(ms, t)
        print(f"### canvas {label}, t <= {t:.2f}")
        print(k.art())
    # how the regions changed through the flight: every message, the text in a few regions
    regions = {"mode (r1 c1..4)": (1, 1, 4), "timer1 (r1 c24..)": (1, 24, 7), "timer2 (r2 c24..)": (2, 24, 7),
               "disarmed (r4 c21..)": (4, 21, 8), "warnings (r12 c19..)": (12, 14, 22),
               "rpm (r15 c1..)": (15, 1, 6), "vbat (r16 c1..)": (16, 1, 8), "debug (r17)": (17, 1, 27)}
    k = Canvas()
    seen = {n: [] for n in regions}
    for m in ms:
        k.apply(m)
        for n, (r, c0, w) in regions.items():
            v = k.text(r, c0, w) if k.rows > r else ""
            if not seen[n] or seen[n][-1][1] != v:
                seen[n].append((m["t"], v))
    for n, ch in seen.items():
        vals = ", ".join(f"{t:.2f}:'{v.rstrip()}'" for t, v in ch[:6])
        more = f" ... {len(ch)} distinct values, last {ch[-1][0]:.2f}:'{ch[-1][1].rstrip()}'" if len(ch) > 6 else ""
        print(f"  {n}: {vals}{more}")
    if t_arm:
        rate_report(ms, t_arm + 1, a.until, "rate armed: ")
    rate_report(ms, 34, a.until, "rate airborne: ")


def cmd_reset(a):
    c = Core(a.port)
    c.wait_t(c.t() + 1.0)
    k0 = len(c.msgs)
    t_req = c.t()
    c.send({"type": "reset", "what": "firmware"})
    c.wait_t(t_req + 4.0)
    after = c.msgs[k0:]
    reb = next((m for m in after if m["type"] == "event" and m["name"] == "reboot"), None)
    ms = [m for m in after if m["type"] == "osd"]
    print(f"  reset firmware at t={t_req:.2f}; reboot event: {reb and (reb['t'], reb['data'])}")
    last_before = [m for m in ms if reb and m["t"] <= reb["t"] and m.get("link")]
    first_after = next((m for m in ms if reb and m["t"] >= reb["t"] and m.get("link")), None)
    print("  link:false: " + ("; ".join(show(m) for m in link_msgs(ms)) or "none"))
    print(f"  osd messages between the request and the reboot: {len(last_before)}; first after: " +
          (f"t={first_after['t']:.2f} (+{first_after['t'] - reb['t']:.2f} s) clear={first_after['clear']} "
           f"frame={first_after['frame']} cells={len(first_after['cells'])}" if first_after else "none"))
    fulls = [m for m in ms if m["clear"]]
    print("  full frames after the request: " + ", ".join(f"t={m['t']:.2f}" for m in fulls))
    if first_after:
        print("### first frame after the reboot")
        print(replay([first_after]).art())
        print("### canvas 4 s after the request")
        print(replay(c.msgs and [m for m in c.msgs if m["type"] == "osd"]).art())


def cmd_fault(a):
    c = Core(a.port)
    c.wait_t(c.t() + 1.0)
    k0 = len(c.msgs)
    t_on = c.t()
    c.send({"type": "fault", "device": "msp_osd", "kind": "link_lost", "on": True})
    c.wait_t(t_on + 1.0)
    t_off = c.t()
    c.send({"type": "fault", "device": "msp_osd", "kind": "link_lost", "on": False})
    c.wait_t(t_off + 1.0)
    after = c.msgs[k0:]
    for m in after:
        if m["type"] in ("event", "error") and (m["type"] == "error" or m["name"] == "fault"):
            print(f"  {m}")
    lf = link_msgs([m for m in after if m["type"] == "osd"])
    print("  link:false: " + ("; ".join(show(m) for m in lf) or "none"))
    during = [m for m in after if m["type"] == "osd" and t_on + 0.1 < m["t"] <= t_off and m.get("link")]
    back = [m for m in after if m["type"] == "osd" and m["t"] > t_off]
    print(f"  link_lost on at t={t_on:.2f}: osd messages while on: {len(during)}; off at t={t_off:.2f}: first after "
          + (show(back[0]) if back else "none"))
    frames = c.state and None
    del frames


def cmd_sub(a):
    c = Core(a.port)
    c.wait_t(c.t() + 0.5)
    c.send({"type": "subscribe", "osd": False})
    c.wait_t(c.t() + 0.2)
    k0 = len(c.msgs)
    t0 = c.t()
    c.wait_t(t0 + 1.0)
    n_off = len(osd_msgs(c, k0))
    k1 = len(c.msgs)
    t1 = c.t()
    c.send({"type": "subscribe", "osd": True})
    c.wait_t(t1 + 0.5)
    ms = osd_msgs(c, k1)
    print(f"  osd:false for 1 s: {n_off} messages; osd:true at t={t1:.2f}: first t={ms[0]['t']:.2f} "
          f"clear={ms[0]['clear']} cells={len(ms[0]['cells'])}, then {len(ms) - 1} deltas in 0.5 s" if ms else
          f"  osd:false: {n_off} messages; osd:true: none")
    c.send({"type": "subscribe", "osd": "yes"})
    e = c.wait_for(lambda m: m["type"] == "error", 5)
    print(f"  subscribe osd:\"yes\" -> {e}")


def cmd_none(a):
    c = Core(a.port)
    c.send({"type": "subscribe", "signals": ["osd.frames"]})
    c.wait_t(a.until)
    ms = osd_msgs(c)
    s = c.wait_for(lambda m: m["type"] == "samples", 5)
    print(f"  osd messages until t={a.until:.1f}: {len(ms)}; osd.frames = {s and s['v'].get('osd.frames')}")
    k = len(c.msgs)
    c.send({"type": "fault", "device": "msp_osd", "kind": "link_lost", "on": True})
    c.wait_t(c.t() + 0.3)
    for m in c.msgs[k:]:
        if m["type"] == "error":
            print(f"  fault msp_osd.link_lost -> {m}")
    print(f"  state.fc: armed={c.state['fc']['armed']} flags={c.state['fc']['arming_flags']}")


def bench(mon):
    """the machine's bench counters (qom-get /machine bench) as a dict of the key=value words"""
    import re, socket
    s = socket.create_connection(("127.0.0.1", mon))
    s.settimeout(0.5)
    out = b""
    s.sendall(b"qom-get /machine bench\n")
    end = time.time() + 3
    while time.time() < end and b"osd_bad" not in out:
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        out += d
    s.close()
    return dict(re.findall(r"(\w+)=([^\s\"]+)", out.decode(errors="replace")))


def link_msgs(ms):
    return [m for m in ms if m.get("link") is False]


def show(m):
    return (f"t={m['t']:.2f} link={str(m.get('link')).lower()} clear={str(m['clear']).lower()} "
            f"frame={m['frame']} cells={len(m['cells'])}")


def cmd_power(a):
    """power off: one osd link:false (clear, no cells) at once, nothing while off; power on: the
    first frame is full with link:true"""
    c = Core(a.port)
    c.wait_t(c.t() + 1.0)
    k0 = len(c.msgs)
    t_off = c.t()
    c.send({"type": "power", "on": False})
    time.sleep(3.0)                         # powered off the state stream may stop: host time
    k1 = len(c.msgs)
    c.send({"type": "power", "on": True})
    c.wait_t(c.t() + 4.0)
    off = [m for m in c.msgs[k0:k1] if m["type"] == "osd"]
    on = [m for m in c.msgs[k1:] if m["type"] == "osd"]
    ev = [m for m in c.msgs[k0:] if m["type"] == "event" and m["name"] in ("power", "reboot")]
    print("  events: " + ", ".join(f"{m['name']}@{m['t']:.2f}{m['data']}" for m in ev))
    print(f"  power off at t~{t_off:.2f}: {len(off)} osd messages while off: " + "; ".join(show(m) for m in off[:3]))
    print(f"  power on: first osd message {show(on[0]) if on else 'none'}; link:false after power on: "
          f"{len(link_msgs(on))}")


def cmd_cli(a):
    """CLI mode: link:false when the core sees `Entering CLI Mode`, no frames while in it; `exit`
    reboots: the first frame after it full with link:true"""
    c = Core(a.port)
    c.wait_t(c.t() + 1.0)
    k0 = len(c.msgs)
    lines = c.cli("get osd_framerate_hz")
    t_in = c.t()
    c.wait_t(t_in + 1.0)
    k1 = len(c.msgs)
    c.cli("exit", first_s=1.0)
    c.wait_t(c.t() + 4.0)
    ent = next((m for m in c.msgs[k0:] if m["type"] == "log" and "firmware in CLI mode" in m["text"]), None)
    during = [m for m in c.msgs[k0:k1] if m["type"] == "osd"]
    after = [m for m in c.msgs[k1:] if m["type"] == "osd"]
    reb = next((m for m in c.msgs[k1:] if m["type"] == "event" and m["name"] == "reboot"), None)
    print(f"  cli: {[x.strip() for x in lines if 'osd_framerate' in x][:1]}; core log 'firmware in CLI mode' at "
          f"t={ent and round(ent['t'], 3)}")
    lf = link_msgs(during)
    print(f"  link:false messages from the CLI entry: {len(lf)}" + (f", first {show(lf[0])}" if lf else ""))
    print(f"  link:true frames between the link:false and exit: "
          f"{sum(1 for m in during if lf and m['t'] > lf[0]['t'] and m.get('link'))}")
    real = [m for m in after if m.get("link")]
    print(f"  exit: reboot at t={reb and round(reb['t'], 2)}; first frame after: {show(real[0]) if real else 'none'}; "
          f"link:false after exit: {len(link_msgs(after))}")


def cmd_rate2(a):
    """a session with osd_framerate_hz = 2: the rate, contacts stay put, no link:false"""
    c = Core(a.port)
    c.wait_t(a.from_t)
    b0 = bench(a.mon)
    k0 = len(c.msgs)
    c.wait_t(a.until)
    b1 = bench(a.mon)
    ms = osd_msgs(c)
    later = [m for m in c.msgs[k0:] if m["type"] == "osd"]
    ev = [m for m in c.msgs if m["type"] == "event" and m["name"] == "reboot"]
    print("  reboots: " + ", ".join(f"{m['t']:.2f}{m['data']}" for m in ev))
    print(f"  all osd messages to t={a.until:.0f}: {len(ms)}, link:false {len(link_msgs(ms))} at "
          + ", ".join(f"{m['t']:.2f}" for m in link_msgs(ms)))
    rate_report(later, a.from_t, a.until, "rate: ")
    print(f"  bench at t={a.from_t:.0f}: contacts {b0.get('osd_contacts')} frames {b0.get('osd_frames')}; at "
          f"t={a.until:.0f}: contacts {b1.get('osd_contacts')} frames {b1.get('osd_frames')}; "
          f"t {a.from_t:.0f}..{a.until:.0f}: {len(later)} messages, full {sum(1 for m in later if m['clear'])}, "
          f"link:false {len(link_msgs(later))}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("boot"); s.add_argument("--port", type=int, required=True)
    s.add_argument("--until", type=float, default=12.0); s.add_argument("--cli", action="store_true")
    s.add_argument("--msp", type=int); s.add_argument("--vbat-row", type=int, default=16)
    s = sub.add_parser("flight"); s.add_argument("--port", type=int, required=True)
    s.add_argument("--pilot", type=int); s.add_argument("--until", type=float, default=45.0)
    s = sub.add_parser("reset"); s.add_argument("--port", type=int, required=True)
    s = sub.add_parser("fault"); s.add_argument("--port", type=int, required=True)
    s = sub.add_parser("sub"); s.add_argument("--port", type=int, required=True)
    s = sub.add_parser("none"); s.add_argument("--port", type=int, required=True)
    s.add_argument("--until", type=float, default=10.0)
    s = sub.add_parser("power"); s.add_argument("--port", type=int, required=True)
    s = sub.add_parser("cli"); s.add_argument("--port", type=int, required=True)
    s = sub.add_parser("rate2"); s.add_argument("--port", type=int, required=True)
    s.add_argument("--mon", type=int, required=True)
    s.add_argument("--from-t", type=float, default=8.0); s.add_argument("--until", type=float, default=30.0)
    a = ap.parse_args()
    {"boot": cmd_boot, "flight": cmd_flight, "reset": cmd_reset, "fault": cmd_fault, "sub": cmd_sub,
     "none": cmd_none, "power": cmd_power, "cli": cmd_cli, "rate2": cmd_rate2}[a.cmd](a)


if __name__ == "__main__":
    main()
