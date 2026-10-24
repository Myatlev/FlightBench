#!/usr/bin/env python3
"""Phase 4a evidence: the speedybee-f405-wing's sensors under stock iNav 9.1.0 (evidence/sensors/).

  C  first start with an empty cfg-cache: the CLI script (with the GPS's lines), save, the ACC and the
     compass calibration, save; then the CLI `status`, `get mag`, the diff's sensor lines
  S  from the snapshot, at rest on the pad: the GPS fix (when iNav reports 3D, sats), MSP_RAW_GPS vs
     the FDM's position around home; the heading (MSP_ATTITUDE) with the model held at yaw
     0/90/180/270/45 deg; the baro altitude; the battery as iNav reads it (MSP2_INAV_ANALOG);
     faults: ublox no_fix / silence / spoof, battery voltage / disconnect, qmc5883l offset / detached,
     spl06 offset; then the CLI `status`
  L  flight.py --plan plane-launch, twice, to T0 + 34 s: FDM log md5 (determinism), flight_report.py
     --plane; the battery over the flight (voltage, current, mAh vs throttle), MSP_RAW_GPS vs the FDM
     position (lag-compensated), the baro altitude vs the FDM's; after the hashed window iNav's own
     battery reading (MSP2_INAV_ANALOG: voltage, current, mAh drawn) against the model's
  R  RS7 liftoff (sessions/rs7.json, flight.py liftoff via prof.py run, FDM log every 20 ms to 45 s),
     twice: md5 (the nexus-xr's baseline c5a2b9bb6c49 must hold)
  P  prof=on, plane-launch to T0 + 34 s: prof.py show; the flight's speed

usage: sensors_evidence.py [stages, default "C S L R P"]
  QEMU=<qemu-system-arm> (default ~/flightbench-spike/qemu-4a), OUT=<dir> (default
  core/qemu/evidence/sensors), T0 (default 20), CFG_CACHE=<dir> (a cfg-cache with the snapshot,
  instead of an empty one), PORT_BASE (default 31285): control, MSP, HMP, motion, pilot from there.
"""
import datetime, hashlib, json, math, os, socket, struct, subprocess, sys, tempfile, time

sys.stdout.reconfigure(line_buffering=True)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402
sys.path.insert(0, HERE)
import winproc  # noqa: E402

PORT0 = int(os.environ.get("PORT_BASE", "31285"))   # five ports from here
CTL, MSP, MON, MOTION, PILOT = range(PORT0, PORT0 + 5)
STAGES = sys.argv[1] if len(sys.argv) > 1 else "C S L R P"
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-4a/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/sensors"))
T0 = float(os.environ.get("T0", "20"))
UNTIL = T0 + 34.0
DATE = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))).strftime("%Y-%m-%d")
TMP = tempfile.mkdtemp(prefix="sensors-ev.")
CACHE = os.environ.get("CFG_CACHE") or os.path.join(TMP, "cfg-cache")
HOME = (40.1792, 44.4991)
os.makedirs(OUT, exist_ok=True)


def bench(name="trainer.json"):
    d = json.load(open(os.path.join(ROOT, "core/qemu/sessions", name)))
    d["ports"] = {"control": CTL, "msp": MSP}
    p = os.path.join(TMP, name)
    json.dump(d, open(p, "w"))
    return p


