#!/usr/bin/env python3
"""speedybee-f405-wing evidence: the stock iNav 9.1.0 SPEEDYBEEF405WING hex on the STM32F405 machine.

  A  boot on a blank config sector: the CLI script over USART6 (config: lines), save, reboot;
     then the CLI over the MSP UART socket: version, status, tasks, diff (+ exit, reboot)
  B  the control server's iNav status (MSP2_INAV_STATUS): armed, mode, arming flags, cpu load
  C  the snapshot: `firmware` (same hex: whole flash erased) -> the sector written back, no script
  D  speed: host s per 10 virtual s (t 10..20, from the state stream), idle skip off / on, 2 runs
  E  determinism: two runs each for off / on, paused at exactly t = 20 s (time pause + step):
     SRAM + CCM hash (HMP pmemsave), config sector hash

usage: f405_evidence.py <bench.json> [stages, default "A B C D E"]
  QEMU=<qemu-system-arm>; ports: control 31280, MSP 31281, HMP 31282, motion 31283, pilot 31284
  (the bench copy gets control/msp; the rest go to run.sh by env). Prints the log on stdout.
"""
import hashlib, json, os, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402

CTL, MSP, MON, MOTION, PILOT = 31280, 31281, 31282, 31283, 31284
BENCH = sys.argv[1]
STAGES = sys.argv[2] if len(sys.argv) > 2 else "A B C D E"
TMP = tempfile.mkdtemp(prefix="f405-ev.")
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu/build/qemu-system-arm"))


def bench(name, idle_skip):
    d = json.load(open(BENCH))
    d["ports"] = {"control": CTL, "msp": MSP}
    d["time"] = dict(d.get("time", {}), idle_skip=idle_skip)
    p = os.path.join(TMP, name + ".json")
    json.dump(d, open(p, "w"))
    return p


