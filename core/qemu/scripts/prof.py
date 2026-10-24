#!/usr/bin/env python3
"""Host-time profile of the nexus-xr core (-M prof=on): record, show, compare.

usage: prof.py run --session S [--until 45] [--plan liftoff] [--speed-from 20] --out F.json
       prof.py show F.json [--top N]
       prof.py diff A.json B.json [--top N]

run    connects to a core that is already running (started with PROF=1 core/qemu/run.sh --session S;
       it starts nothing itself). Ports come from the session (control; MSP) and --pilot (default
       31238, run.sh's PILOT_PORT). It turns the client's state stream off, queues the stick plan of
       scripts/flight.py (--plan liftoff|spool|plane-launch|ground-takeoff: flight.plans() /
       plane_plans(), imported, with "at" times in virtual
       seconds, as flight.py sends it), asks the core to run up to --until virtual seconds and pause
       there (a `time` step), watches the virtual clock with `ping` (20 Hz), then requests one `prof`
       dump and writes it with a "meta" object (git hash, host, session, plan, the measured speed:
       host s per 10 virtual s from --speed-from to --until). Without a plan the firmware just runs
       (bare boot). With --no-dump it only measures the speed (for a prof=off core).
show   one profile: accounts by host time, hand-offs by reason and timer, wake/kick latency, main
       loop, accounted ratio, and the headline host s per 10 virtual s.
diff   two profiles side by side per virtual second (runs of different length compare), deltas and
       ratios, the top movers marked.

The profile format is the `prof` message of docs/protocol.md.
"""
import argparse, datetime, json, os, platform, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))


# ---------------------------------------------------------------------------- run

class Ctl:
    """newline-delimited JSON to the control server; messages we do not wait for are skipped"""

    def __init__(self, port, host="127.0.0.1", timeout=30.0):
        end = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection((host, port), timeout=5)
                break
            except OSError as e:
                if time.time() > end:
                    sys.exit(f"prof.py: no core on {host}:{port} ({e}); start it first, e.g. "
                             f"PROF=1 core/qemu/run.sh --session <session.json>")
                time.sleep(0.2)
        self.f = self.s.makefile("rb")

    def send(self, obj):
        self.s.sendall((json.dumps(obj) + "\n").encode())

    def wait(self, typ, timeout=30.0):
        self.s.settimeout(timeout)
        while True:
            line = self.f.readline()
            if not line:
                raise ConnectionError("control server closed the connection")
            m = json.loads(line)
            if m.get("type") == "error":
                raise RuntimeError(f"core: {m.get('what')}")
            if m.get("type") == typ:
                return m

    def ping(self):
        self.send({"type": "ping"})
        return self.wait("pong")["t"]


def git_hash():
    try:
        h = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                           text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", ROOT, "diff", "--quiet", "HEAD", "--", "core", "devices",
                                "fdm"]).returncode
        return h + ("+dirty" if dirty else "")
    except OSError:
        return "unknown"


def cmd_run(a):
    sess = json.load(open(a.session))
    ports = sess.get("ports", {})
    ctl = Ctl(a.ctl or ports.get("control", 31250))
    ctl.wait("hello")
    ctl.send({"type": "subscribe", "state_hz": a.state_hz, "osd": False, "log": False, "signals": []})
    t0 = ctl.ping()
    if a.until <= t0:
        sys.exit(f"prof.py: the core is already at t={t0:.3f} >= --until {a.until}")
    plan = None
    if a.plan:
        sys.path.insert(0, HERE)
        import flight                       # the plan lives in flight.py (plans())
        plan = dict(flight.plans(a.coll), **flight.plane_plans(a.t0))[a.plan]
        try:
            pilot = flight.Line(a.pilot)
        except OSError as e:
            sys.exit(f"prof.py: no pilot socket on 127.0.0.1:{a.pilot} ({e}); --pilot / PILOT_PORT")
        for t, step in plan:
            for line in flight.pilot_lines(t, step):
                r = pilot.send(line)
                if not r.startswith("ok"):
                    sys.exit(f"pilot: {r}")
        print(f"# plan {a.plan} (coll {a.coll} us) queued, {len(plan)} commands", flush=True)
    # run to --until and pause there (the step timer is a virtual deadline: exact to the insn)
    ctl.send({"type": "time", "mode": "step", "step_s": round(a.until - t0, 9)})
    clock = [(t0, time.monotonic())]
    last_print = t0
    while True:
        time.sleep(0.05)
        t = ctl.ping()
        clock.append((t, time.monotonic()))
        if t >= last_print + 5:
            print(f"t={t:.1f}", flush=True)
            last_print = t
        if t >= a.until - 1e-6:
            break
    w = [c for c in clock if c[0] >= a.speed_from]
    speed = None
    if len(w) >= 2 and w[-1][0] > w[0][0]:
        speed = {"host_s_per_10_virtual_s": round(10 * (w[-1][1] - w[0][1]) / (w[-1][0] - w[0][0]), 3),
                 "virtual_from": round(w[0][0], 3), "virtual_to": round(w[-1][0], 3)}
        print(f"speed: {speed['host_s_per_10_virtual_s']:.2f} host s per 10 virtual s "
              f"(virtual {w[0][0]:.1f}..{w[-1][0]:.1f} s, ping-watched)", flush=True)
    meta = {"git": git_hash(), "hostname": platform.node(), "python_platform": platform.platform(),
            "session": os.path.relpath(os.path.abspath(a.session), ROOT), "plan": a.plan,
            "until": a.until, "t_connect": t0, "speed": speed, "state_hz": a.state_hz,
            # datetime: numeric %z everywhere (time.strftime gives a localized zone name on Windows)
            "date": datetime.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %z")}
    if a.no_dump:
        out = {"type": "speed", "meta": meta}
    else:
        ctl.send({"type": "prof", "dump": True})
        out = ctl.wait("prof")
        out["meta"] = meta
    if a.out:
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)
            f.write("\n")
        print(f"wrote {a.out}")
    if a.resume:
        ctl.send({"type": "time", "mode": "run"})