class Run:
    """run.sh --session in the background (run.sh execs QEMU: stop() kills it by PID)"""
    def __init__(self, name, opts="", prof=False, session="trainer.json", log_ms=10):
        self.name = name
        self.log = os.path.join(TMP, name + ".log")
        self.fdm = os.path.join(TMP, name + "-fdm.csv")
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=f"cfg-cache={CACHE},fdm-log={self.fdm},fdm-log-ms={log_ms}" + (f",{opts}" if opts else ""))
        if prof:
            env["PROF"] = "1"
        self.p = winproc.popen_run_sh([os.path.join(HERE, "../run.sh"), "--session", bench(session)],
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
            if self.p.poll() is not None:
                return False
            time.sleep(1)
        return False

    def stop(self):
        winproc.stop(self.p, self.fdm)   # the FDM log's path is unique to this run's QEMU


# ---- MSP v2 over the MSP socket (the core forwards host frames; its own polls are v2 too, their
# replies it keeps: ours carry other command ids) ------------------------------------------------

def crc8_dvb_s2(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def msp2(cmd, payload=b"", timeout=10.0):
    body = struct.pack("<BHH", 0, cmd, len(payload)) + payload
    frame = b"$X<" + body + bytes([crc8_dvb_s2(body)])
    with socket.create_connection(("127.0.0.1", MSP), timeout=2) as s:
        s.settimeout(0.2)
        s.sendall(frame)
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            try:
                buf += s.recv(4096)
            except socket.timeout:
                continue
            i = buf.find(b"$X>")
            if i >= 0 and len(buf) >= i + 8:
                flag, c, n = struct.unpack("<BHH", buf[i + 3:i + 8])
                if len(buf) >= i + 9 + n:
                    if c == cmd:
                        return buf[i + 8:i + 8 + n]
                    buf = buf[i + 9 + n:]
    raise TimeoutError(f"no reply to MSP2 0x{cmd:04x}")


def inav_analog():
    """MSP2_INAV_ANALOG: cells, voltage V, current A, mAh drawn, battery %"""
    p = msp2(0x2002)
    flags, mv, ca, pw, mah, mwh, cap, pct = struct.unpack("<BHhIIIIB", p[:22])
    return {"cells": flags >> 4, "state": (flags >> 2) & 3, "voltage_v": mv / 100, "current_a": ca / 100,
            "mah": mah, "pct": pct}


def raw_gps():
    """MSP_RAW_GPS (106) as a v2 frame"""
    p = msp2(106)
    fix, sats, lat, lon, alt, spd, crs = struct.unpack("<BBiihHH", p[:16])
    return {"fix": fix, "sats": sats, "lat": lat / 1e7, "lon": lon / 1e7, "alt_m": alt, "speed_mps": spd / 100,
            "course_deg": crs / 10}


def ned_of(lat, lon, home=HOME):
    """lat/lon -> north/east metres around home, the model's WGS-84 radii (devices/src/ublox.c)"""
    a, e2 = 6378137.0, 6.69437999014e-3
    phi = math.radians(home[0])
    s = math.sin(phi) ** 2
    rm = a * (1 - e2) / (1 - e2 * s) ** 1.5
    rn = a / math.sqrt(1 - e2 * s)
    return math.radians(lat - home[0]) * rm, math.radians(lon - home[1]) * rn * math.cos(phi)


def wrap(d):
    return (d + 180.0) % 360.0 - 180.0


# ---- stages ------------------------------------------------------------------------------------

def fly(plan, until, t0=T0):
    t = time.time()
    r = subprocess.run([sys.executable, os.path.join(HERE, "flight.py"), "--plan", plan, "--until", str(until),
                        "--t0", str(t0), "--pilot", str(PILOT), "--msp", str(MSP), "--quiet"],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
    return time.time() - t


def cut(path, until, out):
    rows = [l for i, l in enumerate(open(path)) if i == 0 or float(l.split(",", 1)[0]) <= until + 1e-9]
    open(out, "w").writelines(rows)
    return hashlib.md5("".join(rows).encode()).hexdigest()[:12], len(rows) - 1


def ensure_cache():
    if not (os.path.isdir(CACHE) and os.listdir(CACHE)):
        stage_c(quiet=True)


def cli_print(run, *cmds, keep=None):
    for cmd in cmds:
        for line in run.core.cli(cmd):
            if keep is None or any(k in line for k in keep):
                print("cli| " + line)


def stage_c(quiet=False):
    os.makedirs(CACHE, exist_ok=True)
    if not quiet:
        print("\n## C. first start, empty cfg-cache: script, save, ACC + compass calibration, save", flush=True)
    run = Run("first-start")
    try:
        ok = run.wait_log('"state":"applied"') or run.wait_log('"state":"failed"', timeout=1)
        run.wait_log("firmware says", n=3)
        t_up = float(run.lines("firmware says")[-1].split("t=")[1].split(":")[0])
        run.core.send({"type": "subscribe", "state_hz": 2})
        run.core.wait_t(t_up + 3.0)
        host = time.time() - run.host0
        if quiet:
            print(f"# cfg-cache filled ({'ok' if ok else 'FAILED'}, {host:.0f} host s)", flush=True)
            return
        for l in run.lines("config", "calibrate", "function", "snapshot", "firmware says", "reboot"):
            print(l)
        st = run.core.state
        print(f"# to t = {st['t']:.1f} virtual s in {host:.0f} host s")
        print(f"t={st['t']:.2f} fc: armed {st['fc']['armed']}, arming_flags {st['fc']['arming_flags']}")
        cli_print(run, "status")
        cli_print(run, "get mag", keep=("magzero", "maggain", "align_mag ", "mag_hardware", "mag_declination",
                                        "inav_auto_mag_decl", "mag_calibration_time"))
        cli_print(run, "diff all", keep=("hardware", "align_mag", "feature", "serial", "gps_"))
        print("cfg-cache: " + " ".join(f"{f} ({os.path.getsize(os.path.join(CACHE, f))} B)" for f in os.listdir(CACHE)))
    finally:
        run.stop()


def state_fc(run):
    st = run.core.state
    return st, st["fc"]


def stage_s():
    ensure_cache()
    print("\n## S. at rest on the pad, from the snapshot: GPS fix, position, heading, baro, battery, faults",
          flush=True)
    run = Run("rest", "env-scenario=ground-takeoff")
    try:
        c = run.core
        c.send({"type": "subscribe", "state_hz": 10})
        # the GPS: when iNav has a 3D fix
        t_fix = t_gps = None
        while c.t() < 60:
            m = c.wait_for(lambda m: m["type"] == "state", 60)
            g = m["fc"].get("gps")
            if g and t_gps is None:
                t_gps = m["t"]
                print(f"t={m['t']:.2f} iNav reports the GPS (MSP sensors bit 3): fix {g['fix']} sats {g['sats']}")
            if g and g["fix"] >= 2 and t_fix is None:
                t_fix = m["t"]
                print(f"t={m['t']:.2f} iNav's GPS: fix {g['fix']} (iNav's gpsFixType_e: 2 = 3D), {g['sats']} sats, "
                      f"{g['lat']:.7f} {g['lon']:.7f}, {g['alt_m']} m")
                break
        c.wait_t(c.t() + 3)
        # MSP_RAW_GPS vs the FDM's position (at rest: the pad = home), 10 samples 1 s apart
        errs = []
        for _ in range(10):
            st, fc = state_fc(run)
            g = raw_gps()
            n, e = ned_of(g["lat"], g["lon"])
            errs.append(math.hypot(n - st["pos_ned"][0], e - st["pos_ned"][1]))
            c.wait_t(c.t() + 1)
        print(f"MSP_RAW_GPS vs FDM position around home (10 samples, 1 s apart): horizontal error max "
              f"{max(errs):.2f} m, mean {sum(errs) / len(errs):.2f} m; last {g}")
        st, fc = state_fc(run)
        print(f"t={st['t']:.2f} baro: iNav's altitude (MSP_ALTITUDE) {fc.get('baro_alt_m')} m, FDM "
              f"{-st['pos_ned'][2]:.2f} m; battery (model) {st.get('battery')}; iNav {inav_analog()}")
        print(f"arming blockers now: {fc['arming_flags']}")
        # the heading: held level at a yaw, 8 s still, then MSP_ATTITUDE yaw vs the FDM's
        print("heading (model held level at yaw, 8 s still): FDM yaw, iNav MSP_ATTITUDE heading, difference")
        for yaw in (0, 90, 180, 270, 45, 0):
            c.send({"type": "constraint", "mode": "hold", "euler_deg": [0, 0, yaw], "rotate_s": 2})
            c.wait_t(c.t() + 10)
            st, fc = state_fc(run)
            fy, iy = st["euler_deg"][2], fc["attitude_deg"][2]
            print(f"  t={st['t']:.2f} FDM {fy:7.2f} deg, iNav {iy:4d} deg, diff {wrap(iy - fy):+.2f} deg")
        # GPS faults as iNav sees them
        def gps_now(label):
            c.wait_t(c.t() + 3)
            st, fc = state_fc(run)
            g = raw_gps()
            print(f"  t={st['t']:.2f} {label}: MSP_RAW_GPS fix {g['fix']} sats {g['sats']} "
                  f"{g['lat']:.7f} {g['lon']:.7f}; blockers {fc['arming_flags']}")
            return g
        print("GPS faults (MSP_RAW_GPS 3 s after each change)")
        for kind, params in (("no_fix", None), ("silence", None), ("spoof", {"north_m": 50, "east_m": -20})):
            msg = {"type": "fault", "device": "ublox", "kind": kind, "on": True}
            if params:
                msg["params"] = params
            c.send(msg)
            g = gps_now(f"ublox.{kind} on")
            if kind == "spoof":
                n, e = ned_of(g["lat"], g["lon"])
                print(f"    spoofed position {n:.1f} m N, {e:.1f} m E of home (fault: 50 N, -20 E)")
            c.send({"type": "fault", "device": "ublox", "kind": kind, "on": False})
            gps_now(f"ublox.{kind} off")
        c.wait_t(c.t() + 5)
        gps_now("5 s later")
        # battery faults as iNav reads them
        print("battery faults (MSP2_INAV_ANALOG 2 s after each change)")
        for label, msg in (("voltage 11.1 V", {"kind": "voltage", "on": True, "params": {"volts": 11.1}}),
                           ("voltage off", {"kind": "voltage", "on": False}),
                           ("disconnect", {"kind": "disconnect", "on": True}),
                           ("disconnect off", {"kind": "disconnect", "on": False})):
            c.send(dict(msg, type="fault", device="battery"))
            c.wait_t(c.t() + 2)
            st, _ = state_fc(run)
            print(f"  t={st['t']:.2f} {label}: iNav {inav_analog()}; model {st.get('battery')}")
        # the compass: a hard-iron offset and a detached chip, the heading at yaw 90
        print("compass faults (the model held at yaw 90)")
        c.send({"type": "constraint", "mode": "hold", "euler_deg": [0, 0, 90], "rotate_s": 2})
        c.wait_t(c.t() + 8)
        for label, msg, wait in (("offset x 150 mG", {"kind": "offset", "on": True, "params": {"x_mg": 150}}, 8),
                                 ("offset off", {"kind": "offset", "on": False}, 8),
                                 ("detached", {"kind": "detached", "on": True}, 4),
                                 ("detached off", {"kind": "detached", "on": False}, 8)):
            c.send(dict(msg, type="fault", device="qmc5883l"))
            c.wait_t(c.t() + wait)
            st, fc = state_fc(run)
            print(f"  t={st['t']:.2f} {label}: FDM yaw {st['euler_deg'][2]:.1f}, iNav {fc['attitude_deg'][2]} deg; "
                  f"blockers {fc['arming_flags']}")
        # the baro: +120 Pa (about -10 m)
        c.send({"type": "fault", "device": "spl06", "kind": "offset", "on": True, "params": {"offset_pa": 120}})
        c.wait_t(c.t() + 4)
        st, fc = state_fc(run)
        print(f"t={st['t']:.2f} spl06.offset +120 Pa: iNav's altitude {fc.get('baro_alt_m')} m (FDM "
              f"{-st['pos_ned'][2]:.2f} m)")
        c.send({"type": "fault", "device": "spl06", "kind": "offset", "on": False})
        c.wait_t(c.t() + 4)
        st, fc = state_fc(run)
        print(f"t={st['t']:.2f} spl06.offset off: iNav's altitude {fc.get('baro_alt_m')} m")
        print(f"faults events: " + "; ".join(l.split(": ", 1)[1] for l in run.lines("event fault")))
        cli_print(run, "status")
    finally:
        run.stop()


def stage_l():
    ensure_cache()
    print(f"\n## L. plane-launch (T0 {T0:g}), twice from the cached snapshot, to t = {UNTIL:g} s", flush=True)
    sums = []
    for i in (1, 2):
        run = Run(f"launch-{i}")
        try:
            host = fly("plane-launch", UNTIL)
            if i == 1:
                # after the hashed window: iNav's own battery reading and GPS against the model
                run.core.send({"type": "subscribe", "state_hz": 10})
                run.core.wait_t(UNTIL + 1)
                st = run.core.state
                a, g = inav_analog(), raw_gps()
                n, e = ned_of(g["lat"], g["lon"])
                print(f"t={st['t']:.2f} (after the window) iNav MSP2_INAV_ANALOG {a}; the model {st.get('battery')}")
                print(f"t={st['t']:.2f} iNav MSP_RAW_GPS {g}: {n:.1f} m N {e:.1f} m E; FDM {st['pos_ned'][0]:.1f} N "
                      f"{st['pos_ned'][1]:.1f} E, speed {math.hypot(*st['vel_ned'][:2]):.2f} m/s")
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
    f = os.path.join(OUT, f"plane-launch-{DATE}-run1.csv")
    r = subprocess.run([sys.executable, os.path.join(HERE, "flight_report.py"), "--plane", f, "--launch-end",
                        str(T0 + 14), "--from", str(T0 - 1), "--every", "0.5",
                        "--motor-delay", str(motor_delay_s())], capture_output=True, text=True)
    print(r.stdout + r.stderr, flush=True)
    sensors_report(f)


def motor_delay_s():
    """nav_fw_launch_motor_delay from the trainer's script (iNav's default 500 ms without it)"""
    for l in open(os.path.join(ROOT, "models/trainer/inav.cli")):
        w = l.split()
        if w[:3] == ["set", "nav_fw_launch_motor_delay", "="]:
            return int(w[3]) / 1000
    return 0.5


def sensors_report(path):
    import csv
    rows = list(csv.DictReader(open(path)))
    for r in rows:
        for k in ("t", "alt", "throttle", "motor_rpm", "batt_v", "batt_a", "batt_mah", "pos_n", "pos_e", "vel_n",
                  "vel_e", "fc_gps_fix", "fc_gps_sats", "fc_lat", "fc_lon", "fc_baro_alt", "yaw", "fc_yaw"):
            r[k] = float(r[k]) if r.get(k) not in (None, "", "nan") else math.nan
    print("## the battery over the flight (the model: 4S 4200 mAh, current = the ESC's + 0.3 A)")
    print("t,throttle,motor_rpm,batt_v,batt_a,batt_mah")
    for t in (T0 - 1, T0 + 3.5, T0 + 4.6, T0 + 5, T0 + 8, T0 + 12, T0 + 14, T0 + 16, T0 + 24, UNTIL):
        r = min(rows, key=lambda r: abs(r["t"] - t))
        print(f"{r['t']:.2f},{r['throttle']:.3f},{r['motor_rpm']:.0f},{r['batt_v']:.3f},{r['batt_a']:.2f},{r['batt_mah']:.2f}")
    fix = [r for r in rows if r["fc_gps_fix"] >= 2]         # iNav's GPS_FIX_3D = 2
    if fix:
        print(f"## iNav's GPS: 3D fix in the log from t={fix[0]['t']:.2f}, sats {fix[0]['fc_gps_sats']:.0f}")
        air = [r for r in fix if r["t"] >= T0 + 4.5]
        best = None
        for lag in [x / 100 for x in range(0, 41, 5)]:
            err = []
            for r in air:
                p = min(rows, key=lambda q: abs(q["t"] - (r["t"] - lag)))
                n, e = ned_of(r["fc_lat"], r["fc_lon"])
                err.append(math.hypot(n - p["pos_n"], e - p["pos_e"]))
            m = (max(err), sum(err) / len(err), lag)
            best = m if best is None or m[1] < best[1] else best
            if lag == 0:
                print(f"MSP_RAW_GPS vs the FDM position at the same time, in flight ({len(err)} rows): max "
                      f"{m[0]:.2f} m, mean {m[1]:.2f} m (speed ~13 m/s: the GPS epoch + MSP poll latency)")
        print(f"... against the FDM position {best[2]:.2f} s earlier (the latency): max {best[0]:.2f} m, "
              f"mean {best[1]:.2f} m")
    ba = [r for r in rows if not math.isnan(r["fc_baro_alt"]) and r["t"] >= T0 + 4]
    if ba:
        d = [r["fc_baro_alt"] - r["alt"] for r in ba]
        print(f"## iNav's altitude (MSP_ALTITUDE, baro + GPS) vs the FDM height in flight: mean {sum(d) / len(d):+.2f} m, "
              f"max |diff| {max(abs(x) for x in d):.2f} m")
    hd = [wrap(r["fc_yaw"] - r["yaw"]) for r in rows if r["t"] >= T0 + 17 and not math.isnan(r["fc_yaw"])]
    if hd:
        print(f"## heading in level flight (t >= {T0 + 17:g}): iNav - FDM mean {sum(hd) / len(hd):+.2f} deg, "
              f"max |diff| {max(abs(x) for x in hd):.2f} deg (iNav's heading is integer degrees)")


def stage_r():
    print("\n## R. RS7 liftoff (nexus-xr), FDM log every 20 ms to 45 s, twice", flush=True)
    sums = []
    for i in (1, 2):
        run = Run(f"rs7-{i}", session="rs7.json", log_ms=20)
        try:
            r = subprocess.run([sys.executable, os.path.join(HERE, "prof.py"), "run", "--session", bench("rs7.json"),
                                "--pilot", str(PILOT), "--until", "45", "--speed-from", "5", "--plan", "liftoff",
                                "--no-dump"],
                               capture_output=True, text=True)
            print("\n".join(l for l in (r.stdout + r.stderr).splitlines() if l.startswith("speed")))
        finally:
            run.stop()
        md5, n = cut(run.fdm, 45.0, os.path.join(TMP, f"rs7-{i}.cut"))
        sums.append(md5)
        print(f"run {i}: RS7 FDM log {md5}, {n} rows", flush=True)
    print(f"=> {sums} (baseline c5a2b9bb6c49: {'HOLDS' if set(sums) == {'c5a2b9bb6c49'} else 'CHANGED'})")


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
    print("\n".join(r.stdout.splitlines()[:45]))


print(f"# sensors evidence, {datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=4))):%Y-%m-%d %H:%M} "
      f"Yerevan; {subprocess.run(['git', '-C', ROOT, 'describe', '--always', '--dirty'], capture_output=True, text=True).stdout.strip()}; "
      f"load {open('/proc/loadavg').read().split()[:3] if os.path.exists('/proc/loadavg') else '?'}")
print(f"# QEMU {QEMU}; bench core/qemu/sessions/trainer.json (ports {CTL}/{MSP}); T0 {T0:g}", flush=True)
for st in STAGES.split():
    {"C": stage_c, "S": stage_s, "L": stage_l, "R": stage_r, "P": stage_p}[st]()
