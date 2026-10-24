#!/usr/bin/env python3
"""The firmware's config as bench state (-M bench-state=; evidence/bench-state/).

The trainer (iNav 9.1.0, speedybee-f405-wing) without its GPS, an empty cfg-cache and no bench
state, then with the same bench-state file throughout:
  S  first start: the script, the automatic functions; the state file after the saves
  M  the configurator's Modes tab "Save" replayed over the MSP port (tabs/auxiliary.js: every range
     by MSP_SET_MODE_RANGE in the UI's order -- grouped by mode, so slots move --, then
     MSP_EEPROM_WRITE, no reboot) adding touching and overlapping ranges on one channel; the
     `modes` message and the state file after it
  C  the CLI: a host tool's '#' over the MSP port -> state.fc.cli, `cli` events; {"type":"cli",
     "exit":true} -> the reboot, cli closed; the modes still there
  R  restart: restored from the bench state, no function run, the modes of M
  G  the GPS wired (the script's GPS lines): restored + the delta only, no function run
  X  {"type":"config","reset":"first_start"}: the whole first start, the modes of the script
  F  the file itself (after S): copies of the state -- intact, truncated, metadata not JSON, another
     firmware's, the sector changed under its metadata, no metadata -- each at a start: what the
     core logs, whether it restores it

usage: bench_state_evidence.py [stages, default "S M C R G X"]
  QEMU=<qemu-system-arm>, OUT=<dir> (default core/qemu/evidence/bench-state), PORT_BASE (default
  31270: control, MSP, HMP, motion, pilot). Prints the log on stdout; the state metadata (.state.json; the sector by its sha256) goes to OUT.
"""
import datetime, json, os, shutil, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402
sys.path.insert(0, HERE)
import winproc  # noqa: E402
from configurator_handshake import v2, Reader  # noqa: E402

PORT0 = int(os.environ.get("PORT_BASE", "31270"))
CTL, MSP, MON, MOTION, PILOT = range(PORT0, PORT0 + 5)
STAGES = sys.argv[1] if len(sys.argv) > 1 else "S M C R G X"
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-prof/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/bench-state"))
TMP = tempfile.mkdtemp(prefix="bench-state-ev.")
CACHE = os.path.join(TMP, "cfg-cache")
STATE = os.path.join(TMP, "bench", "trainer.state.bin")
os.makedirs(OUT, exist_ok=True)
MSP_MODE_RANGES, MSP_SET_MODE_RANGE, MSP_EEPROM_WRITE = 34, 35, 250


def bench(gps):
    d = json.load(open(os.path.join(ROOT, "core/qemu/sessions/trainer.json")))
    d["ports"] = {"control": CTL, "msp": MSP}
    if not gps:
        d["wiring"] = [w for w in d["wiring"] if w["device"] != "ublox"]
    p = os.path.join(TMP, "trainer.json")         # one bench, its wiring changed in G
    json.dump(d, open(p, "w"))
    return p


