#!/usr/bin/env python3
"""Baro evidence helpers (core/qemu/scripts/baro_evidence.sh runs them against its own core).

  baro_evidence.py pad --port P            on the pad: baro signals, the firmware's altitude after
                                           calibration, then CLI status + tasks (the CLI's exit reboots)
  baro_evidence.py record --port P --out f.csv --until T
                                           samples of fdm.alt_m, fc.baro_alt_m, baro.pressure_pa,
                                           baro.temp_c at 20 Hz until virtual time T (runs next to flight.py)
  baro_evidence.py table f.csv [--from T0] [--to T1] [--every S]
                                           t / fdm_alt / fc_baro_alt table and the tracking error
  baro_evidence.py cli --port P [cmd...]   disarm (ch5 low), wait, CLI commands (after a flight)
  baro_evidence.py freeze --port P --pilot Q --until T
                                           lift off (flight.py's liftoff plan), climb, dps310.freeze in
                                           the climb, off, prints the table around it
  baro_evidence.py notwired --port P       fault dps310.freeze on a session without the baro -> error
  baro_evidence.py freezereset --port P --mon M
                                           on the pad: dps310.freeze, then an MCU reset (the firmware
                                           soft-resets the chip) and a power cycle (the chip's power-on
                                           reset): the chip's result registers (bench baro_regs) must
                                           stay the frozen bytes; freeze off -> live again
"""
import argparse, csv, math, os, socket, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from core_client import Core  # noqa: E402
import flight  # noqa: E402

SIGS = ["fdm.alt_m", "fc.baro_alt_m", "baro.pressure_pa", "baro.temp_c", "fdm.vel_d_mps"]


def fmt(v, nd=2):
    return "nan" if v is None or (isinstance(v, float) and math.isnan(v)) else f"{v:.{nd}f}"


def samples(c, t_end, out=None):
    """collect samples until virtual t_end: list of (t, {id: v})"""
    rows = []
    while True:
        m = c.wait_for(lambda m: m["type"] == "samples", 120)
        if m is None:
            break
        rows.append((m["t"], m["v"]))
        if out:
            out.writerow([f"{m['t']:.3f}"] + [m["v"].get(k) for k in SIGS])
        if m["t"] >= t_end:
            break
    return rows


def cli_lines(c, cmds, keep=("BARO", "I2C", "Arming", "GYRO=", "Devices", "CPU:", "rate/hz", "Total", "Uptime")):
    for cmd in cmds:
        out = c.cli(cmd)
        text = out if isinstance(out, str) else "\n".join(out or [])
        for line in text.splitlines():
            if cmd == "tasks" or any(k in line for k in keep):
                print(f"  cli {cmd}: {line}")


def cmd_pad(a):
    c = Core(a.port)
    c.send({"type": "subscribe", "state_hz": 20, "signals": SIGS})
    rows = samples(c, a.until)
    print(f"hello: firmware={c.hello.get('firmware')}; faults offered: " +
          ", ".join(f"{d['id']}.{f['id']}" for d in c.manifest["devices"] if d.get("implemented")
                    for f in d["faults"]))
    for t0 in (1, 3, 5, 8, 11, 14, a.until - 0.1):
        r = next(((t, v) for t, v in rows if t >= t0), None)
        if r:
            t, v = r
            print(f"t={t:6.2f} fdm.alt_m={fmt(v['fdm.alt_m'], 3)} baro.pressure_pa={fmt(v['baro.pressure_pa'], 1)} "
                  f"baro.temp_c={fmt(v['baro.temp_c'], 2)} fc.baro_alt_m={fmt(v['fc.baro_alt_m'], 2)}")
    late = [v["fc.baro_alt_m"] for t, v in rows if t >= a.until - 4 and v["fc.baro_alt_m"] is not None
            and not math.isnan(v["fc.baro_alt_m"])]
    if late:
        print(f"firmware altitude on the pad, t {a.until - 4:.0f}..{a.until:.0f}: min {min(late):.2f} max {max(late):.2f} m "
              f"({len(late)} samples)")
    st = c.state
    print(f"state.fc: armed={st['fc']['armed']} flags={st['fc']['arming_flags']} baro_alt_m={st['fc'].get('baro_alt_m')}")
    c.send({"type": "subscribe", "state_hz": 20, "signals": []})
    cli_lines(c, ["status", "tasks"])


def cmd_record(a):
    c = Core(a.port)
    c.send({"type": "subscribe", "state_hz": 20, "signals": SIGS})
    with open(a.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t"] + SIGS)
        samples(c, a.until, w)


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if v not in ("", "None", "nan") else math.nan) for k, v in r.items()})
    return rows


