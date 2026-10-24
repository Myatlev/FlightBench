#!/usr/bin/env python3
"""Phase 3 evidence: the trainer plane on stock iNav 9.1.0, speedybee-f405-wing (evidence/plane/).

  C  first start with an empty cfg-cache: the CLI script, save, the 6-position ACC calibration, save;
     then the control server's arming flags and the CLI `status` / `get acc` of the saved config
  L  flight.py --plan plane-launch, twice, from the cached snapshot: FDM log (every 10 ms) to
     T0 + 34 s, md5 per run (determinism); flight_report.py --plane of run 1
  G  flight.py --plan ground-takeoff (-M env-scenario=ground-takeoff): the runway take-off and the
     receiver's link lost in flight (iNav's failsafe); flight_report.py --plane
  P  prof=on, plane-launch to T0 + 34 s: prof.py show; the flight's speed
  M  the iNav Configurator's connect on the MSP port (configurator_handshake.py, MSP v2, its request
     chain, 3 s retry, 10 s window) once the firmware answers: `host:port` connects, `tcp://host:port`
     (the RotorFlight form) fails to open as in the real configurator; then CLI `exit` and how long
     until a connect succeeds again (iNav's boot)
  D  what the iNav Configurator 9.1.1 sees after the connect (configurator_handshake.py --tabs): the
     "Default values" dialog's applied_defaults, the Status tab's mixer banner (appliedMixerPreset),
     the Mixer tab's rules, the Outputs tab's servo rates and output switch (PWM_OUTPUT_ENABLE)

  E  the configurator stays connected through a flight: configurator_handshake.py --tabs --poll (its
     Setup tab + status bar polling) while plane-launch runs to T0 + 40 s and the app's client is
     subscribed to the modes; E_ARGS="--reboot=<host s>" adds a tab's "Save and Reboot" and the
     configurator's reconnect (10 host s window), "--hex" the bytes; the poller's log goes to OUT

usage: plane_evidence.py [stages, default "C L G P", also M, D, E]
  QEMU=<qemu-system-arm>, OUT=<dir> (default core/qemu/evidence/plane; the CSVs and profiles go
  there), T0=<virtual s of the plans' first step> (default 20), CFG_CACHE=<dir> (a cfg-cache with the
  snapshot to fly from, instead of an empty one: stage L without C). Ports: control 31280, MSP 31281,
  HMP 31282, motion 31283, pilot 31284 (never the app's core). Prints the log on stdout.
"""
import datetime, shutil, hashlib, json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402
sys.path.insert(0, HERE)
import winproc  # noqa: E402

PORT0 = int(os.environ.get("PORT_BASE", "31280"))   # five ports from here
CTL, MSP, MON, MOTION, PILOT = range(PORT0, PORT0 + 5)
STAGES = sys.argv[1] if len(sys.argv) > 1 else "C L G P"
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-prof/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/plane"))
T0 = float(os.environ.get("T0", "20"))
UNTIL = T0 + 34.0                   # plane-launch: 14 s launch + 20 s after it
DATE = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))).strftime("%Y-%m-%d")
TMP = tempfile.mkdtemp(prefix="plane-ev.")
CACHE = os.environ.get("CFG_CACHE") or os.path.join(TMP, "cfg-cache")   # CFG_CACHE: an existing snapshot dir
os.makedirs(OUT, exist_ok=True)


def bench():
    d = json.load(open(os.path.join(ROOT, "core/qemu/sessions/trainer.json")))
    d["ports"] = {"control": CTL, "msp": MSP}
    p = os.path.join(TMP, "trainer.json")
    json.dump(d, open(p, "w"))
    return p


