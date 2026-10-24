#!/usr/bin/env python3
"""Firmware modes, the applied diff and the environment functions (evidence/modes/).

  R  RS7 (RotorFlight 4.6, nexus-xr): `modes` after boot, state.fc.boxes with ARM on, a `modes`
     request, `diff` on request (CLI, diff all, exit), `modes` again after that reboot, the
     functions calibrate_gyro and calibrate_acc (RotorFlight's level calibration), errors
     (an unknown function, `modes` while the firmware is in the CLI), MSP fixtures
  T  trainer (iNav 9.1.0, speedybee-f405-wing), empty cfg-cache: the first start (script, diff
     all, save, the auto functions, save), its `config`/`function`/`diff`/`modes` messages; then a
     second start from the snapshot with a client that connects late (the replayed config state,
     the snapshot's diff, the modes), MSP fixtures, the functions launch, calibrate_gyro,
     calibrate_acc (standalone: poses + CLI save), a `diff` request afterwards

usage: modes_evidence.py [stages, default "R T"]
  QEMU=<qemu-system-arm>, OUT=<dir> (default core/qemu/evidence/modes). Ports: control 31280, MSP
  31281, HMP 31282, motion 31283, pilot 31284 (never the app's core). Prints the log on stdout;
  the messages go to OUT/*.json, the diffs to OUT/*.diff.txt.
"""
import datetime, json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402

CTL, MSP, MON, MOTION, PILOT = 31280, 31281, 31282, 31283, 31284
STAGES = sys.argv[1] if len(sys.argv) > 1 else "R T"
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-prof/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/modes"))
DATE = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))).strftime("%Y-%m-%d")
TMP = tempfile.mkdtemp(prefix="modes-ev.")
CACHE = os.path.join(TMP, "cfg-cache")
os.makedirs(OUT, exist_ok=True)
KEEP = ("modes", "diff", "function", "error")


def bench(name):
    d = json.load(open(os.path.join(ROOT, f"core/qemu/sessions/{name}.json")))
    d["ports"] = {"control": CTL, "msp": MSP}
    p = os.path.join(TMP, f"{name}.json")
    json.dump(d, open(p, "w"))
    return p


RUNS = []


class Run:
    """run.sh --session in the background (run.sh execs QEMU: stop() kills it by PID)"""
    def __init__(self, name, session, connect=True):
        RUNS.append(self)
        self.log = os.path.join(TMP, name + ".log")
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=f"cfg-cache={CACHE}", BENCH_STATE="")
        self.p = subprocess.Popen([os.path.join(HERE, "../run.sh"), "--session", bench(session)],
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        self.core = self.subscribed() if connect else None

    def subscribed(self):
        """a client that gets the modes after every boot (subscribe modes:true, as the app does)"""
        c = Core(CTL, timeout=60)
        c.send({"type": "subscribe", "modes": True})
        return c

    def connect(self):
        self.core = self.subscribed()
        return self.core

    def stop(self):
        self.p.terminate()
        self.p.wait()

    def lines(self, *keys):
        return [l.rstrip() for l in open(self.log) if any(k in l for k in keys)]


def short(m):
    """one line per message, the long parts abridged"""
    m = dict(m)
    if m.get("type") == "diff" and m.get("text"):
        m["text"] = f"<{len(m['text'])} bytes, {m['text'].count(chr(10))} lines>"
    if m.get("type") == "modes" and "boxes" in m:
        m["boxes"] = " ".join(f"{b['id']}:{b['name']}" for b in m["boxes"])
    return json.dumps(m, ensure_ascii=False)


def wait(c, pred, timeout, what):
    m = c.wait_for(pred, timeout)
    if m is None:
        print(f"  !! timeout waiting for {what}")
    return m


def is_(ty, **kw):
    return lambda m: m.get("type") == ty and all(m.get(k) == v for k, v in kw.items())


def is_ev(name, **kw):
    return lambda m: m.get("type") == "event" and m.get("name") == name and all(
        m.get("data", {}).get(k) == v for k, v in kw.items())


def fn_run(c, name, timeout, params=None):
    """a function to its done/failed; returns the messages"""
    out = []
    c.drain()
    c.send(dict({"type": "function", "name": name}, **({"params": params} if params else {})))
    end = time.time() + timeout
    while time.time() < end:
        m = c.wait_for(lambda m: m["type"] in ("function", "error"), end - time.time())
        if m is None:
            print(f"  !! {name}: timeout")
            break
        print("  " + short(m))
        out.append(m)
        if m["type"] == "error" or (m.get("name") == name and m.get("phase") in ("done", "failed")):
            break
    return out


def save(name, obj):
    with open(os.path.join(OUT, name), "w") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)
    print(f"  -> {name}")