class Run:
    """run.sh --session in the background with the bench state; the app's client subscribed"""
    def __init__(self, name, gps=False, state=STATE):
        self.log = os.path.join(TMP, name + ".log")
        self.state = state
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=f"cfg-cache={CACHE}", BENCH_STATE=state)
        self.p = winproc.popen_run_sh([os.path.join(HERE, "../run.sh"), "--session", bench(gps)],
                                      stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        self.core = Core(CTL, timeout=60)
        self.core.send({"type": "subscribe", "modes": True, "state_hz": 5})
        self.msgs = []

    def wait(self, pred, timeout, what):
        end = time.time() + timeout
        while time.time() < end:
            m = self.core.wait_for(lambda m: m["type"] != "state", end - time.time())
            if m is None:
                break
            self.msgs.append(m)
            if pred(m):
                return m
        print(f"  !! timeout waiting for {what}")
        return None

    def seen(self, pred):
        return [m for m in self.msgs if pred(m)]

    def lines(self, *keys):
        return [l.rstrip() for l in open(self.log) if any(k in l for k in keys)]

    def stop(self):
        winproc.stop(self.p, self.state)   # in QEMU's command line (bench-state=)


def cfg(state):
    return lambda m: m["type"] == "event" and m.get("name") == "config" and m["data"].get("state") == state


def ev(name):
    return lambda m: m["type"] == "event" and m.get("name") == name


def short(m):
    if m["type"] == "event":
        return f"event {m['name']} {json.dumps(m.get('data'), ensure_ascii=False)}"
    if m["type"] == "function":
        return f"function {m.get('name')} {m.get('phase')}"
    if m["type"] == "modes":
        return "modes " + ranges_str(m)
    return json.dumps(m)[:160]


def ranges_str(m):
    names = {b["id"]: b["name"] for b in m.get("boxes", [])}
    return "; ".join(f"slot {r['slot']} {names.get(r['box'], r['box'])} ch{r['rc_channel']} "
                     f"{r['start_us']}-{r['end_us']}" for r in m.get("ranges", []))


def state_meta():
    j = STATE[:-4] + ".json"
    if not os.path.exists(j):
        return None
    return json.load(open(j))


def show_state(tag):
    d = state_meta()
    if not d:
        print(f"  state file: none ({tag})")
        return None
    print(f"  state file: {os.path.getsize(STATE)} B, sector {d['sector_sha256'][:12]}, firmware "
          f"{d['firmware'][:12]}, saves {d['saves']}, functions {d['functions']}, script "
          f"{d['script'].count(chr(10))} lines, diff {'%d B' % len(d['diff']) if d.get('diff') else '-'}")
    shutil.copy(STATE[:-4] + ".json", os.path.join(OUT, f"{tag}.state.json"))
    return d


def msp(s, r, cmd, payload=b"", timeout=5):
    s.sendall(v2(cmd, payload))
    end = time.time() + timeout
    while time.time() < end:
        f = r.frame(end)
        if f and f[2] == cmd:
            return f[3]
    raise TimeoutError(f"MSP {cmd}: no reply")


def up(run, n=1, timeout=900):
    """the firmware answers (n-th boot of this run), its boot calibration over"""
    end = time.time() + timeout
    while time.time() < end and len(run.lines("firmware says")) < n:
        time.sleep(0.5)
    run.core.wait_t(run.core.t() + 3, timeout=120)


def stage_s():
    print("\n## S. first start (empty cfg-cache, no bench state; the trainer without its GPS)", flush=True)
    if os.path.exists(STATE):
        os.unlink(STATE)
    run = Run("s-first")
    try:
        run.wait(lambda m: cfg("applied")(m) and "functions" in m["data"] or cfg("failed")(m), 900,
                 "the first start applied")
        up(run, 3)
        time.sleep(2)
        for m in run.msgs:
            if m["type"] == "event" and m["name"] in ("config", "cli") or \
                    m["type"] == "function" and m.get("phase") in ("start", "done", "failed"):
                print("  " + short(m))
        print("  " + "\n  ".join(run.lines("bench state:", "firmware saved its config")))
        show_state("s-first")
    finally:
        run.stop()


# touching (ANGLE 900-1300 | HORIZON 1300-1700) and overlapping (MANUAL 1500-2100 over HORIZON)
# ranges on one aux channel, after the script's own
EXTRA = [("ANGLE", 900, 1300), ("HORIZON", 1300, 1700), ("MANUAL", 1500, 2100)]
AUX = 3                             # aux 4 = rc channel 8


def stage_m():
    print("\n## M. the configurator's Modes tab Save, replayed (every slot, then MSP_EEPROM_WRITE)", flush=True)
    run = Run("m-modes")
    try:
        m0 = run.wait(lambda m: m["type"] == "modes" and "ranges" in m, 900, "modes")
        up(run)
        ids = {b["name"]: b["id"] for b in m0["boxes"]}
        print(f"  before: {ranges_str(m0)}")
        s = socket.create_connection(("127.0.0.1", MSP), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        r = Reader(s, time.time(), False)
        raw = msp(s, r, MSP_MODE_RANGES)
        slots = [raw[i:i + 4] for i in range(0, len(raw), 4)]
        used = [bytes(x) for x in slots if x[2] < x[3]]
        used += [bytes([ids[n], AUX, (a - 900) // 25, (b - 900) // 25]) for n, a, b in EXTRA]
        # tabs/auxiliary.js Save: the ranges as the UI lists them -- mode by mode in the firmware's
        # box order, each mode's ranges in their order --, then empty ranges up to the count read
        order = [b["id"] for b in m0["boxes"]]
        new = sorted(used, key=lambda x: order.index(x[0]))
        new += [bytes([0, 0, 0, 0])] * (len(slots) - len(new))
        saves0 = (state_meta() or {}).get("saves")
        t0 = run.core.t()
        for i, x in enumerate(new):
            msp(s, r, MSP_SET_MODE_RANGE, bytes([i]) + x)
        msp(s, r, MSP_EEPROM_WRITE)
        t1 = run.core.t()
        print(f"  sent {len(new)} MSP_SET_MODE_RANGE + MSP_EEPROM_WRITE, t {t0:.2f} -> {t1:.2f} s")
        want = {(ids[n], AUX, a, b) for n, a, b in EXTRA}
        m1 = run.wait(lambda m: m["type"] == "modes" and want <= {(x["box"], x["channel"], x["start_us"], x["end_us"])
                                                                   for x in m.get("ranges", [])}, 60, "the new modes")
        if m1:
            print(f"  modes at t={m1['t']:.2f} ({m1['t'] - t1:.2f} s after the write's reply): {ranges_str(m1)}")
        rb = msp(s, r, MSP_MODE_RANGES)
        print(f"  firmware MSP_MODE_RANGES now: {' '.join(rb[i:i + 4].hex() for i in range(0, 4 * len(used), 4))}")
        fw = [x for x in (rb[i:i + 4] for i in range(0, len(rb), 4)) if x[2] < x[3]]
        got = [(x["box"], x["channel"], (x["start_us"] - 900) // 25, (x["end_us"] - 900) // 25) for x in m1["ranges"]] if m1 else []
        same = got == [tuple(x) for x in fw]
        print(f"  the modes message {'matches' if same else 'DIFFERS from'} the firmware's slots")
        s.close()
        time.sleep(3)
        print("  " + "\n  ".join(run.lines("firmware saved its config", "bench state: the config")[-2:]))
        d = show_state("m-modes")
        print(f"  M: {'PASS' if m1 and same and d and d['saves'] != saves0 else 'FAIL'}")
    finally:
        run.stop()


def stage_c():
    print("\n## C. the CLI: a host tool types '#', the app sees it and sends exit", flush=True)
    run = Run("c-cli")
    try:
        run.wait(lambda m: m["type"] == "modes" and "ranges" in m, 900, "modes")
        up(run)
        s = socket.create_connection(("127.0.0.1", MSP), timeout=5)
        s.sendall(b"#")
        o = run.wait(ev("cli"), 60, "cli open")
        st = run.core.wait_for(lambda m: m["type"] == "state" and m["fc"].get("cli"), 30)
        print(f"  {short(o) if o else '-'}; state.fc.cli {st['fc']['cli'] if st else '-'} at t={st['t'] if st else 0:.2f}")
        run.core.send({"type": "cli", "exit": True})
        c = run.wait(ev("cli"), 60, "cli closed")
        print(f"  {short(c) if c else '-'}")
        m = run.wait(lambda m: m["type"] == "modes" and "ranges" in m, 120, "modes after the reboot")
        print(f"  modes after the reboot: {ranges_str(m) if m else '-'}")
        s.close()
        run.core.send({"type": "cli", "exit": True})
        e = run.wait(lambda m: m["type"] == "error", 10, "the error")
        print(f"  exit again: {e.get('message') or e if e else '-'}")
        ok = o and o["data"] == {"open": True, "by": "client", "via": "msp"} and st and c and not c["data"]["open"]
        print(f"  C: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


def restart(name, gps):
    run = Run(name, gps)
    try:
        m = run.wait(lambda m: m["type"] == "modes" and "ranges" in m, 900, "modes")
        up(run)
        end = time.time() + 60                  # the delta's save + reboot, the calibration check
        while time.time() < end and not run.lines("automatic functions' results are in the sector",
                                                  "run now", "calibrations are not checked"):
            time.sleep(0.5)
        time.sleep(3)
        m = [x for x in run.msgs + list(drain(run)) if x["type"] == "modes" and "ranges" in x][-1:] or [m]
        for x in run.msgs:
            if x["type"] == "event" and x["name"] in ("config", "cli") or \
                    x["type"] == "function" and x.get("phase") in ("start", "done", "failed"):
                print("  " + short(x))
        print("  " + "\n  ".join(run.lines("config: bench state", "config: delta", "bench state:")))
        print(f"  modes: {ranges_str(m[0]) if m[0] else '-'}")
        return run, m[0]
    finally:
        run.stop()


def drain(run):
    while True:
        m = run.core.wait_for(lambda m: m["type"] != "state", 0.2)
        if m is None:
            return
        run.msgs.append(m)
        yield m


def has_extra(m):
    return m and sum(1 for x in m.get("ranges", []) if x["channel"] == AUX) == len(EXTRA)


def stage_r():
    print("\n## R. restart: the bench state, no calibration", flush=True)
    run, m = restart("r-restart", False)
    fns = run.seen(lambda x: x["type"] == "function")
    ok = run.seen(lambda x: cfg("restored")(x) and x["data"].get("source") == "bench") and not fns and has_extra(m)
    show_state("r-restart")
    print(f"  R: {'PASS' if ok else 'FAIL'}")


def stage_g():
    print("\n## G. the GPS wired: the bench state + the script's delta", flush=True)
    run, m = restart("g-gps", True)
    ap = run.seen(cfg("applying"))
    fns = run.seen(lambda x: x["type"] == "function")
    d = show_state("g-gps")
    ok = ap and ap[0]["data"].get("delta") and ap[0]["data"]["lines"] == 4 and not fns and has_extra(m) and \
        d and "feature GPS" in d["script"]
    print(f"  G: {'PASS' if ok else 'FAIL'}")


def stage_x():
    print("\n## X. config reset first_start", flush=True)
    run = Run("x-reset", True)
    try:
        run.wait(lambda m: m["type"] == "modes" and "ranges" in m, 900, "modes")
        up(run)
        time.sleep(5)
        run.msgs.clear()
        run.core.send({"type": "config", "reset": "first_start"})
        run.wait(lambda m: cfg("applied")(m) and "functions" in m["data"] or cfg("failed")(m), 900, "applied")
        up(run, len(run.lines("firmware says")) + 1)
        run.core.send({"type": "modes"})           # (up() consumed the subscription's)
        m = run.wait(lambda m: m["type"] == "modes" and "ranges" in m, 120, "modes")
        for x in run.msgs:
            if x["type"] == "event" and x["name"] in ("config",) or \
                    x["type"] == "function" and x.get("phase") in ("start", "done", "failed"):
                print("  " + short(x))
        print(f"  modes: {ranges_str(m) if m else '-'}")
        time.sleep(2)
        d = show_state("x-reset")
        fns = {x["name"] for x in run.seen(lambda x: x["type"] == "function" and x.get("phase") == "done")}
        ok = run.seen(cfg("reset")) and fns and m and not has_extra(m) and d and d["functions"]
        print(f"  X: {'PASS' if ok else 'FAIL'} (functions {sorted(fns)})")
    finally:
        run.stop()


def stage_f():
    print("\n## F. the state file: intact, damaged, another firmware's", flush=True)
    base = open(STATE, "rb").read()
    meta = json.load(open(STATE[:-4] + ".json"))
    d = os.path.join(TMP, "f")
    os.makedirs(d, exist_ok=True)
    cases = [("intact", base, meta, True),
             ("truncated", base[:4096], meta, False),
             ("not-json", base, "{\"format\": 1, \"machine\"", False),
             ("other-firmware", base, dict(meta, firmware="0" * 64), False),
             ("sector-changed", base[:100] + bytes([base[100] ^ 1]) + base[101:], meta, True),
             ("no-metadata", base, None, False)]
    ok = True
    for name, sec, js, restore in cases:
        f = os.path.join(d, name + ".state.bin")
        open(f, "wb").write(sec)
        if js is not None:
            open(f[:-4] + ".json", "w").write(js if isinstance(js, str) else json.dumps(js))
        run = Run("f-" + name, state=f)
        try:
            end = time.time() + 60
            while time.time() < end and not run.lines("config: blank config sector"):
                time.sleep(0.2)
            got = run.lines("bench state:", "config: blank config sector")
        finally:
            run.stop()
        restored = any("the bench state written back" in l for l in got)
        good = restored == restore
        ok &= good
        print(f"  {name}: {'restored' if restored else 'not restored'} ({'as expected' if good else 'WRONG'})")
        print("    " + "\n    ".join(l.split(": ", 1)[1] for l in got[:3]))
    print(f"  F: {'PASS' if ok else 'FAIL'}")


print(f"# bench-state evidence, {datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))):%Y-%m-%d %H:%M} "
      f"Yerevan; {subprocess.run(['git', '-C', ROOT, 'describe', '--always', '--dirty'], capture_output=True, text=True).stdout.strip()}")
print(f"# QEMU {QEMU}; trainer (ports {CTL}/{MSP}); bench state {STATE}", flush=True)
for st in STAGES.split():
    {"S": stage_s, "M": stage_m, "C": stage_c, "R": stage_r, "G": stage_g, "X": stage_x, "F": stage_f}[st]()
