#!/usr/bin/env python3
"""The merge gates of docs/contributing.md in one run: host suites, equiv, and the four baseline flights.

  gates.py [--qemu BIN] [--ports 31280] [--runs 2] [--only host,equiv,rs7,rs7-osd,trainer,trainer-osd]
           [--expect NAME=HASH ...] [--new-baseline NAME ...] [--out DIR]

Flights (FDM-log md5, 12 hex digits, as every evidence README records them):
  rs7          sessions/rs7.json          prof.py run --plan liftoff to 45 s, 20 ms rows  (rs7_stock.py H)
  rs7-osd      sessions/rs7-osd.json      the same                                        (speed_evidence.sh S E)
  trainer      sessions/trainer.json      plane_evidence.py "C L": first start + plane-launch to 54 s, 10 ms rows
  trainer-osd  sessions/trainer-osd.json  inav_osd_evidence.py "C F": the same on the OSD bench
Expected hashes: the table below per platform (docs/contributing.md); --expect overrides one, --new-baseline
marks a flight whose hash is expected to move (it is printed, not compared: the brief must say why).
Everything runs on the five ports from --ports; the evidence scripts' output goes to --out (default a
temp dir: nothing lands in core/qemu/evidence/). Exit 0 when every gate holds, 1 otherwise; the last
lines are the summary table. Stops its own QEMUs by PID (winproc.py); never touches other cores.
"""
import argparse, hashlib, json, os, platform, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
import winproc  # noqa: E402

for _f in (sys.stdout, sys.stderr):      # Windows consoles default to cp1251/cp866: never die on a hash table
    try:
        _f.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

WIN = sys.platform == "win32"
EXPECT = {  # docs/contributing.md "Gates"; the flight models changed 2026-10-24: Linux to re-baseline on the homelab
    "linux": {},
    "win32": {"rs7": "adcc355a6808", "trainer": "18c7adf95de1"},
    "darwin": {"rs7": "530d7de34c99", "trainer": "523feee9abe7"},   # Apple silicon (M-series), 2026-10-24
}
ALL = ["host", "equiv", "rs7", "rs7-osd", "trainer", "trainer-osd"]


def sh(cmd, env=None, cwd=None, timeout=3600):
    r = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, env=env, cwd=cwd,
                       timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def tail(text, n=6):
    return "\n".join(l for l in text.splitlines() if l.strip())[-2000:].splitlines()[-n:]