def stage_r():
    print("\n## R. RS7 (RotorFlight 4.6.0, nexus-xr)")
    r = Run("rs7", "rs7")
    c = r.core
    print("hello.osd:", json.dumps(c.hello.get("osd")))
    m = wait(c, is_("modes"), 120, "modes after boot")
    print(f"t={m['t']}: " + short(m))
    save(f"modes-rs7-{DATE}.json", m)
    c.wait_t(8)                         # past RotorFlight's boot grace (else ARMSWITCH needs a toggle)
    print("state.fc disarmed: mode", c.state["fc"]["mode"], "boxes", c.state["fc"].get("boxes"))
    ch = [1500] * 16
    ch[4], ch[5] = 2000, 1000           # AECR1T23: ch5 (AUX1) high = ARM, ch6 throttle low
    c.send({"type": "rc", "ch_us": ch})
    wait(c, is_ev("armed"), 60, "armed")
    c.wait_t(c.t() + 0.5)
    print(f"t={c.t():.2f} armed: state.fc mode {c.state['fc']['mode']!r} boxes {c.state['fc'].get('boxes')}")
    c.send({"type": "diff"})
    print("  diff while armed: " + short(wait(c, is_("diff"), 30, "diff error")))
    c.send({"type": "function", "name": "calibrate_gyro"})
    print("  calibrate_gyro while armed: " + short(wait(c, is_("error"), 30, "error")))
    ch[4] = 1000
    c.send({"type": "rc", "ch_us": ch})
    wait(c, is_ev("disarmed"), 60, "disarmed")
    c.send({"type": "modes"})
    m = wait(c, is_("modes"), 60, "modes on request")
    print("  modes on request: t", m["t"], "boxes", len(m["boxes"]), "ranges", m["ranges"])
    print("request: diff")
    c.drain()
    c.send({"type": "diff"})
    d = wait(c, is_("diff"), 120, "diff")
    print("  " + short(d))
    if d and d.get("text"):
        open(os.path.join(OUT, f"diff-rs7-{DATE}.diff.txt"), "w").write(d["text"])
        print(f"  -> diff-rs7-{DATE}.diff.txt")
    rb = wait(c, is_ev("reboot"), 60, "reboot after exit")
    print("  reboot after exit: t", rb and rb["t"])
    m = wait(c, is_("modes"), 120, "modes after the reboot")
    print("  modes after the reboot: t", m and m["t"])
    c.wait_t(c.t() + 2)
    print("functions:")
    fn_run(c, "calibrate_gyro", 300)
    c.wait_t(c.t() + 1)
    fn_run(c, "calibrate_acc", 300)
    c.send({"type": "function", "name": "launch"})
    print("  launch on the heli: " + short(wait(c, is_("error"), 30, "error")))
    print("modes while the firmware is in the CLI:")
    c.cli("status", quiet_s=0.5)
    c.send({"type": "modes"})
    print("  " + short(wait(c, is_("modes"), 30, "modes error")))
    c.send({"type": "diff"})
    print("  diff: " + short(wait(c, is_("diff"), 30, "diff error")))
    c.send({"type": "cli", "text": "exit"})
    wait(c, is_ev("reboot"), 60, "reboot")
    wait(c, is_("modes"), 120, "modes")
    subprocess.run([sys.executable, os.path.join(HERE, "msp_capture.py"), str(MSP),
                    os.path.join(ROOT, "core/qemu/tests/fixtures/msp-rotorflight-rs7.json")],
                   stdout=subprocess.DEVNULL)
    print("  MSP fixture -> core/qemu/tests/fixtures/msp-rotorflight-rs7.json")
    msgs = [x for x in c.msgs if x["type"] in KEEP or (x["type"] == "event" and x["name"] in ("config", "reboot"))]
    save(f"messages-rs7-{DATE}.json", msgs)
    r.stop()
    print("core log (functions, modes, diff):")
    for l in r.lines("fbctl t=") :
        if any(k in l for k in ("function ", "modes:", "diff", "calibrate_", "event reboot")):
            print("  " + l)


