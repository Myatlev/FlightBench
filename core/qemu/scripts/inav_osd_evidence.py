#!/usr/bin/env python3
"""iNav MSP DisplayPort OSD evidence: the trainer with an HD VTX OSD on UART4 (evidence/inav-osd/).

  C  first start of the OSD bench (sessions/trainer-osd.json) with an empty cfg-cache: the CLI script
     with the OSD's device lines, save, the ACC calibration, save (the snapshot the other stages boot)
  B  boot from the snapshot to t = 30 s on the ground: the stream the firmware sends to the device
     (-M osd-log= -> stream-boot.bin), `osd` messages (rate, full/delta, the font), a text-art dump of
     the canvas, the core's view (hello `osd`, the bench line: polls, FC variant, OPTIONS mode, the
     MAX7456's writes), the CLI's `serial` / `get osd_video_system` / `feature`
  F  plane-launch twice (T0 20, to t = 54 s) on the OSD bench: the FDM log md5 per run
     (determinism), host s; the stream of run 1 (stream-flight.bin) and a frame in the climb
  P  prof=on, plane-launch to t = 54 s on the OSD bench: prof.py show (top accounts)

usage: inav_osd_evidence.py [stages, default "C B F P"]
  QEMU=<qemu-system-arm>, OUT=<dir> (default core/qemu/evidence/inav-osd), CFG_CACHE=<dir> (an
  existing snapshot of this bench: B/F/P without C). Ports: control 31280, MSP 31281, HMP 31282,
  motion 31283, pilot 31284 (PORT_BASE). Prints the log on stdout.
"""
import datetime, hashlib, json, os, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402
sys.path.insert(0, HERE)
import winproc  # noqa: E402
from osd_evidence import Canvas, osd_msgs, rate_report, summary  # noqa: E402

PORT0 = int(os.environ.get("PORT_BASE", "31280"))
CTL, MSP, MON, MOTION, PILOT = range(PORT0, PORT0 + 5)
STAGES = sys.argv[1] if len(sys.argv) > 1 else "C B F P"
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-osd/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/inav-osd"))
T0 = 20.0
UNTIL = T0 + 34.0
TMP = tempfile.mkdtemp(prefix="inav-osd-ev.")
CACHE = os.environ.get("CFG_CACHE") or os.path.join(TMP, "cfg-cache")
os.makedirs(OUT, exist_ok=True)


def bench():
    d = json.load(open(os.path.join(ROOT, "core/qemu/sessions/trainer-osd.json")))
    d["ports"] = {"control": CTL, "msp": MSP}
    p = os.path.join(TMP, "trainer-osd.json")
    json.dump(d, open(p, "w"))
    return p


def hmp(cmd):
    s = socket.create_connection(("127.0.0.1", MON), timeout=10)
    s.recv(4096)
    s.sendall((cmd + "\n").encode())
    out = b""
    end = time.time() + 5
    while time.time() < end:
        try:
            out += s.recv(65536)
        except socket.timeout:
            break
        if out.rstrip().endswith(b"(qemu)") and cmd.encode() in out:
            break
    s.close()
    lines = out.decode(errors="replace").splitlines()
    return [l for l in lines if l and not l.startswith("(qemu)") and "QEMU" not in l and cmd not in l]


def bench_line():
    got = hmp("qom-get /machine bench")
    return got[-1].strip('"') if got else ""


def fields(line, keys):
    kv = dict(x.split("=", 1) for x in line.split() if "=" in x)
    return " ".join(f"{k}={kv.get(k)}" for k in keys)


OSD_KEYS = ("t", "osd", "osd_frames", "osd_writes", "osd_clipped", "osd_contacts", "osd_canvas_sent",
            "osd_canvas_acks", "osd_bad", "osd_polls", "osd_variants", "osd_errors", "osd_options", "osd_mode",
            "osd_fc", "max7456_display_writes", "max7456_clears")


class Run:
    def __init__(self, name, opts="", prof=False, log_stream=None):
        self.name = name
        self.log = os.path.join(TMP, name + ".log")
        self.fdm = os.path.join(TMP, name + "-fdm.csv")
        mo = f"cfg-cache={CACHE},fdm-log={self.fdm},fdm-log-ms=10"
        if log_stream:
            mo += f",osd-log={log_stream}"
        if opts:
            mo += "," + opts
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=mo)
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
        winproc.stop(self.p, self.fdm)


