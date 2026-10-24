#!/usr/bin/env python3
"""Phase 4b evidence: host speed of the iNav (speedybee-f405-wing) machine (evidence/speed-inav/).

  C  first start with an empty cfg-cache (trainer bench): power-on -> the config snapshot applied
     (script, save, ACC calibration, save), host s; fills CFG_CACHE for the other stages
  B  bare boot (sessions/f405-boot.json, idle skip on unless SKIP=off, no plan) to B_UNTIL virtual s:
     power-on -> "firmware says" (host s), host s per 10 virtual s over B_FROM..B_UNTIL (ping-watched);
     PROF=1: prof=on and the profile JSON
  S  trainer boot from the cached snapshot: power-on -> "firmware says" (host s, virtual s)
  F  trainer plane-launch (prof.py run, no client polling) to 54 virtual s, RUNS times: host s per 10
     virtual s over 24..54 and the FDM log md5 (every 10 ms, to 54 s); PROF=1: prof=on + profile JSON
  Q  prof=on profile of the trainer boot from the snapshot: power-on to 3.4 virtual s (the firmware
     answers at 3.49), no plan
  L  the gate as plane_evidence.py L runs it: flight.py --plan plane-launch to 54 s, twice, FDM md5
  R  reboot after the CLI `exit` on the MSP port (trainer, from the snapshot): host s from `exit` to the
     next "firmware says" and to the first answered MSP request (configurator_handshake.py)

usage: speed_inav_evidence.py "<stages>" [label]
  QEMU=<qemu-system-arm> (default ~/flightbench-spike/qemu-sp/build/qemu-system-arm),
  OUT=<dir> (default core/qemu/evidence/speed-inav), CFG_CACHE=<dir> (persistent snapshot dir,
  default ~/flightbench-spike/sp-cache), RUNS (F, default 2), PROF=1, SKIP=on|off (B),
  MACHINE_OPTS_EXTRA=<-M options added to every run>, B_FROM/B_UNTIL=<virtual s> (B's speed
  window, default 5..20; the bare boot answers at 7.9 s, saves its config and reboots at 9.1 s, answers
  again at 12.6 s: the steady state is 14..). Ports 31280..31284.
The label names the output files (`<stage>-<label>...`). Prints the log on stdout.
"""
import datetime, hashlib, json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402

CTL, MSP, MON, MOTION, PILOT = 31280, 31281, 31282, 31283, 31284
STAGES = sys.argv[1] if len(sys.argv) > 1 else "B S F"
LABEL = sys.argv[2] if len(sys.argv) > 2 else "run"
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-sp/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/speed-inav"))
CACHE = os.environ.get("CFG_CACHE", os.path.expanduser("~/flightbench-spike/sp-cache"))
RUNS = int(os.environ.get("RUNS", "2"))
PROF = os.environ.get("PROF") == "1"
EXTRA = os.environ.get("MACHINE_OPTS_EXTRA", "")
T0, UNTIL = 20.0, 54.0
B_FROM = float(os.environ.get("B_FROM", "5"))    # B: the speed window (virtual s)
B_UNTIL = float(os.environ.get("B_UNTIL", "20"))
DATE = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))).strftime("%Y-%m-%d")
TMP = tempfile.mkdtemp(prefix="speed-inav.")
os.makedirs(OUT, exist_ok=True)


def bench(name):
    d = json.load(open(os.path.join(ROOT, "core/qemu/sessions", name)))
    d["ports"] = {"control": CTL, "msp": MSP}
    p = os.path.join(TMP, name)
    json.dump(d, open(p, "w"))
    return p


def opts(*o):
    return ",".join(x for x in (*o, EXTRA) if x)


def ports_free():
    """wait (up to 30 min) until no process listens on our ports: another bench on them would get our
    clients' commands (plans, steps) -- never talk to a core we did not start"""
    import socket
    end = time.time() + 1800
    while True:
        busy = []
        for p in (CTL, MSP, MON, MOTION, PILOT):
            with socket.socket() as so:
                if so.connect_ex(("127.0.0.1", p)) == 0:
                    busy.append(p)
        if not busy:
            return
        if time.time() > end:
            sys.exit(f"ports {busy} still in use by another process; not starting")
        time.sleep(5)