def stage_t():
    print("\n## T. trainer (iNav 9.1.0, speedybee-f405-wing), empty cfg-cache")
    h0 = time.time()
    r = Run("trainer-first", "trainer")
    c = r.core
    print("hello.osd:", json.dumps(c.hello.get("osd")))
    end = time.time() + 1800
    while time.time() < end:
        m = c.wait_for(lambda m: m["type"] in ("function", "diff", "modes") or
                       (m["type"] == "event" and m["name"] in ("config", "reboot")), end - time.time())
        if m is None:
            print("  !! timeout")
            break
        print("  " + short(m))
        if m["type"] == "diff" and m.get("text"):
            open(os.path.join(OUT, f"diff-trainer-{DATE}.diff.txt"), "w").write(m["text"])
        if m["type"] == "event" and m["name"] == "config" and m["data"]["state"] in ("applied", "failed"):
            break
    m = wait(c, is_("modes"), 300, "modes after the last reboot")
    if m:
        print("  " + short(m))
        save(f"modes-trainer-{DATE}.json", m)
    print(f"  first start: {time.time() - h0:.0f} host s; state.osd {json.dumps(c.state.get('osd'))}")
    msgs = [x for x in c.msgs if x["type"] in KEEP or (x["type"] == "event" and x["name"] in ("config", "reboot"))]
    save(f"messages-trainer-first-{DATE}.json", msgs)
    r.stop()
    for l in r.lines("fbctl t="):
        if any(k in l for k in ("config:", "function ", "diff", "calibrate_acc", "modes:", "event config")):
            print("  " + l)

    print("\n### second start from the snapshot, a client connecting late (t > 8)")
    r = Run("trainer-second", "trainer", connect=False)
    while True:
        time.sleep(2)
        c0 = Core(CTL, timeout=60)
        c0.wait_t(0.1, 60)
        if c0.t() >= 8:
            break
        c0.sock.close()
    c = c0
    for x in c.msgs[:12]:
        if x["type"] not in ("hello", "manifest", "signals", "log"):
            print("  " + short(x))
    # nobody was subscribed since the boot, so no modes were read: subscribing reads them now
    c.send({"type": "subscribe", "modes": True})
    m = wait(c, is_("modes"), 120, "modes on subscribe")
    print(f"  subscribe modes:true at t={c.t():.2f}: " + (short(m) if m else "-"))
    save(f"replay-trainer-{DATE}.json", [x for x in c.msgs if x.get("replay") or (x.get("data") or {}).get("replay")])
    subprocess.run([sys.executable, os.path.join(HERE, "msp_capture.py"), str(MSP),
                    os.path.join(ROOT, "core/qemu/tests/fixtures/msp-inav-trainer.json")],
                   stdout=subprocess.DEVNULL)
    print("  MSP fixture -> core/qemu/tests/fixtures/msp-inav-trainer.json")
    c.wait_t(c.t() + 1)
    print("functions:")
    fn_run(c, "launch", 120, {"speed_kmh": 50})
    c.send({"type": "reset", "what": "aircraft"})
    c.wait_t(c.t() + 3)
    fn_run(c, "calibrate_gyro", 600)
    c.wait_t(c.t() + 1)
    fn_run(c, "calibrate_acc", 1200)
    wait(c, is_("modes"), 300, "modes after the save's reboot")
    c.wait_t(c.t() + 1)
    print("request: diff (stale after the calibration's save)")
    c.drain()
    c.send({"type": "diff", "refresh": False})
    print("  cached: " + short(wait(c, is_("diff"), 30, "diff")))
    for _ in range(30):                 # refused while the firmware's boot calibration runs
        c.drain()
        c.send({"type": "diff"})
        d = wait(c, lambda m: m["type"] == "diff" and (m.get("source") == "request" or m.get("error")), 300, "diff")
        if not d or d.get("error") != "calibrating":
            break
        print("  refused: calibrating (retry in 1 s)")
        c.wait_t(c.t() + 1)
    print("  " + (short(d) if d else "-"))
    if d and d.get("text"):
        open(os.path.join(OUT, f"diff-trainer-request-{DATE}.diff.txt"), "w").write(d["text"])
    msgs = [x for x in c.msgs if x["type"] in KEEP or (x["type"] == "event" and x["name"] in ("config", "reboot"))]
    save(f"messages-trainer-second-{DATE}.json", msgs)
    r.stop()
    for l in r.lines("fbctl t="):
        if any(k in l for k in ("config:", "function ", "diff", "calibrate_", "modes:", "client")):
            print("  " + l)


print(f"# modes evidence, {DATE}; {subprocess.run(['git', '-C', HERE, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()}; QEMU {QEMU}")
try:
    if "R" in STAGES:
        stage_r()
    if "T" in STAGES:
        stage_t()
finally:
    for run in RUNS:
        if run.p.poll() is None:
            run.stop()