# ---------------------------------------------------------------------------- show / diff

def load(path):
    d = json.load(open(path))
    if d.get("type") != "prof":
        sys.exit(f"{path}: not a prof message (type {d.get('type')!r})")
    return d


def vsec(d):
    return max(d["virt_ns"], 1) / 1e9


def fmt_n(x, digits=0):
    if x is None:
        return "-"
    if abs(x) >= 1e6 and digits == 0:
        return f"{x / 1e6:.2f}M"
    if abs(x) >= 1e4 and digits == 0:
        return f"{x / 1e3:.1f}k"
    return f"{x:.{digits}f}"


def rows(d):
    """account name -> derived numbers"""
    wall, vs = max(d["wall_ns"], 1), vsec(d)
    out = {}
    for a in d["accounts"]:
        c = a["count"]
        out[a["name"]] = {
            "kind": a["kind"], "thread": a.get("thread", ""), "count": c, "count_vs": c / vs,
            "host_ns": a["host_ns"], "self_ns": a.get("self_ns", a["host_ns"]),
            "us_vs": a["host_ns"] / 1e3 / vs, "avg_ns": a["host_ns"] / c if c else None,
            "self_avg_ns": a.get("self_ns", 0) / c if c else None,
            "pct_wall": 100 * a["host_ns"] / wall, "virt_ns": a.get("virt_ns", 0),
            "bytes": a.get("bytes"), "fds": a.get("fds"), "deadlines": a.get("deadlines", 0)}
    return out


# accounts that are a thread waiting (asleep), not work: marked "~" in show
WAITS = {"mainloop.poll", "vcpu.halt_wait", "vcpu.realtime_sleep"}


def headline(d):
    return 10 * d["wall_ns"] / max(d["virt_ns"], 1)


def describe(d, path):
    h, s, m = d.get("host", {}), d.get("session", {}), d.get("meta", {})
    lines = [f"# {path}",
             f"# host: {h.get('cpu')}; {h.get('os')}; qemu {h.get('qemu')}; {h.get('build')}",
             f"# session: {s.get('firmware')}; icount shift {s.get('icount_shift')}; idle skip "
             f"{s.get('idle_skip_mode')}; realtime {s.get('realtime')}; fdm {s.get('fdm')}"]
    if m:
        sp = m.get("speed") or {}
        lines.append(f"# run: {m.get('date')} git {m.get('git')} on {m.get('hostname')}; session "
                     f"{m.get('session')}; plan {m.get('plan')}; until {m.get('until')} s"
                     + (f"; ping-watched speed {sp['host_s_per_10_virtual_s']} host s / 10 virtual s "
                        f"(virtual {sp['virtual_from']}..{sp['virtual_to']})" if sp else ""))
    return lines