def cut(path, until, out=None):
    rows = [l for i, l in enumerate(open(path)) if i == 0 or float(l.split(",", 1)[0]) <= until + 1e-9]
    if out:
        open(out, "w").writelines(rows)
    return hashlib.md5("".join(rows).encode()).hexdigest()[:12], len(rows) - 1


def fly(plan, until, quiet=True):
    t0 = time.time()
    r = subprocess.run([sys.executable, os.path.join(HERE, "flight.py"), "--plan", plan, "--until", str(until),
                        "--t0", str(T0), "--pilot", str(PILOT), "--msp", str(MSP)] + (["--quiet"] if quiet else []),
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
    return time.time() - t0


# iNav's font (inav-configurator resources/osd/analogue/default.mcm, drivers/osd_symbols.h): text at
# 0x20..0x5F except these four, symbols at 0x60..0x7E (no lowercase letters) and 0x00..0x1F, 0x7F..0x1FF
INAV_SYMBOLS_IN_ASCII = {0x22, 0x24, 0x27, 0x3F}
INAV_SRC = os.environ.get("INAV_SRC", os.path.expanduser("~/flightbench-spike/inav"))


def sym_names():
    """glyph -> SYM_ names from iNav's drivers/osd_symbols.h (when the source is there)"""
    names = {}
    try:
        for line in open(os.path.join(INAV_SRC, "src/main/drivers/osd_symbols.h")):
            w = line.split()
            if len(w) >= 3 and w[0] == "#define" and w[1].startswith("SYM_") and w[2].startswith("0x"):
                names.setdefault(int(w[2], 16), []).append(w[1])
    except OSError:
        pass
    return names


def art(k):
    """the canvas as text: iNav's text characters as is, every symbol glyph as '#' with the legend below
    (glyph = (attr & 3) << 8 | byte, the page in the attr's low bits)"""
    names = sym_names()
    legend, out = {}, []
    for r in range(k.rows):
        row = ""
        for c in range(k.cols):
            ch, at = ord(k.ch[r][c]), k.at[r][c]
            g = (at & 3) << 8 | ch
            if g < 0x60 and ch >= 0x20 and ch not in INAV_SYMBOLS_IN_ASCII:
                row += chr(ch)
            else:
                legend.setdefault(g, []).append((r, c))
                row += "#"
        out.append(f"  {r:2d} |{row}|")
    out.append("  # = a symbol glyph — glyph (iNav name): cells (row,col)")
    for g in sorted(legend):
        cells = legend[g]
        nm = "/".join(names.get(g, [])[:2])
        out.append(f"    0x{g:03x}{' ' + nm if nm else ''}: " + " ".join(f"({r},{c})" for r, c in cells[:10]) +
                   (f" +{len(cells) - 10}" if len(cells) > 10 else ""))
    return "\n".join(out)


def write_canvas(ms, path):
    """the canvas a client of the `osd` messages holds after the last one, for the replay test
    (devices/tests/test_msp_osd.c): the DRAW count, then one line per row, per cell attr and byte as
    4 hex digits"""
    k = canvas_of(ms)
    with open(path, "w") as f:
        f.write(f"# the canvas after DRAW {ms[-1]['frame']} (t = {ms[-1]['t']:.3f}), from the core's osd messages; "
                f"{k.rows} rows x {k.cols} cols, per cell attr<<8|byte in hex\n")
        f.write(f"frame {ms[-1]['frame']}\n")
        for r in range(k.rows):
            f.write(" ".join(f"{k.at[r][c]:02x}{ord(k.ch[r][c]):02x}" for c in range(k.cols)) + "\n")


def canvas_of(ms, until=None):
    k = Canvas()
    for m in ms:
        if until is not None and m["t"] > until:
            break
        k.apply(m)
    return k


def stage_c():
    os.makedirs(CACHE, exist_ok=True)
    print("\n## C. first start of the OSD bench (empty cfg-cache): script with the OSD lines, save, ACC calibration",
          flush=True)
    run = Run("cal")
    try:
        ok = run.wait_log('"state":"applied"')
        run.wait_log("firmware says", n=3)
        t_up = float(run.lines("firmware says")[-1].split("t=")[1].split(":")[0])
        run.core.wait_t(t_up + 3.0)
        for l in run.lines("config:", "osd", "calibration", "firmware says", "reboot"):
            if "config: set " in l and "osd" not in l and "OSD" not in l:
                continue
            print(l)
        print(f"# applied={ok}; to t = {run.core.t():.1f} virtual s in {time.time() - run.host0:.0f} host s")
        print("# " + fields(bench_line(), OSD_KEYS))
    finally:
        run.stop()


def ensure_cache():
    if not (os.path.isdir(CACHE) and os.listdir(CACHE)):
        stage_c()


def stage_b():
    ensure_cache()
    until = 30.0
    stream = os.path.join(OUT, "stream-boot.bin")
    print(f"\n## B. boot from the snapshot to t = {until:g} s on the ground: stream, osd messages, canvas", flush=True)
    run = Run("boot", log_stream=stream)
    try:
        c = run.core
        hello = next((m for m in c.msgs if m.get("type") == "hello"), None)
        c.wait_t(until)
        host = time.time() - run.host0
        ms = osd_msgs(c)
        print(f"# to t = {c.t():.1f} virtual s in {host:.0f} host s")
        if hello:
            print("hello osd: " + json.dumps(hello.get("osd")))
        for l in run.lines("firmware says", "reboot", "osd"):
            print(l)
        print("bench: " + fields(bench_line(), OSD_KEYS))
        summary(ms)
        fonts = sorted({m.get("font") for m in ms})
        print(f"  font field: {fonts}; link:false messages {sum(1 for m in ms if not m.get('link', True))}")
        if ms:
            rate_report(ms, 10.0, until, "osd messages t 10..30: ")
        k = canvas_of(ms)
        print(f"### the canvas at t = {ms[-1]['t'] if ms else 0:.2f} (frame {ms[-1]['frame'] if ms else '-'}), "
              f"{k.rows} x {k.cols}:")
        print(art(k))
        attrs = sorted({k.at[r][c] for r in range(k.rows) for c in range(k.cols)})
        print(f"  attr values on the canvas: {[hex(a) for a in attrs]}")
        json.dump(ms, open(os.path.join(TMP, "boot-osd.json"), "w"))
        write_canvas(ms, os.path.join(OUT, "stream-boot.canvas.txt"))
        # status: "OSD: <displayPortType> [cols x rows]" (fc/cli.c:4263-4267): the MSP OSD, not the MAX7456
        for cmd in ("status", "serial", "get osd_video_system", "feature", "get osd_msp_displayport_fullframe_interval"):
            for line in c.cli(cmd):
                line = line.rstrip()
                if not line or line.startswith("#") or (cmd == "serial" and not line.startswith("serial 3")):
                    continue
                if cmd == "status" and not line.startswith(("OSD", "System Uptime", "Sensor status")):
                    continue
                print(f"cli {cmd}| {line}")
    finally:
        run.stop()
    print(f"stream-boot.bin: {os.path.getsize(stream)} B")


def stage_f():
    ensure_cache()
    print(f"\n## F. plane-launch (T0 {T0:g}) twice on the OSD bench, to t = {UNTIL:g} s", flush=True)
    sums = []
    for i in (1, 2):
        stream = os.path.join(OUT, "stream-flight.bin") if i == 1 else None
        run = Run(f"launch-{i}", log_stream=stream)
        try:
            host = fly("plane-launch", UNTIL)
            ms = osd_msgs(run.core)
            b = bench_line()
        finally:
            run.stop()
        md5, n = cut(run.fdm, UNTIL)
        sums.append(md5)
        print(f"run {i}: FDM log {md5}, {n} rows; {host:.0f} host s for {UNTIL:g} virtual s "
              f"({UNTIL / host:.3f}x); osd messages {len(ms)}; " + fields(b, ("osd_frames", "osd_polls", "osd_fc")))
        if i == 1:
            rate_report(ms, T0 + 4, UNTIL, "osd messages in flight: ")
            k = canvas_of(ms, T0 + 12.0)
            print(f"### the canvas at t = {T0 + 12:g} (in the climb after the launch):")
            print(art(k))
    print(f"=> {len(set(sums))} distinct FDM log(s) over 2 runs", flush=True)


def stage_p():
    ensure_cache()
    print(f"\n## P. prof=on, plane-launch to t = {UNTIL:g} s on the OSD bench", flush=True)
    run = Run("prof", prof=True)
    f = os.path.join(OUT, "plane-launch-osd-prof")
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


def main():
    stamp = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))).strftime("%Y-%m-%d %H:%M")
    rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    print(f"# inav-osd evidence, {stamp} Yerevan; {rev}; QEMU {QEMU}; ports {CTL}..{PILOT}; stages {STAGES}")
    for st in STAGES.split():
        {"C": stage_c, "B": stage_b, "F": stage_f, "P": stage_p}[st]()


if __name__ == "__main__":
    main()