class Run:
    """run.sh --session in the background; stop() kills QEMU by PID (run.sh execs it)"""
    def __init__(self, name, idle_skip=False):
        self.log = os.path.join(TMP, name + ".log")
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION),
                   PILOT_PORT=str(PILOT), BENCH_STATE="")
        self.p = subprocess.Popen([os.path.join(HERE, "../run.sh"), "--session", bench(name, idle_skip)],
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        for _ in range(100):
            try:
                self.core = Core(CTL)
                break
            except OSError:
                time.sleep(0.1)

    def lines(self, *keys):
        return [l.rstrip() for l in open(self.log) if any(k in l for k in keys)]

    def wait_log(self, key, n=1, timeout=600):
        end = time.time() + timeout
        while time.time() < end:
            if len(self.lines(key)) >= n:
                return True
            time.sleep(0.5)
        return False

    def stop(self):
        self.p.kill()
        self.p.wait()


def hmp(cmds):
    s = socket.create_connection(("127.0.0.1", MON))
    s.settimeout(0.5)
    out = b""

    def drain(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            try:
                d = s.recv(65536)
                if not d:
                    return
                out += d
            except socket.timeout:
                if out.rstrip().endswith(b"(qemu)"):
                    return
    drain(2)
    for c in cmds:
        out = b""
        s.sendall(c.encode() + b"\n")
        drain(30)
    s.close()
    return out.decode(errors="replace")


def uart_cli(cmds, exit_after=True):
    s = socket.create_connection(("127.0.0.1", MSP))
    s.settimeout(0.5)

    def drain(quiet, first=20.0):
        out, end, last = b"", time.time() + 300, None
        while time.time() < end:
            try:
                d = s.recv(4096)
                if d:
                    out += d
                    last = time.time()
                    continue
            except socket.timeout:
                pass
            if last and time.time() - last > quiet:
                break
            if not last and time.time() > end - 300 + first:
                break
        return out.decode(errors="replace").replace("\r", "")
    s.sendall(b"#")
    print(drain(3).strip())
    for c in cmds:
        s.sendall(c.encode() + b"\r\n")
        print(f"--- {c}")
        print("\n".join(l for l in drain(4).split("\n") if l.strip()))
    if exit_after:
        s.sendall(b"exit\r\n")
        time.sleep(1)
    s.close()


def fc_at(core, t):
    m = core.wait_t(t)
    return m["t"], m["fc"], m["timing"]


def vt_ns():
    """the machine's virtual clock in ns (qom `bench` vt_ns=)"""
    import re
    m = re.search(r"\bvt_ns=(\d+)", hmp(["qom-get /machine bench"]))
    return int(m.group(1)) if m else None


def pause_at(core, t_ns):
    """pause (host-timed), then one step armed while paused that ends at t_ns exactly (under
    icount the vCPU stops at the deadline rounded up to a whole instruction); returns the clock"""
    core.send({"type": "time", "mode": "pause"})
    time.sleep(1)
    now = vt_ns()
    core.send({"type": "time", "mode": "step", "step_s": (t_ns - now) / 1e9})
    last = None
    for _ in range(600):
        time.sleep(0.5)
        v = vt_ns()
        if v is not None and v == last and v >= t_ns - 10**6:
            return v
        last = v
    return vt_ns()


def ram_hashes(tag):
    f = {n: os.path.join(TMP, f"{tag}-{n}.bin") for n in ("sram", "ccm", "cfg")}
    hmp([f"pmemsave 0x20000000 0x20000 \"{f['sram']}\"", f"pmemsave 0x10000000 0x10000 \"{f['ccm']}\"",
         f"pmemsave 0x080E0000 0x20000 \"{f['cfg']}\""])
    return {n: hashlib.sha256(open(p, "rb").read()).hexdigest()[:12] for n, p in f.items()}


def bench_stats():
    out = hmp(["qom-get /machine bench"])
    import re
    m = re.search(r"idle_skips=\d+ idle_skipped_s=[0-9.]+", out)
    return m.group(0) if m else "(no idle stats)"


rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
dirty = subprocess.run(["git", "-C", ROOT, "diff", "--quiet", "HEAD"]).returncode
cpu = next(l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name"))
print(f"# speedybee-f405-wing evidence, {time.strftime('%Y-%m-%d %H:%M', time.gmtime(time.time() + 4 * 3600))} "
      f"Yerevan; {rev}{'+dirty' if dirty else ''}; {cpu}; load {open('/proc/loadavg').read().split()[:3]}")
print(f"# QEMU={QEMU}; bench {BENCH}: {json.load(open(BENCH))['firmware']}")

if "A" in STAGES or "B" in STAGES or "C" in STAGES:
    r = Run("a")
    try:
        print("\n## A. boot on a blank config sector, the CLI script over USART6, save, reboot")
        r.wait_log("firmware says", 2)
        for l in r.lines("session:", "config:", "firmware says", "event reboot", "cli:", "CLI mode"):
            print(l)
        time.sleep(3)
        print("\n### the CLI over the MSP UART socket (USART6), after the configured reboot")
        uart_cli(["version", "status", "tasks", "diff"])
        if "B" in STAGES:
            print("\n## B. the control server's iNav status (MSP2_INAV_STATUS decoder), t > 25 s after `exit`")
            r.wait_log("firmware says", 3)
            t, fc, timing = fc_at(r.core, r.core.t() + 3)
            print(f"t={t:.2f}: fc = {json.dumps(fc)}")
        if "C" in STAGES:
            print("\n## C. snapshot: `firmware` with the same hex (whole flash erased) -> sector written back")
            n_cli = len(r.lines("entering the CLI"))
            r.core.send({"type": "firmware", "path": os.path.expanduser(json.load(open(BENCH))["firmware"])})
            r.wait_log("firmware says", 4)
            for l in r.lines("config:")[-3:]:
                print(l)
            print(f"CLI script runs in this session: {len(r.lines('entering the CLI'))} "
                  f"(before the reload {n_cli}: the reload did {'not ' if len(r.lines('entering the CLI')) == n_cli else ''}run it again)")
            uart_cli(["diff"], exit_after=True)
    finally:
        r.stop()

if "D" in STAGES:
    print("\n## D. speed: host s for virtual t 10..20 s (state stream), bare-boot bench, disarmed")
    for skip in (False, True):
        for i in (1, 2):
            r = Run(f"d-{skip}-{i}", skip)
            try:
                r.core.wait_t(10)
                h0 = time.time()
                r.core.wait_t(20)
                h = time.time() - h0
                st = bench_stats()
                print(f"idle_skip {'on ' if skip else 'off'} run {i}: {h:.2f} host s per 10 virtual s "
                      f"({10 / h:.3f}x realtime); {st}")
            finally:
                r.stop()

if "E" in STAGES:
    print(f"\n## E. determinism: paused at exactly t = 20 s, SRAM/CCM/config sector hashes (dumps: {TMP})")
    for skip in (False, True):
        for i in (1, 2):
            r = Run(f"e-{skip}-{i}", skip)
            try:
                r.core.wait_t(18)
                t = pause_at(r.core, 20 * 10**9)
                print(f"idle_skip {'on ' if skip else 'off'} run {i}: paused at {t} ns; {bench_stats()}; {ram_hashes(f'e-{skip}-{i}')}")
            finally:
                r.stop()