def table(rows, t0, t1, every, lag_fit=True, header=True):
    ok = [r for r in rows if t0 <= r["t"] <= t1 and not math.isnan(r["fc.baro_alt_m"])]
    if header:
        print(f"{'t':>7} {'fdm_alt':>8} {'fc_baro_alt':>11} {'diff':>6} {'pressure_pa':>11}")
    nxt = t0
    for r in ok:
        if r["t"] >= nxt - 1e-6:
            print(f"{r['t']:7.2f} {r['fdm.alt_m']:8.2f} {r['fc.baro_alt_m']:11.2f} "
                  f"{r['fc.baro_alt_m'] - r['fdm.alt_m']:6.2f} {r['baro.pressure_pa']:11.1f}")
            nxt += every
    if not ok or not lag_fit:
        return
    # the firmware's value lags (baro task ~21 Hz pressure samples, PT2 at position_baro_alt_lpf,
    # the core's MSP_ALTITUDE poll ~10 Hz): error against the FDM altitude `lag` seconds earlier
    ts = [r["t"] for r in rows]

    def fdm_at(t):
        import bisect
        i = bisect.bisect_left(ts, t)
        i = min(max(i, 1), len(ts) - 1)
        a, b = rows[i - 1], rows[i]
        if b["t"] == a["t"]:
            return b["fdm.alt_m"]
        return a["fdm.alt_m"] + (b["fdm.alt_m"] - a["fdm.alt_m"]) * (t - a["t"]) / (b["t"] - a["t"])

    best = None
    for k in range(0, 31):
        lag = k * 0.05
        e = [abs(r["fc.baro_alt_m"] - fdm_at(r["t"] - lag)) for r in ok]
        rms = math.sqrt(sum(x * x for x in e) / len(e))
        if best is None or rms < best[1]:
            best = (lag, rms, max(e))
    raw = [abs(r["fc.baro_alt_m"] - r["fdm.alt_m"]) for r in ok]
    print(f"t {t0:.1f}..{t1:.1f}: {len(ok)} samples; |fc - fdm| max {max(raw):.2f} m, rms "
          f"{math.sqrt(sum(x * x for x in raw) / len(raw)):.2f} m; against the FDM {best[0]:.2f} s earlier "
          f"(best lag): max {best[2]:.2f} m, rms {best[1]:.2f} m")


def cmd_table(a):
    table(load(a.csv), a.t_from, a.t_to, a.every)


def cmd_cli(a):
    c = Core(a.port)
    c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 1000, 1000]})
    c.wait_for(lambda m: m["type"] == "state" and not m["fc"]["armed"], 10)
    c.wait_t(c.t() + 1.0)
    print(f"t={c.t():.2f} disarmed={not c.state['fc']['armed']}")
    cli_lines(c, a.cmds or ["status"])


def cmd_freeze(a):
    """lift off with flight.py's plan, then climb and freeze the baro in the climb"""
    c = Core(a.port)
    c.send({"type": "subscribe", "state_hz": 20, "signals": SIGS})
    p = flight.Line(a.pilot)
    plan = flight.plans(1620)["liftoff"]
    # after the doublets: collective up for a climb (47..57 s), then back to about hover
    plan += [(48.0, {3: 1660}), (56.0, {3: 1606})]
    for t, ch in plan:
        p.send({"set": {str(k): v for k, v in ch.items()}, "at": t})
    rows = []
    ev = []
    for t_on, t_off in ((a.freeze_on, a.freeze_off),):
        rows += samples(c, t_on)
        k = len(c.msgs)
        c.send({"type": "fault", "device": "dps310", "kind": "freeze", "on": True})
        rows += samples(c, t_off)
        c.send({"type": "fault", "device": "dps310", "kind": "freeze", "on": False})
        rows += samples(c, a.until)
        ev = [m for m in c.msgs[k:] if m["type"] in ("event", "error")]
    for m in ev:
        print(f"  {m}")
    rr = [{"t": t, **{k: (v.get(k) if v.get(k) is not None else math.nan) for k in SIGS}} for t, v in rows]
    faults = [(t, v) for t, v in rows]
    del faults
    print(f"### climb + freeze {a.freeze_on:.0f}..{a.freeze_off:.0f} s, every 0.5 s")
    table(rr, a.freeze_on - 4, a.until, 0.5, lag_fit=False)
    fz = [r for r in rr if a.freeze_on + 0.5 <= r["t"] <= a.freeze_off and not math.isnan(r["fc.baro_alt_m"])]
    if fz:
        fc = [r["fc.baro_alt_m"] for r in fz]
        print(f"frozen window {fz[0]['t']:.2f}..{fz[-1]['t']:.2f}: fc_baro_alt {min(fc):.2f}..{max(fc):.2f} m while "
              f"fdm_alt {fz[0]['fdm.alt_m']:.2f} -> {fz[-1]['fdm.alt_m']:.2f} m")
    after = [r for r in rr if r["t"] >= a.freeze_off and not math.isnan(r["fc.baro_alt_m"])]
    rec = next((r for r in after if abs(r["fc.baro_alt_m"] - r["fdm.alt_m"]) < 0.5), None)
    if rec:
        print(f"recovered (|fc - fdm| < 0.5 m) at t={rec['t']:.2f}, {rec['t'] - a.freeze_off:.2f} s after off")
    table(rr, a.freeze_off + 3, a.until, 99, header=False)