def cmd_show(a):
    d = load(a.file)
    vs, wall = vsec(d), d["wall_ns"]
    print("\n".join(describe(d, a.file)))
    print(f"\nHEADLINE: {headline(d):.2f} host s per 10 virtual s  (wall {wall / 1e9:.2f} s, "
          f"virtual {vs:.3f} s)")
    th = d.get("threads", {})
    print(f"vCPU thread: accounted {100 * d['accounted_ratio']:.1f} % of wall; "
          f"guest_exec_or_unaccounted {d['guest_exec_or_unaccounted_ns'] / 1e9:.3f} s "
          f"({100 * d['guest_exec_or_unaccounted_ns'] / max(wall, 1):.1f} % of wall)"
          + (f"; main loop thread accounted {100 * th['main']['accounted_ratio']:.1f} %" if "main" in th else ""))

    r = rows(d)
    order = sorted(r, key=lambda k: -r[k]["host_ns"])
    if a.top:
        order = order[:a.top]
    print(f"\n{'account':28} {'kind':8} {'thr':4} {'count':>9} {'/virt s':>9} {'host ms':>10} "
          f"{'us/virt s':>10} {'ns/event':>9} {'self ns/ev':>10} {'% wall':>7}")
    for k in order:
        x = r[k]
        print(f"{('~' if k in WAITS else '') + k:28.28} {x['kind']:8} {x['thread'][:4]:4} {fmt_n(x['count']):>9} "
              f"{fmt_n(x['count_vs']):>9} {x['host_ns'] / 1e6:10.1f} {x['us_vs']:10.1f} "
              f"{fmt_n(x['avg_ns']):>9} {fmt_n(x['self_avg_ns']):>10} {x['pct_wall']:7.2f}")
    print("(thr: thread the account ran on; host = inclusive of accounts nested inside, self = without;"
          " accounts on the two threads overlap in wall time; ~ = the thread asleep, waiting, not work)")

    ho = d["handoffs"]
    n = ho["count"]
    print(f"\nHAND-OFFS (vCPU slice ends): {n} = {n / vs:.0f} per virtual s")
    for k, v in sorted(ho["by_reason"].items(), key=lambda kv: -kv[1]):
        if v:
            print(f"  {k:26} {v:>10} {v / vs:>10.0f}/virt s {100 * v / max(n, 1):6.1f} %")
    bt = ho.get("by_timer", {})
    tot = sum(bt.values())
    if tot:
        print(f"  deadline reached (timer_deadline + idle_skip) by timer: {tot}")
        for k, v in sorted(bt.items(), key=lambda kv: -kv[1]):
            print(f"    {k:24} {v:>10} {v / vs:>10.0f}/virt s {100 * v / tot:6.1f} %")
    for key, label in (("wake_ns", "vCPU -> main loop wake (aio_notify -> poll returns)"),
                       ("kick_ns", "other thread -> vCPU kick (kick -> vCPU acts)")):
        w = ho.get(key, {})
        if w:
            print(f"  {label}: {w['count']} ({w['count'] / vs:.0f}/virt s); mean {w['mean']} ns, "
                  f"p50 {w['p50']} ns, p99 {w['p99']} ns, max {w['max']} ns")
    for name in ("vcpu.bql_wait", "main.bql_wait"):
        if name in r:
            x = r[name]
            print(f"  {name}: {x['count']} contended acquisitions ({x['count_vs']:.0f}/virt s), "
                  f"{x['host_ns'] / 1e6:.1f} ms blocked, avg {fmt_n(x['avg_ns'])} ns")
    ml = d["mainloop"]
    it = max(ml["iters"], 1)
    print(f"\nMAIN LOOP: {ml['iters']} iterations ({ml['iters'] / vs:.0f}/virt s); asleep in poll "
          f"{ml['poll_ns'] / 1e9:.3f} s ({100 * ml['poll_ns'] / max(wall, 1):.1f} % of wall), "
          f"dispatch {ml.get('dispatch_ns', 0) / 1e9:.3f} s; {ml['fds'] / it:.1f} fds per poll")