class Gates:
    def __init__(self, a):
        self.a = a
        self.qemu = a.qemu
        self.ctl, self.msp, self.mon, self.motion, self.pilot = range(a.ports, a.ports + 5)
        self.out = a.out or tempfile.mkdtemp(prefix="gates.")
        os.makedirs(self.out, exist_ok=True)
        self.expect = dict(EXPECT.get(sys.platform if sys.platform in EXPECT else "linux", {}))
        for e in a.expect or []:
            k, v = e.split("=", 1)
            self.expect[k] = v
        self.results = []   # (gate, ok|None, detail)
        head = sh(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"])[1].strip()
        print(f"# gates {time.strftime('%Y-%m-%d %H:%M')} host={platform.node()} {sys.platform} "
              f"repo {head}; QEMU {self.qemu}; ports {self.ctl}..{self.pilot}; out {self.out}", flush=True)

    def record(self, gate, ok, detail):
        self.results.append((gate, ok, detail))
        tag = "PASS" if ok else ("info" if ok is None else "FAIL")
        print(f"=> {gate}: {tag} - {detail}", flush=True)

    # ---- host suites and equiv --------------------------------------------------------------
    def host(self):
        print("\n## host suites", flush=True)
        ok_all = True
        make = "mingw32-make" if WIN and shutil.which("mingw32-make") else "make"
        steps = [
            ("devices", [make, "-C", os.path.join(ROOT, "devices"), "test"]),
            ("fdm", [make, "-C", os.path.join(ROOT, "fdm"), "test"]),
            ("core/qemu/tests", [make, "-C", os.path.join(ROOT, "core/qemu/tests"), "test"]),
            ("test_wiring.py", [sys.executable, os.path.join(HERE, "test_wiring.py")]),
            ("py_compile", [sys.executable, "-m", "py_compile"] +
             [os.path.join(HERE, f) for f in sorted(os.listdir(HERE)) if f.endswith(".py")] +
             [os.path.join(ROOT, "tools", f) for f in sorted(os.listdir(os.path.join(ROOT, "tools"))) if f.endswith(".py")]),
        ]
        for name, cmd in steps:
            rc, text = sh(cmd)
            open(os.path.join(self.out, f"host-{name.replace('/', '_')}.log"), "w").write(text)
            fails = [l for l in text.splitlines() if re.search(r"\b(FAIL|failed|error)\b", l, re.I)
                     and not re.search(r"\b0 fail|0 failure|0 failed|, 0 failed|failed=0", l, re.I)]
            ok = rc == 0 and not fails
            ok_all &= ok
            print(f"{name}: {'ok' if ok else 'FAILED (rc %d)' % rc}" + ("" if ok else "\n  " + "\n  ".join(fails[:5] or tail(text))),
                  flush=True)
        self.record("host", ok_all, "all suites pass" if ok_all else "see above")

    def equiv(self):
        print("\n## equiv (C resolver vs Python reference, -M session-check=on)", flush=True)
        rc, text = sh(["make", "-C", os.path.join(ROOT, "core/qemu/tests"), "equiv", f"QEMU={self.qemu}"])
        open(os.path.join(self.out, "equiv.log"), "w").write(text)
        m = re.search(r"# (\d+) passed, (\d+) failed \(of (\d+)\)", text)
        d = re.search(r"# cli_diff: (\d+) passed, (\d+) failed", text)
        ok = rc == 0 and m and m.group(2) == "0" and (not d or d.group(2) == "0")
        detail = (f"{m.group(1)}/{m.group(3)} benches" if m else "no summary line") + (f", cli_diff {d.group(1)} ok" if d else "")
        if not ok:
            detail += " | " + " / ".join(tail(text, 4))
        self.record("equiv", bool(ok), detail)

    # ---- RS7 benches: prof.py run (speed) + FDM log cut to 45 s -----------------------------
    def rs7_like(self, gate, session):
        print(f"\n## {gate} ({session}) liftoff to 45 s, {self.a.runs} runs", flush=True)
        d = json.load(open(os.path.join(ROOT, "core/qemu/sessions", session)))
        d["ports"] = {"control": self.ctl, "msp": self.msp}
        bench = os.path.join(self.out, f"{gate}.json")
        json.dump(d, open(bench, "w"))
        sums, speeds = [], []
        for i in range(1, self.a.runs + 1):
            fdm = os.path.join(self.out, f"{gate}-run{i}-fdm.csv")
            env = dict(os.environ, QEMU=self.qemu, MON_PORT=str(self.mon), MOTION_PORT=str(self.motion),
                       PILOT_PORT=str(self.pilot), MACHINE_OPTS=f"fdm-log={fdm},fdm-log-ms=20")
            log = open(os.path.join(self.out, f"{gate}-run{i}.log"), "w")
            p = winproc.popen_run_sh([os.path.join(HERE, "../run.sh"), "--session", bench], stdout=log,
                                     stderr=subprocess.STDOUT, env=env)
            try:
                rc, text = sh([sys.executable, os.path.join(HERE, "prof.py"), "run", "--session", bench, "--pilot",
                               str(self.pilot), "--until", "45", "--speed-from", "5", "--plan", "liftoff", "--no-dump"],
                              timeout=1800)
            finally:
                winproc.stop(p, fdm)
                log.close()
            sp = [l for l in text.splitlines() if l.startswith("speed")]
            speeds += sp
            md5, n = self.cut(fdm, 45.0)
            sums.append(md5)
            print(f"run {i}: FDM log {md5}, {n} rows; {sp[0] if sp else 'no speed line (prof.py rc %d)' % rc}", flush=True)
            if rc:
                print("  " + "\n  ".join(tail(text)), flush=True)
        self.judge(gate, sums)

    @staticmethod
    def cut(path, until):
        if not os.path.exists(path):
            return "no-fdm-log", 0
        rows = [l for i, l in enumerate(open(path)) if i == 0 or float(l.split(",", 1)[0]) <= until + 1e-9]
        return hashlib.md5("".join(rows).encode()).hexdigest()[:12], len(rows) - 1

    # ---- trainer benches: the evidence scripts' C + flight stages, hashes parsed ----------------
    def trainer_like(self, gate, script, stages):
        print(f"\n## {gate}: {script} \"{stages}\"", flush=True)
        env = dict(os.environ, QEMU=self.qemu, PORT_BASE=str(self.ctl), OUT=os.path.join(self.out, gate),
                   CFG_CACHE=os.path.join(self.out, gate + "-cfg-cache"))
        os.makedirs(env["OUT"], exist_ok=True)
        rc, text = sh([sys.executable, os.path.join(HERE, script), stages], env=env, timeout=3600)
        open(os.path.join(self.out, f"{gate}.log"), "w").write(text)
        runs = re.findall(r"^run (\d+): FDM log ([0-9a-f]{12}), (\d+) rows; (\d+) host s", text, re.M)
        for r in runs:
            print(f"run {r[0]}: FDM log {r[1]}, {r[2]} rows; {r[3]} host s", flush=True)
        if rc or not runs:
            print(f"{script} rc {rc}\n  " + "\n  ".join(tail(text, 8)), flush=True)
        self.judge(gate, [r[1] for r in runs] or ["no-run"])

    def judge(self, gate, sums):
        distinct = sorted(set(sums))
        if gate in (self.a.new_baseline or []):
            self.record(gate, len(distinct) == 1, f"new baseline {distinct} x{len(sums)} (not compared)")
            return
        exp = self.expect.get(gate)
        if exp is None:
            self.record(gate, None, f"{distinct} x{len(sums)} - no baseline for {sys.platform} in the table")
            return
        ok = distinct == [exp]
        self.record(gate, ok, f"{'holds' if ok else 'MOVED'}: got {distinct} x{len(sums)}, expected {exp}")

    def run(self):
        only = self.a.only.split(",") if self.a.only else ALL
        try:
            for g in only:
                if g == "host": self.host()
                elif g == "equiv": self.equiv()
                elif g == "rs7": self.rs7_like("rs7", "rs7.json")
                elif g == "rs7-osd": self.rs7_like("rs7-osd", "rs7-osd.json")
                elif g == "trainer": self.trainer_like("trainer", "plane_evidence.py", "C L")
                elif g == "trainer-osd": self.trainer_like("trainer-osd", "inav_osd_evidence.py", "C F")
                else: self.record(g, False, "unknown gate")
        finally:
            print("\n## summary", flush=True)
            width = max(len(r[0]) for r in self.results) if self.results else 8
            for gate, ok, detail in self.results:
                print(f"{gate:<{width}}  {'PASS' if ok else ('info' if ok is None else 'FAIL'):<4}  {detail}")
            failed = [r[0] for r in self.results if r[1] is False]
            print(f"# {'ALL GATES HOLD' if not failed else 'FAILED: ' + ', '.join(failed)}; logs in {self.out}", flush=True)
        return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", default=os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu/build/qemu-system-arm")))
    ap.add_argument("--ports", type=int, default=int(os.environ.get("PORT_BASE", "31280")), help="first of five ports")
    ap.add_argument("--runs", type=int, default=2, help="runs per RS7 flight (the trainer scripts fly twice)")
    ap.add_argument("--only", help="comma list of " + ",".join(ALL))
    ap.add_argument("--expect", action="append", metavar="NAME=HASH")
    ap.add_argument("--new-baseline", action="append", metavar="NAME")
    ap.add_argument("--out", help="directory for logs/CSVs (default: a temp dir)")
    a = ap.parse_args()
    if not os.path.exists(a.qemu):
        sys.exit(f"gates.py: QEMU binary not found: {a.qemu}")
    sys.exit(Gates(a).run())


if __name__ == "__main__":
    main()