class Run:
    """run.sh --session in the background (run.sh execs QEMU: stop() kills it by PID)"""
    def __init__(self, name, session, machine_opts, prof=False):
        self.log = os.path.join(TMP, name + ".log")
        self.fdm = os.path.join(TMP, name + "-fdm.csv")
        self.session = session
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=machine_opts.replace("@FDM@", self.fdm))
        env.pop("PROF", None)
        if prof:
            env["PROF"] = "1"
        ports_free()
        self.host0 = time.time()
        self.p = subprocess.Popen([os.path.join(HERE, "../run.sh"), "--session", session],
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        time.sleep(1.5)                     # a QEMU that could not bind its ports has exited by now
        if self.p.poll() is not None:
            sys.exit(f"{name}: QEMU exited at start (ports taken?): " + open(self.log).read()[-300:])

    def lines(self, *keys):
        return [l.rstrip() for l in open(self.log) if any(k in l for k in keys) and "event beep" not in l]

    def wait_log(self, key, n=1, timeout=1800):
        """host s since power-on when the n-th line with key appeared (None: timeout)"""
        end = time.time() + timeout
        while time.time() < end:
            if len(self.lines(key)) >= n:
                return time.time() - self.host0
            if self.p.poll() is not None:
                return None
            time.sleep(0.05)
        return None

    def vt(self, line):
        return float(line.split("t=")[1].split(":")[0].split()[0])

    def stop(self):
        self.p.kill()
        self.p.wait()


def md5_cut(path, until):
    rows = [l for i, l in enumerate(open(path)) if i == 0 or float(l.split(",", 1)[0]) <= until + 1e-9]
    return hashlib.md5("".join(rows).encode()).hexdigest()[:12], len(rows) - 1


def prof_run(session, until, speed_from, plan, out):
    args = [sys.executable, os.path.join(HERE, "prof.py"), "run", "--session", session, "--until", str(until),
            "--speed-from", str(speed_from), "--pilot", str(PILOT)]
    if plan:
        args += ["--plan", plan, "--t0", str(T0)]
    args += ["--out", out] if out else ["--no-dump"]
    r = subprocess.run(args, capture_output=True, text=True)
    speed = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("speed")]
    if r.returncode or not speed:
        print(r.stdout + r.stderr)
    return speed[0] if speed else "speed: ?"


def stage_c():
    print(f"\n## C. first start, empty cfg-cache {CACHE}", flush=True)
    if os.path.isdir(CACHE) and os.listdir(CACHE):
        print("# cfg-cache already filled; move it away to measure the first start")
        return
    os.makedirs(CACHE, exist_ok=True)
    run = Run("first", bench("trainer.json"), opts(f"cfg-cache={CACHE}"))
    try:
        h1 = run.wait_log("firmware says")
        h = run.wait_log('"state":"applied"')
        l = run.lines('"state":"applied"')
        print(f"first 'firmware says' {h1:.1f} host s; config applied at t={run.vt(l[-1]):.2f} virtual s, "
              f"{h:.1f} host s after power-on" if h else "FAILED", flush=True)
    finally:
        run.stop()


def stage_b():
    skip = os.environ.get("SKIP", "on")
    print(f"\n## B. bare boot (f405-boot, idle-skip={skip}, prof={'on' if PROF else 'off'}) to {B_UNTIL:g} virtual s",
          flush=True)
    cache = tempfile.mkdtemp(prefix="bare-cache.", dir=TMP)     # blank config sector: the script at boot
    run = Run("bare", bench("f405-boot.json"), opts(f"idle-skip={skip}", f"cfg-cache={cache}"), prof=PROF)
    try:
        out = os.path.join(OUT, f"bare-boot-skip-{skip}-{LABEL}.json") if PROF else None
        sp = prof_run(run.session, B_UNTIL, B_FROM, None, out)
        h = run.wait_log("firmware says", timeout=1)
        l = run.lines("firmware says")
        print(f"firmware says at t={run.vt(l[0]):.2f} virtual s, {h:.1f} host s after power-on; {sp}"
              if l else f"no 'firmware says'; {sp}", flush=True)
    finally:
        run.stop()


def stage_s():
    print(f"\n## S. trainer boot from the cached snapshot", flush=True)
    for i in (1, 2):
        run = Run(f"snap-{i}", bench("trainer.json"), opts(f"cfg-cache={CACHE}"))
        try:
            h = run.wait_log("firmware says")
            l = run.lines("firmware says")
            print(f"run {i}: firmware says at t={run.vt(l[0]):.2f} virtual s, {h:.1f} host s after power-on "
                  f"({run.vt(l[0]) / h:.3f}x); " + "; ".join(x.split(": ", 1)[1] for x in run.lines("config:")[:2]),
                  flush=True)
        finally:
            run.stop()