def cmd_diff(a):
    A, B = load(a.a), load(a.b)
    print("\n".join(describe(A, "A = " + a.a)))
    print("\n".join(describe(B, "B = " + a.b)))
    ha, hb = headline(A), headline(B)
    print(f"\nHEADLINE host s per 10 virtual s: A {ha:.2f}  B {hb:.2f}  delta {hb - ha:+.2f}  "
          f"ratio {hb / ha:.3f}")
    print(f"accounted (vCPU): A {100 * A['accounted_ratio']:.1f} %  B {100 * B['accounted_ratio']:.1f} %;"
          f" guest_exec_or_unaccounted per virtual s: A {A['guest_exec_or_unaccounted_ns'] / 1e3 / vsec(A):.0f} us"
          f"  B {B['guest_exec_or_unaccounted_ns'] / 1e3 / vsec(B):.0f} us")
    ra, rb = rows(A), rows(B)
    names = sorted(set(ra) | set(rb))
    z = {"us_vs": 0.0, "count_vs": 0.0, "avg_ns": None, "kind": "", "thread": ""}
    delta = {k: rb.get(k, z)["us_vs"] - ra.get(k, z)["us_vs"] for k in names}
    movers = set(sorted(names, key=lambda k: -abs(delta[k]))[:a.movers])
    order = sorted(names, key=lambda k: -max(ra.get(k, z)["us_vs"], rb.get(k, z)["us_vs"]))
    if a.top:
        order = order[:a.top]
    print(f"\n  {'account':28} {'kind':8} {'A us/vs':>9} {'B us/vs':>9} {'delta':>9} {'ratio':>6} "
          f"{'A n/vs':>8} {'B n/vs':>8} {'A ns/ev':>8} {'B ns/ev':>8}")
    for k in order:
        x, y = ra.get(k, z), rb.get(k, z)
        ratio = f"{y['us_vs'] / x['us_vs']:.2f}" if x["us_vs"] > 0 else "-"
        print(f"{'*' if k in movers else ' '} {k[:28]:28} {(x['kind'] or y['kind']):8} {x['us_vs']:9.1f} "
              f"{y['us_vs']:9.1f} {delta[k]:+9.1f} {ratio:>6} {fmt_n(x['count_vs']):>8} "
              f"{fmt_n(y['count_vs']):>8} {fmt_n(x['avg_ns']):>8} {fmt_n(y['avg_ns']):>8}")
    print(f"(per virtual second; * = the {a.movers} largest |delta|)")
    print("\nHAND-OFFS per virtual s:")
    va, vb = vsec(A), vsec(B)
    print(f"  {'total':26} {A['handoffs']['count'] / va:10.0f} {B['handoffs']['count'] / vb:10.0f}")
    for k in A["handoffs"]["by_reason"]:
        x, y = A["handoffs"]["by_reason"].get(k, 0) / va, B["handoffs"]["by_reason"].get(k, 0) / vb
        if x or y:
            print(f"  {k:26} {x:10.0f} {y:10.0f}")
    ta, tb = A["handoffs"].get("by_timer", {}), B["handoffs"].get("by_timer", {})
    for k in sorted(set(ta) | set(tb), key=lambda k: -(ta.get(k, 0) + tb.get(k, 0))):
        print(f"    by timer {k:17} {ta.get(k, 0) / va:10.0f} {tb.get(k, 0) / vb:10.0f}")
    for key in ("wake_ns", "kick_ns"):
        x, y = A["handoffs"].get(key, {}), B["handoffs"].get(key, {})
        print(f"  {key}: A mean/p50/p99 {x.get('mean')}/{x.get('p50')}/{x.get('p99')} ns, "
              f"B {y.get('mean')}/{y.get('p50')}/{y.get('p99')} ns")
    ma, mb = A["mainloop"], B["mainloop"]
    print(f"\nMAIN LOOP iterations per virtual s: A {ma['iters'] / va:.0f}  B {mb['iters'] / vb:.0f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="record a profile from a running core")
    r.add_argument("--session", required=True)
    r.add_argument("--until", type=float, default=45.0, help="virtual seconds; the core pauses there")
    r.add_argument("--plan", choices=["liftoff", "spool", "plane-launch", "ground-takeoff"],
                   help="flight.py stick plan (default: none)")
    r.add_argument("--t0", type=float, default=20.0, help="flight.py --t0 (plane plans)")
    r.add_argument("--coll", type=int, default=1620, help="flight.py --coll")
    r.add_argument("--speed-from", type=float, default=20.0, help="speed window start, virtual s")
    r.add_argument("--ctl", type=int, help="control port (default: the session's)")
    r.add_argument("--pilot", type=int, default=int(os.environ.get("PILOT_PORT", 31238)))
    r.add_argument("--state-hz", type=float, default=0, help="state stream for this client (default off)")
    r.add_argument("--no-dump", action="store_true", help="only measure the speed (prof=off core)")
    r.add_argument("--resume", action="store_true", help="let the core run on afterwards")
    r.add_argument("--out", help="profile JSON")
    s = sub.add_parser("show", help="print one profile")
    s.add_argument("file")
    s.add_argument("--top", type=int, default=0, help="only the N largest accounts")
    d = sub.add_parser("diff", help="compare two profiles")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--top", type=int, default=0)
    d.add_argument("--movers", type=int, default=5)
    a = ap.parse_args()
    {"run": cmd_run, "show": cmd_show, "diff": cmd_diff}[a.cmd](a)


if __name__ == "__main__":
    main()