class Run:
    """run.sh --session in the background (run.sh execs QEMU: stop() kills it by PID)"""
    def __init__(self, name, opts="", prof=False):
        self.name = name
        self.log = os.path.join(TMP, name + ".log")
        self.fdm = os.path.join(TMP, name + "-fdm.csv")
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=f"cfg-cache={CACHE},fdm-log={self.fdm},fdm-log-ms=10" + (f",{opts}" if opts else ""))
        if prof:
            env["PROF"] = "1"
        self.p = winproc.popen_run_sh([os.path.join(HERE, "../run.sh"), "--session", bench()],
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        self.core = Core(CTL, timeout=60)
        self.host0 = time.time()

    def lines(self, *keys):
        return [l.rstrip() for l in open(self.log) if any(k in l for k in keys) and "event beep" not in l]

    def wait_log(self, key, n=1, timeout=1800):
        end = time.time() + timeout
        while time.time() < end:
            if len(self.lines(key)) >= n:
                return True
            time.sleep(1)
        return False

    def stop(self):
        winproc.stop(self.p, self.fdm)   # the FDM log's path is unique to this run's QEMU


def fly(run, plan, until, quiet=False):
    """flight.py plan until the virtual time (quiet: no MSP polling of its own); returns host seconds"""
    t0 = time.time()
    r = subprocess.run([sys.executable, os.path.join(HERE, "flight.py"), "--plan", plan, "--until", str(until),
                        "--t0", str(T0), "--pilot", str(PILOT), "--msp", str(MSP)] + (["--quiet"] if quiet else []),
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
    return time.time() - t0


def cut(path, until, out):
    rows = [l for i, l in enumerate(open(path)) if i == 0 or float(l.split(",", 1)[0]) <= until + 1e-9]
    open(out, "w").writelines(rows)
    return hashlib.md5("".join(rows).encode()).hexdigest()[:12], len(rows) - 1


def motor_delay_s():
    """nav_fw_launch_motor_delay from the trainer's script (iNav's default 500 ms without it)"""
    for l in open(os.path.join(ROOT, "models/trainer/inav.cli")):
        w = l.split()
        if w[:3] == ["set", "nav_fw_launch_motor_delay", "="]:
            return int(w[3]) / 1000
    return 0.5


def report(csv, *args):
    r = subprocess.run([sys.executable, os.path.join(HERE, "flight_report.py"), "--plane", csv, *args,
                        "--motor-delay", str(motor_delay_s())],
                       capture_output=True, text=True)
    print(r.stdout + r.stderr, flush=True)


def ensure_cache():
    if not (os.path.isdir(CACHE) and os.listdir(CACHE)):
        stage_c(quiet=True)


def stage_c(quiet=False):
    os.makedirs(CACHE, exist_ok=True)
    if not quiet:
        print("\n## C. first start, empty cfg-cache: script, save, ACC calibration, save", flush=True)
    run = Run("cal")
    try:
        ok = run.wait_log('"state":"applied"')
        run.wait_log("firmware says", n=3)          # the third boot: after the calibration's save
        t_up = float(run.lines("firmware says")[-1].split("t=")[1].split(":")[0])
        run.core.send({"type": "subscribe", "state_hz": 2})
        run.core.wait_t(t_up + 3.0)
        st = run.core.state
        host = time.time() - run.host0
        if quiet:
            print(f"# cfg-cache filled ({'ok' if ok else 'FAILED'}, {host:.0f} host s)", flush=True)
            return
        for l in run.lines("config", "calibration", "snapshot", "firmware says", "event pose", "reboot"):
            print(l)
        print(f"# to t = {st['t']:.1f} virtual s in {host:.0f} host s")
        print(f"t={st['t']:.2f} control server (MSP2_INAV_STATUS): armed {st['fc']['armed']}, mode "
              f"{st['fc']['mode']}, arming_flags {st['fc']['arming_flags']}, attitude {st['fc'].get('attitude_deg')}")
        for cmd in ("status", "get acc"):
            for line in run.core.cli(cmd):
                print("cli| " + line)
        print("cfg-cache: " + " ".join(f"{f} ({os.path.getsize(os.path.join(CACHE, f))} B)" for f in os.listdir(CACHE)))
    finally:
        run.stop()


def stage_l():
    ensure_cache()
    print(f"\n## L. plane-launch (T0 {T0:g}), twice from the cached snapshot, to t = {UNTIL:g} s", flush=True)
    sums = []
    for i in (1, 2):
        run = Run(f"launch-{i}")
        try:
            host = fly(run, "plane-launch", UNTIL)
        finally:
            run.stop()
        out = os.path.join(OUT, f"plane-launch-{DATE}-run{i}.csv")
        md5, n = cut(run.fdm, UNTIL, out)
        sums.append(md5)
        print(f"run {i}: FDM log {md5}, {n} rows; {host:.0f} host s for {UNTIL:g} virtual s "
              f"({UNTIL / host:.3f}x); " + "; ".join(l.split(": ", 1)[1] for l in run.lines("launch:", "event launch")))
    print(f"=> {len(set(sums))} distinct FDM log(s) over 2 runs", flush=True)
    if sums[0] == sums[1]:
        os.remove(os.path.join(OUT, f"plane-launch-{DATE}-run2.csv"))
    report(os.path.join(OUT, f"plane-launch-{DATE}-run1.csv"), "--launch-end", str(T0 + 14),
           "--from", str(T0 - 1), "--every", "0.5")


def stage_g():
    ensure_cache()
    until = T0 + 30.0
    print(f"\n## G. ground-takeoff (env-scenario=ground-takeoff), link lost at T0 + 14 .. 18, to t = {until:g} s",
          flush=True)
    run = Run("ground", "env-scenario=ground-takeoff")
    try:
        host = fly(run, "ground-takeoff", until)
    finally:
        run.stop()
    out = os.path.join(OUT, f"ground-takeoff-{DATE}.csv")
    md5, n = cut(run.fdm, until, out)
    print(f"FDM log {md5}, {n} rows; {host:.0f} host s; " +
          "; ".join(l.split(": ", 1)[1] for l in run.lines("event failsafe", "event fault", "link ", "event crash")))
    report(out, "--from", str(T0 - 1), "--every", "0.5")


def stage_p():
    ensure_cache()
    print(f"\n## P. prof=on, plane-launch to t = {UNTIL:g} s", flush=True)
    run = Run("prof", prof=True)
    f = os.path.join(OUT, f"plane-launch-prof-{DATE}")
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, "prof.py"), "run", "--session", bench(),
                            "--until", str(UNTIL), "--plan", "plane-launch", "--t0", str(T0),
                            "--speed-from", str(T0 + 4), "--pilot", str(PILOT), "--out", f + ".json"],
                           capture_output=True, text=True)
        print("\n".join(l for l in (r.stdout + r.stderr).splitlines() if l.startswith(("speed", "wrote", "#", "prof"))))
    finally:
        run.stop()
    r = subprocess.run([sys.executable, os.path.join(HERE, "prof.py"), "show", f + ".json"], capture_output=True, text=True)
    open(f + ".txt", "w").write(r.stdout)
    print("\n".join(r.stdout.splitlines()[:40]))