def stage_f():
    print(f"\n## F. plane-launch to {UNTIL:g} s (prof.py run, no client polling), {RUNS} run(s), "
          f"prof={'on' if PROF else 'off'}", flush=True)
    sums = []
    for i in range(1, RUNS + 1):
        run = Run(f"flight-{i}", bench("trainer.json"), opts(f"cfg-cache={CACHE}", "fdm-log=@FDM@", "fdm-log-ms=10"),
                  prof=PROF)
        try:
            out = os.path.join(OUT, f"plane-launch-prof-{LABEL}.json") if PROF and i == 1 else None
            h0 = time.time()
            sp = prof_run(run.session, UNTIL, T0 + 4, "plane-launch", out)
            host = time.time() - run.host0
        finally:
            run.stop()
        md5, n = md5_cut(run.fdm, UNTIL)
        sums.append(md5)
        print(f"run {i}: FDM log {md5}, {n} rows; {sp}; whole run {host:.0f} host s ({time.time() - h0:.0f} in "
              f"prof.py)", flush=True)
    print(f"=> {len(set(sums))} distinct FDM log(s) over {len(sums)} runs", flush=True)


def stage_q():
    print("\n## Q. prof=on, trainer boot from the snapshot to 3.4 virtual s", flush=True)
    run = Run("boot-prof", bench("trainer.json"), opts(f"cfg-cache={CACHE}"), prof=True)
    try:
        print(prof_run(run.session, 3.4, 0.5, None, os.path.join(OUT, f"boot-prof-{LABEL}.json")), flush=True)
    finally:
        run.stop()


def stage_l():
    print(f"\n## L. flight.py --plan plane-launch to {UNTIL:g} s, twice (the gate)", flush=True)
    sums = []
    for i in (1, 2):
        run = Run(f"gate-{i}", bench("trainer.json"), opts(f"cfg-cache={CACHE}", "fdm-log=@FDM@", "fdm-log-ms=10"))
        try:
            h0 = time.time()
            r = subprocess.run([sys.executable, os.path.join(HERE, "flight.py"), "--plan", "plane-launch", "--until",
                                str(UNTIL), "--t0", str(T0), "--pilot", str(PILOT), "--msp", str(MSP)],
                               capture_output=True, text=True)
            if r.returncode:
                print(r.stdout, r.stderr)
            host = time.time() - h0
        finally:
            run.stop()
        md5, n = md5_cut(run.fdm, UNTIL)
        sums.append(md5)
        print(f"run {i}: FDM log {md5}, {n} rows; {host:.0f} host s", flush=True)
    print(f"=> {len(set(sums))} distinct FDM log(s) over 2 runs", flush=True)


def stage_r():
    print("\n## R. reboot after the CLI `exit` (trainer, from the snapshot)", flush=True)
    hs = os.path.join(HERE, "configurator_handshake.py")
    run = Run("reboot", bench("trainer.json"), opts(f"cfg-cache={CACHE}"))
    try:
        run.wait_log("firmware says")
        core = Core(CTL, timeout=60)
        core.cli("exit")
        t_exit, h_exit = core.t(), time.time()
        n0 = len(run.lines("firmware says"))
        end = time.time() + 300
        h_says = None
        while time.time() < end:
            if len(run.lines("firmware says")) > n0:
                h_says = time.time() - h_exit
                break
            time.sleep(0.05)
        t_says = run.vt(run.lines("firmware says")[-1]) if h_says else None
        r = subprocess.run([sys.executable, hs, f"127.0.0.1:{MSP}", "inav", "--timeout=10"], capture_output=True,
                           text=True)
        h_conn = time.time() - h_exit
        print(f"exit at t={t_exit:.2f}; firmware says again at t={t_says} after {h_says:.1f} host s; "
              f"configurator connect {'OK' if r.returncode == 0 else 'FAILED'} {h_conn:.1f} host s after exit",
              flush=True)
    finally:
        run.stop()


print(f"# speed-inav evidence, {datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))):%Y-%m-%d %H:%M} "
      f"Yerevan; {subprocess.run(['git', '-C', ROOT, 'describe', '--always', '--dirty'], capture_output=True, text=True).stdout.strip()}; "
      f"label {LABEL}; load {open('/proc/loadavg').read().split()[:3] if os.path.exists('/proc/loadavg') else '?'}")
print(f"# QEMU {QEMU}; cfg-cache {CACHE}; extra -M {EXTRA or '-'}", flush=True)
for st in STAGES.split():
    {"C": stage_c, "B": stage_b, "S": stage_s, "F": stage_f, "Q": stage_q, "L": stage_l, "R": stage_r}[st]()