def cmd_notwired(a):
    c = Core(a.port)
    c.wait_t(c.t() + 0.5)
    k = len(c.msgs)
    c.send({"type": "fault", "device": "dps310", "kind": "freeze", "on": True})
    c.wait_t(c.t() + 0.3)
    for m in c.msgs[k:]:
        if m["type"] in ("error", "event"):
            print(f"  {m}")
    print(f"state.faults={c.state['faults']} fc.baro_alt_m in state: {'baro_alt_m' in c.state['fc']}")


def bench(mon):
    """the machine's `bench` property over HMP, as {key: value}"""
    s = socket.create_connection(("127.0.0.1", mon))
    s.settimeout(0.3)

    def drain():
        out = b""
        while True:
            try:
                d = s.recv(65536)
            except socket.timeout:
                return out.decode(errors="replace")
            if not d:
                return out.decode(errors="replace")
            out += d
    drain()
    s.sendall(b"qom-get /machine bench\n")
    time.sleep(0.3)
    text = drain()
    s.close()
    line = next((l for l in text.replace("\r", "").split("\n") if "baro_regs=" in l), "")
    return dict(kv.split("=", 1) for kv in line.strip().strip('"').split() if "=" in kv)


def cmd_freezereset(a):
    c = Core(a.port)
    c.send({"type": "subscribe", "state_hz": 20, "signals": SIGS})
    c.wait_t(14.0)

    def show(tag):
        b = bench(a.mon)
        alt = c.state["fc"].get("baro_alt_m")
        print(f"t={c.t():6.2f} {tag:34s} baro_regs={b.get('baro_regs')} baro_reg_pa={b.get('baro_reg_pa')} "
              f"(given {b.get('baro_pa')}) baro_samples={b.get('baro_samples')} fc.baro_alt_m={alt}")
        return b

    show("live on the pad")
    c.send({"type": "fault", "device": "dps310", "kind": "freeze", "on": True})
    c.wait_t(c.t() + 0.5)
    fz = show("frozen")
    ok = True
    c.send({"type": "reset", "what": "firmware"})
    c.wait_for(lambda m: m["type"] == "event" and m.get("name") == "reboot", 30)
    c.wait_t(c.t() + 8.0)
    r1 = show("after an MCU reset (SOFT_RST)")
    ok &= r1.get("baro_regs") == fz.get("baro_regs") and int(r1["baro_samples"]) > int(fz["baro_samples"])
    c.send({"type": "power", "on": False})
    c.wait_t(c.t() + 0.5)
    c.send({"type": "power", "on": True})
    c.wait_for(lambda m: m["type"] == "event" and m.get("name") == "reboot", 30)
    c.wait_t(c.t() + 8.0)
    r2 = show("after a power cycle (power-on reset)")
    ok &= r2.get("baro_regs") == fz.get("baro_regs") and int(r2["baro_samples"]) > int(r1["baro_samples"])
    lo = [v["fc.baro_alt_m"] for t, v in samples(c, c.t() + 2.0)
          if v.get("fc.baro_alt_m") is not None and not math.isnan(v["fc.baro_alt_m"])]
    if lo:
        print(f"  firmware altitude on the frozen chip after the power cycle: {min(lo):.2f}..{max(lo):.2f} m ({len(lo)} samples)")
    c.send({"type": "fault", "device": "dps310", "kind": "freeze", "on": False})
    c.wait_t(c.t() + 1.0)
    r3 = show("freeze off")
    ok &= r3.get("baro_regs") != fz.get("baro_regs") and abs(float(r3["baro_reg_pa"]) - float(r3["baro_pa"])) < 2.0
    print(f"freeze across reset: {'PASS' if ok else 'FAIL'} (result registers kept through the MCU reset and the "
          f"power cycle, sampling on; live again after off)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("pad"); s.add_argument("--port", type=int, required=True); s.add_argument("--until", type=float, default=17.0)
    s = sub.add_parser("record"); s.add_argument("--port", type=int, required=True); s.add_argument("--out", required=True)
    s.add_argument("--until", type=float, default=62.0)
    s = sub.add_parser("table"); s.add_argument("csv"); s.add_argument("--from", dest="t_from", type=float, default=33.0)
    s.add_argument("--to", dest="t_to", type=float, default=62.0); s.add_argument("--every", type=float, default=1.0)
    s = sub.add_parser("cli"); s.add_argument("--port", type=int, required=True); s.add_argument("cmds", nargs="*")
    s = sub.add_parser("freeze"); s.add_argument("--port", type=int, required=True); s.add_argument("--pilot", type=int, required=True)
    s.add_argument("--until", type=float, default=62.0); s.add_argument("--freeze-on", type=float, default=50.0)
    s.add_argument("--freeze-off", type=float, default=54.0)
    s = sub.add_parser("notwired"); s.add_argument("--port", type=int, required=True)
    s = sub.add_parser("freezereset"); s.add_argument("--port", type=int, required=True)
    s.add_argument("--mon", type=int, required=True)
    a = ap.parse_args()
    {"pad": cmd_pad, "record": cmd_record, "table": cmd_table, "cli": cmd_cli, "freeze": cmd_freeze,
     "notwired": cmd_notwired, "freezereset": cmd_freezereset}[a.cmd](a)


if __name__ == "__main__":
    main()