def stage_m():
    ensure_cache()
    print("\n## M. iNav Configurator connect on the MSP port", flush=True)
    hs = os.path.join(HERE, "configurator_handshake.py")

    def connect(addr, timeout=10):
        t0, h0 = run.core.t(), time.time()
        r = subprocess.run([sys.executable, hs, addr, "inav", f"--timeout={timeout}"], capture_output=True, text=True)
        last = r.stdout.strip().splitlines()[-1]
        print(f"t {t0:.2f}..{run.core.t():.2f} ({time.time() - h0:.1f} host s) '{addr}': {last}", flush=True)
        return r.returncode == 0
    run = Run("cfgr")
    try:
        run.wait_log("firmware says")
        good = connect(f"127.0.0.1:{MSP}")
        bad = not connect(f"tcp://127.0.0.1:{MSP}")
        run.core.cli("exit")
        t_exit, h_exit, n = run.core.t(), time.time(), 1
        while not connect(f"127.0.0.1:{MSP}") and n < 10:
            n += 1
        print(f"after `exit` (t {t_exit:.2f}): connected at attempt {n}, {time.time() - h_exit:.0f} host s later; "
              + "; ".join(l.split(": ", 1)[1] for l in run.lines("CLI mode ended", "firmware says")[-2:]))
        print(f"M: {'PASS' if good and bad else 'FAIL'} (host:port connects {good}, tcp:// refused {bad})")
    finally:
        run.stop()


def stage_d():
    ensure_cache()
    print("\n## D. what the iNav Configurator sees after the connect (dialog, mixer banner, outputs)", flush=True)
    run = Run("tabs")
    try:
        run.wait_log("firmware says")
        r = subprocess.run([sys.executable, os.path.join(HERE, "configurator_handshake.py"), f"127.0.0.1:{MSP}",
                            "inav", "--tabs"], capture_output=True, text=True)
        print(r.stdout + r.stderr, end="")
        print(f"D: {'PASS' if r.returncode == 0 else 'FAIL'} (exit {r.returncode})", flush=True)
    finally:
        run.stop()


REBOOT = os.environ.get("E_ARGS", "").split()      # e.g. E_ARGS="--reboot=30 --hex"


def stage_e():
    ensure_cache()
    until = T0 + 40.0
    print(f"\n## E. the configurator stays connected through a flight: connect + tabs, then its Setup tab's "
          f"polling while plane-launch runs to t = {until:g} s", flush=True)
    run = Run("cfgr-flight")
    try:
        run.wait_log("firmware says")
        app = Core(CTL, timeout=60)                 # the app's view: modes kept current by the core
        app.send({"type": "subscribe", "modes": True, "state_hz": 20})
        plog = os.path.join(TMP, "configurator.log")   # a file, not a pipe: --hex output never blocks it
        pl = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "configurator_handshake.py"),
                               f"127.0.0.1:{MSP}", "inav", "--tabs", "--poll", f"--ctl={CTL}"] + REBOOT,
                              stdout=open(plog, "w"), stderr=subprocess.STDOUT, text=True)
        host = fly(run, "plane-launch", until, quiet=True)
        pl.send_signal(15)
        pl.wait(timeout=30)
        out = open(plog).read()
    finally:
        run.stop()
    print("\n".join(l for l in out.splitlines() if not l.startswith("  ")))
    print("core: " + "; ".join(l.split(": ", 1)[1] for l in run.lines("event armed", "event disarmed", "event launch",
                                                                   "msp:", "dropped", "stalled")))
    shutil.copy(plog, os.path.join(OUT, f"configurator-flight-{DATE}{'-' + '-'.join(a.strip('-').split('=')[0] for a in REBOOT) if REBOOT else ''}.log"))
    # the configurator disconnects only when its socket closes or a (re)connect gets no identification
    # within 10 host s (serial_backend.js 'connecting'); MSP timeouts and the status bar's dark link
    # icon (no INAV_ANALOG reply for 0.9 host s) do not disconnect it: counted, not failed
    ok = pl.returncode == 0 and "inav: connected" in out
    print(f"E: {'PASS' if ok else 'FAIL'} (poller exit {pl.returncode}: {POLL_EXIT.get(pl.returncode, '?')}; link icon "
          f"dark {out.count('link icon dark (')}x; {host:.0f} host s)", flush=True)


POLL_EXIT = {0: "connected throughout, every request answered", 1: "the connect failed", 4: "a request timed out",
             5: "the reconnect after MSP_SET_REBOOT got no identification within 10 host s"}


print(f"# plane evidence, {datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))):%Y-%m-%d %H:%M} "
      f"Yerevan; {subprocess.run(['git', '-C', ROOT, 'describe', '--always', '--dirty'], capture_output=True, text=True).stdout.strip()}; "
      f"load {open('/proc/loadavg').read().split()[:3] if os.path.exists('/proc/loadavg') else '?'}")
print(f"# QEMU {QEMU}; bench core/qemu/sessions/trainer.json (ports {CTL}/{MSP}); T0 {T0:g}", flush=True)
for st in STAGES.split():
    {"C": stage_c, "L": stage_l, "G": stage_g, "P": stage_p, "M": stage_m, "D": stage_d, "E": stage_e}[st]()
