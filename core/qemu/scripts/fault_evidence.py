#!/usr/bin/env python3
"""Evidence for the core manifest and fault injection (docs/protocol.md `manifest`, `fault`).

  core/qemu/run.sh --session <session with its own ports> &
  core/qemu/scripts/fault_evidence.py --port 31264 --msp 31260

1 manifest: `run.sh --manifest` is valid JSON and equal to the `manifest` message that follows
  `hello`; nexus-xr slots = the app wizard's rule (WiringGraph.cs BoardInfo.Slots) applied to the
  board config here; devices with their faults; rs7 with its actuators; bad `fault` commands
  answered with `error`.
2 fly (arm, spool up, lift off, altitude loop on the collective), then in turn:
  crsf.freeze (firmware's MSP_RC constant while the sticks move, armed, no RXLOSS),
  crsf.lq 30 (receiver report + the firmware's RSSI from MSP_ANALOG),
  esc.telemetry_lost (MSP_MOTOR_TELEMETRY rpm and the governor's current head speed = MSP_DEBUG[2]
  with debug_mode GOVERNOR, against the ESC model's rpm),
  crsf.link_lost (RXLOSS/failsafe latency, recovery), then `reset aircraft`, lift off again and
  icm42688p.freeze with a small roll input (firmware attitude vs FDM attitude, what happens after).
3 on the ground: debug_mode CRSF_LINK_STATISTICS_UPLINK (CLI, save), crsf.lq 30 -> the LQ the
  firmware decoded (MSP_DEBUG[2]); debug_mode set back to GOVERNOR.
Channel map (map AECR1T23): 1 roll, 2 pitch, 3 collective, 4 yaw, 5 ARM, 6 throttle switch;
MSP_RC order roll, pitch, yaw, collective, ...
"""
import argparse, json, os, re, socket, struct, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from core_client import Core  # noqa: E402
from session import SLOTS  # noqa: E402

T0 = time.time()
IDLE = [1500, 1500, 1500, 1500, 1000, 1000, 1500, 1500]
SWITCH_FLAGS = {"ARMSWITCH", "BADRX"}
RESULTS = []
MSP_RC, MSP_ANALOG, MSP_MOTOR_TELEMETRY, MSP_DEBUG = 105, 110, 139, 254


def log(msg):
    print(f"[{time.time() - T0:6.1f}s] {msg}", flush=True)


def check(name, ok, detail):
    RESULTS.append((name, ok))
    log(f"CHECK {name}: {detail} -> {'PASS' if ok else 'FAIL'}")


def f1(v):
    return "-" if v is None else "[" + ",".join(f"{x:.1f}" for x in v) + "]"


def ang(a, b):
    return (a - b + 180.0) % 360.0 - 180.0


# ---------------- MSP on the external socket (the core forwards; its own polls are cut out) ----
class Msp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(0.2)

    def req(self, cmd, timeout=3.0):
        body = bytes([0, cmd])
        crc = 0
        for b in body:
            crc ^= b
        self.s.sendall(b"$M<" + body + bytes([crc]))
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            try:
                buf += self.s.recv(1024)
            except socket.timeout:
                continue
            i = buf.find(b"$M>")
            while i >= 0 and len(buf) >= i + 6 and len(buf) >= i + 6 + buf[i + 3]:
                n, c = buf[i + 3], buf[i + 4]
                if c == cmd:
                    return buf[i + 5:i + 5 + n]
                buf = buf[i + 6 + n:]
                i = buf.find(b"$M>")
        raise TimeoutError(f"no reply to MSP {cmd}")

    def debug(self):
        p = self.req(MSP_DEBUG)
        return list(struct.unpack(f"<{len(p) // 4}i", p[:len(p) // 4 * 4]))

    def motor_rpm(self):
        p = self.req(MSP_MOTOR_TELEMETRY)
        return struct.unpack("<I", p[1:5])[0] if p and p[0] else None

    def motor_err(self):
        """invalid-telemetry percent of motor 1 (USE_DSHOT_TELEMETRY_STATS), MSP_MOTOR_TELEMETRY"""
        p = self.req(MSP_MOTOR_TELEMETRY)
        return struct.unpack("<H", p[5:7])[0] if p and p[0] else None

    def rssi(self):
        return struct.unpack("<H", self.req(MSP_ANALOG)[3:5])[0]


def esc_replies(mon_port):
    """bidirectional-DShot replies the machine sent (HMP qom-get /machine bench)"""
    s = socket.create_connection(("127.0.0.1", mon_port))
    s.settimeout(0.3)
    out = b""
    s.sendall(b"qom-get /machine bench\n")
    end = time.time() + 2
    while time.time() < end and b"esc_replies=" not in out:
        try:
            out += s.recv(4096)
        except socket.timeout:
            pass
    s.close()
    m = re.search(rb"esc_replies=(\d+)", out)
    return int(m.group(1)) if m else None


# ---------------- core helpers ----------------
def rc(c, **ch):
    for k, us in ch.items():
        c.cur[int(k[2:]) - 1] = us
    c.send({"type": "rc", "ch_us": c.cur})


def events(c, k, names):
    return [m for m in c.msgs[k:] if m["type"] == "event" and m["name"] in names]


def errors(c, k):
    return [m for m in c.msgs[k:] if m["type"] == "error"]


def fmt_ev(evs):
    return " ".join(f"{m['name']}@{m['t']:.3f}{m.get('data', {})}" for m in evs) or "none"


def fault(c, dev, kind, on, **params):
    msg = {"type": "fault", "device": dev, "kind": kind, "on": on}
    if params:
        msg["params"] = params
    c.send(msg)


def st(m):
    v = m["vel_ned"]
    return (f"alt={-m['pos_ned'][2]:.2f} vd={v[2]:+.2f} fdm={f1(m['euler_deg'])} fw={f1(m['fc'].get('attitude_deg'))} "
            f"armed={m['fc']['armed']} flags={m['fc']['arming_flags']} faults={m['faults']} rpm={m['rpm']:.0f}")


def wait_ready(c, timeout=300):
    return c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"] and
                      set(m["fc"]["arming_flags"]) <= SWITCH_FLAGS, timeout)


class Pilot:
    """altitude loop on the collective (ch3), one correction per 0.1 s of virtual time"""

    def __init__(self, c, alt):
        self.c, self.alt, self.base, self.last = c, alt, 1600, -1.0

    def fly(self, seconds, label, every=0.25, hook=None):
        c, t0, lastlog, m = self.c, self.c.t(), -1.0, self.c.state
        while True:
            m = c.wait_for(lambda m: m["type"] == "state", 30)
            if m["t"] - self.last >= 0.1 - 1e-6:
                self.last = m["t"]
                alt, vd = -m["pos_ned"][2], m["vel_ned"][2]
                rc(c, ch3=int(max(1450, min(1720, self.base + 25 * (self.alt - alt) + 45 * vd))))
            if hook:
                hook(m)
            if every and m["t"] - lastlog >= every - 1e-6:
                lastlog = m["t"]
                log(f"{label} t={m['t']:.2f} {st(m)} ch3={c.cur[2]}")
            if m["t"] - t0 >= seconds:
                return m


def manifest_stage(c, a):
    env = dict(os.environ, QEMU="/nonexistent")     # --manifest must not start anything
    out = subprocess.run([os.path.join(ROOT, "core", "qemu", "run.sh"), "--manifest"], env=env,
                         capture_output=True, text=True, check=True).stdout
    try:
        cli = json.loads(out)
        valid = True
    except ValueError as e:
        cli, valid = None, False
        log(f"run.sh --manifest: invalid JSON: {e}")
    log(f"run.sh --manifest: {len(out)} bytes, valid JSON: {valid}")
    order = [m["type"] for m in c.msgs[:6]]
    got = c.manifest
    log(f"messages after connect: {order}")
    check("1a manifest from run.sh == manifest after hello",
          valid and got is not None and cli == got and order.index("manifest") == order.index("hello") + 1,
          f"run.sh valid={valid}, control server sent manifest={got is not None} right after hello="
          f"{order.index('manifest') == order.index('hello') + 1 if 'manifest' in order else False}, equal={cli == got}")
    # the app wizard's rule, applied here to the board config independently of manifest.py
    with open(os.path.join(ROOT, "boards", "nexus-xr", "NEXUS_XR.config"), encoding="utf-8") as f:
        cfg = f.read()
    app = []
    for prefix, pat in (("gyro", r"^resource GYRO_CS (\d+) "), ("i2c", r"^resource I2C_SCL (\d+) "),
                        ("uart", r"^resource SERIAL_(?:TX|RX) (\d+) "), ("motor", r"^resource MOTOR (\d+) "),
                        ("servo", r"^resource SERVO (\d+) ")):
        app += [f"{prefix}{n}" for n in sorted({int(x) for x in re.findall(pat, cfg, re.M)})]
    b = got["boards"][0]
    slots = [s["id"] for s in b["slots"]]
    log(f"board {b['id']} ({b['mcu']}): {len(slots)} slots: " +
        " ".join(f"{s['id']}:{s['kind']}{'(' + ','.join(s['devices']) + ')' if s.get('devices') else ''}"
                 for s in b["slots"]))
    log(f"app rule on NEXUS_XR.config: {len(app)} slots: {' '.join(app)}")
    sess = sorted({sl for _, sl in SLOTS})
    check("1b slots match the app's rule; session.py's slots are among them",
          slots == app and set(sess) <= set(slots),
          f"{len(slots)} manifest vs {len(app)} app, session.py slots {sess} all present={set(sess) <= set(slots)}")
    for d in got["devices"]:
        fl = "; ".join(f"{x['id']} ({x['label']})" + (f" params {[p['id'] for p in x['params']]}" if x.get("params") else "")
                       for x in d["faults"]) or "-"
        log(f"device {d['id']:9s} {d['category']:8s} {d['kind']:5s} implemented={d['implemented']!s:5s} "
            f"props={[p['id'] for p in d['props']]} faults: {fl}; signals {len(d['signals'])}")
    devs = {d["id"]: d for d in got["devices"]}
    want = {"crsf": {"link_lost", "freeze", "lq"}, "icm42688p": {"freeze"}, "esc": {"telemetry_lost"},
            "dps310": {"freeze"}, "msp_osd": {"link_lost"}}
    ac = {x["id"]: x for x in got["aircraft"]}
    log(f"aircraft: {got['aircraft']}")
    check("1c devices with their faults, rs7 actuators",
          all({f["id"] for f in devs[k]["faults"]} == v and devs[k]["implemented"] for k, v in want.items())
          and not devs["gps"]["implemented"] and not devs["compass"]["implemented"]
          and ac["rs7"]["actuators"] == ["swash_a", "swash_b", "swash_c", "tail", "motor"] and ac["rs7"]["fdm"] == "heli",
          f"faults {({k: sorted(f['id'] for f in devs[k]['faults']) for k in want})}, not implemented "
          f"{[d for d in devs if not devs[d]['implemented']]}, rs7 {ac['rs7']['actuators']}")
    k = len(c.msgs)
    for bad in ({"device": "nope", "kind": "x"}, {"device": "crsf", "kind": "x"}, {"device": "dps310", "kind": "drift"},
                {"device": "crsf", "kind": "lq"}, {"device": "crsf", "kind": "lq", "params": {"lq": 101}}):
        c.send(dict(type="fault", on=True, **bad))
    c.wait_t(c.t() + 0.3)
    errs = errors(c, k)
    for e in errs:
        log(f"error: {e['what']}")
    check("1d unknown device/kind and bad params -> error, nothing applied",
          len(errs) == 5 and c.state["faults"] == [] and not events(c, k, ("fault",)),
          f"{len(errs)} errors, state.faults={c.state['faults']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31264)
    ap.add_argument("--msp", type=int, default=31260)
    ap.add_argument("--mon", type=int, default=31261)
    a = ap.parse_args()
    c = Core(a.port)
    c.cur = list(IDLE)
    c.send({"type": "rc", "ch_us": c.cur})
    log(f"hello: core={c.hello.get('core')} firmware={c.hello.get('firmware')}")
    c.wait_t(0.5)
    manifest_stage(c, a)

    m = wait_ready(c)
    msp = Msp(a.msp)
    log(f"t={m['t']:.2f} fc ready: flags={m['fc']['arming_flags']} mode={m['fc']['mode']} faults={m['faults']}")
    # ---------------- 2: fly ----------------
    rc(c, ch5=1000, ch6=1000)
    c.wait_t(c.t() + 1.0)
    rc(c, ch5=2000)
    m = c.wait_for(lambda m: m["type"] == "state" and m["fc"]["armed"], 20)
    log(f"t={c.t():.2f} armed={m is not None}")
    rc(c, ch6=2000)
    c.wait_t(c.t() + 12.0)
    log(f"t={c.t():.2f} throttle on: head rpm {c.state['rpm']:.0f}, governor debug {msp.debug()[:3]} "
        f"(requested, target, current head speed), fw motor rpm {msp.motor_rpm()}")
    for k10 in range(11):
        rc(c, ch3=1500 + 12 * k10)
        c.wait_t(c.t() + 0.2)
    m = c.wait_for(lambda m: m["type"] == "state" and -m["pos_ned"][2] > 1.0, 600)
    log(f"t={m['t']:.2f} airborne: {st(m)}")
    p = Pilot(c, 2.5)
    p.fly(6.0, "hover", 0.5)

    # ---- crsf.freeze ----
    k = len(c.msgs)
    before = c.state["fc"]["rc_us"][:4]
    fault(c, "crsf", "freeze", True)
    t_fr = c.t()
    seen = []

    def rec_rc(m):
        seen.append((m["t"], m["rc_us"][0], m["fc"]["rc_us"][0], m["fc"]["rc_us"][3], m["fc"]["armed"],
                     "RXLOSS" in m["fc"]["arming_flags"] or "FAILSAFE" in m["fc"]["arming_flags"], m["rc_us"][2]))
    p.fly(0.5, "freeze", 0.25, rec_rc)
    rc(c, ch1=1580)
    p.fly(1.5, "freeze roll stick 1580", 0.25, rec_rc)
    frozen = [x for x in seen if x[0] >= t_fr + 0.3]     # MSP_RC is polled at ~10 Hz
    rc(c, ch1=1540)
    fault(c, "crsf", "freeze", False)
    m = c.wait_for(lambda m: m["type"] == "state" and m["fc"]["rc_us"][0] == 1540, 10)
    lat = (m["t"] - frozen[-1][0]) if m else None
    rc(c, ch1=1500)
    fw_roll = {x[2] for x in frozen}
    fw_coll = {x[3] for x in frozen}
    tx_roll = {x[1] for x in frozen}
    log(f"freeze: fc.rc_us[0..3] before {before}; during: pilot ch1 {sorted(tx_roll)}, firmware roll {sorted(fw_roll)}, "
        f"collective {sorted(fw_coll)} (pilot ch3 {min(x[6] for x in frozen)}..{max(x[6] for x in frozen)}), armed all={all(x[4] for x in frozen)}, "
        f"RXLOSS/FAILSAFE seen={any(x[5] for x in frozen)}; off -> firmware roll 1540 after {lat if lat is None else round(lat, 2)} s")
    check("2a crsf.freeze: firmware armed with frozen channels, no RXLOSS",
          len(fw_roll) == 1 and len(fw_coll) == 1 and 1580 in tx_roll and all(x[4] for x in frozen)
          and not any(x[5] for x in frozen) and m is not None,
          f"{len(frozen)} states over {frozen[-1][0] - frozen[0][0]:.2f} s; events {fmt_ev(events(c, k, ('fault', 'failsafe', 'disarmed')))}")
    p.fly(2.0, "hover", 0.5)

    # ---- crsf.lq ----
    k = len(c.msgs)
    r0 = msp.rssi()
    l0 = dict(c.state["fc"]["link"])
    fault(c, "crsf", "lq", True, lq=30, rssi_dbm=-95)
    ron, roff = [], []
    for i in range(8):      # the firmware averages RSSI over 16 LINK_STATISTICS frames (200 ms each)
        p.fly(0.5, "lq 30", 0)
        ron.append(msp.rssi())
    l1, r1 = dict(c.state["fc"]["link"]), ron[-1]
    log(f"lq 30 t={c.t():.2f} {st(c.state)} fc.link={l1}")
    f_on = list(c.state["faults"])
    fault(c, "crsf", "lq", False)
    for i in range(8):
        p.fly(0.5, "lq off", 0)
        roff.append(msp.rssi())
    l2, r2 = dict(c.state["fc"]["link"]), roff[-1]
    log(f"lq: fc.link before {l0} firmware MSP_ANALOG rssi {r0}/1023; with lq 30 / -95 dBm: {l1}, rssi every 0.5 s "
        f"{ron}; off: {l2}, rssi {roff}")
    check("2b crsf.lq 30: link statistics reported (receiver) and the firmware's RSSI follows",
          l1["lq"] == 30 and l1["rssi_dbm"] == -95 and l1["up"] and l2["lq"] == 100 and r1 < r0 and abs(r2 - r0) < 10
          and "crsf.lq" in f_on,
          f"state.faults {f_on}; RSSI {r0} -> {r1} -> {r2} (scale -130..0 dBm -> 0..1023: -95 dBm = "
          f"{round(35 / 130 * 1023)}, -60 dBm = {round(70 / 130 * 1023)}); events {fmt_ev(events(c, k, ('fault',)))}")

    # ---- esc.telemetry_lost ----
    k = len(c.msgs)
    samples = []

    def sample(label):
        d = msp.debug()
        s = c.state
        samples.append((label, s["t"], s["motor_rpm"], s["rpm"], msp.motor_rpm(), d[0], d[1], d[2],
                        s["fc"]["arming_flags"]))
        log(f"esc {label} t={s['t']:.2f} ESC model motor_rpm={s['motor_rpm']} head={s['rpm']:.0f} | fw MSP_MOTOR_TELEMETRY "
            f"rpm={samples[-1][4]} | governor requested={d[0]} target={d[1]} current={d[2]} | flags={s['fc']['arming_flags']} "
            f"alt={-s['pos_ned'][2]:.2f}")

    sample("before")
    n0 = esc_replies(a.mon)
    fault(c, "esc", "telemetry_lost", True)
    c.wait_t(c.t() + 0.01)
    n1 = esc_replies(a.mon)
    for i in range(6):
        p.fly(0.5, "telemetry_lost", 0)
        sample(f"lost+{0.5 * (i + 1):.1f}s")
    n2 = esc_replies(a.mon)
    err_lost = msp.motor_err()
    fault(c, "esc", "telemetry_lost", False)
    for i in range(3):
        p.fly(0.5, "telemetry back", 0)
        sample(f"back+{0.5 * (i + 1):.1f}s")
    lost = [x for x in samples if x[0].startswith("lost")]
    back = samples[-1]
    log(f"esc: machine's bidir replies {n0} before, {n1} at the fault, {n2} after 3 s of it; firmware invalid-telemetry "
        f"percent during the fault {err_lost}, after {msp.motor_err()}")
    # RotorFlight (drivers/pwm_output_dshot_shared.c) only updates the telemetry when more than
    # MIN_GCR_EDGES edges arrived: with no reply at all it keeps the last value, telemetryActive stays
    # true, and the invalid counter does not count - measured here, not asserted as the "right" behaviour.
    check("2c esc.telemetry_lost: the ESC stops answering; firmware RPM and governor speed freeze at the last value",
          n1 is not None and n2 == n1 and len({x[2] for x in lost}) > 1 and len({x[4] for x in lost}) == 1
          and len({x[7] for x in lost}) == 1 and abs(back[4] - back[2]) < 0.01 * back[2],
          f"ESC model motor_rpm during {[x[2] for x in lost]}; fw motor rpm {[x[4] for x in lost]}; governor current "
          f"head speed {[x[7] for x in lost]}; after: fw rpm {back[4]} governor {back[7]}; "
          f"events {fmt_ev(events(c, k, ('fault', 'failsafe', 'disarmed')))}")
    p.fly(2.0, "hover", 0.5)

    # ---- crsf.link_lost (in flight) ----
    k = len(c.msgs)
    s0 = c.state
    fault(c, "crsf", "link_lost", True)
    t_on = c.t()
    p.fly(1.5, "link lost", 0.25)
    ev = next(iter(x for x in events(c, k, ("failsafe",)) if x["data"]["on"]), None)
    flags_during = list(c.state["fc"]["arming_flags"])
    link_during = dict(c.state["fc"]["link"])
    log(f"link_lost at t={t_on:.2f} ({st(s0)}): failsafe event {fmt_ev([ev] if ev else [])} after "
        f"{ev['t'] - t_on if ev else float('nan'):.3f} s; fc.link during {link_during}")
    fault(c, "crsf", "link_lost", False)
    t_off = c.t()
    k2 = len(c.msgs)
    m = p.fly(4.0, "link back", 0.25)
    evo = next(iter(x for x in events(c, k2, ("failsafe",)) if not x["data"]["on"]), None)
    log(f"link back at t={t_off:.2f}: failsafe off {fmt_ev([evo] if evo else [])} after "
        f"{evo['t'] - t_off if evo else float('nan'):.3f} s; 4 s later {st(m)}")
    check("2d crsf.link_lost: RXLOSS/failsafe within the firmware's timeout, recovery when off",
          ev is not None and ev["data"]["rxloss"] and evo is not None and "RXLOSS" in flags_during
          and not link_during["up"] and link_during["lq"] == 0,
          f"failsafe on after {ev['t'] - t_on if ev else float('nan'):.3f} s, off after "
          f"{evo['t'] - t_off if evo else float('nan'):.3f} s; flags during {flags_during}; events "
          f"{fmt_ev(events(c, k, ('fault', 'failsafe', 'armed', 'disarmed', 'crash')))}")

    # ---- back in the air for the IMU fault ----
    c.send({"type": "reset", "what": "aircraft"})
    rc(c, ch3=1500)
    m = c.wait_for(lambda m: m["type"] == "state" and m["rpm"] > 1750 and m["t"] > c.t() + 1.0, 60)
    log(f"reset aircraft, t={c.t():.2f}: {st(c.state)}")
    for k10 in range(11):
        rc(c, ch3=1500 + 12 * k10)
        c.wait_t(c.t() + 0.2)
    m = c.wait_for(lambda m: m["type"] == "state" and -m["pos_ned"][2] > 1.0, 600)
    log(f"t={m['t']:.2f} airborne again: {st(m)}")
    p.last = -1.0
    p.fly(4.0, "hover", 0.5)

    # ---- icm42688p.freeze ----
    k = len(c.msgs)
    att = []
    fault(c, "icm42688p", "freeze", True)
    rc(c, ch1=1560)         # a small roll input, so the FDM attitude moves while the IMU is frozen
    p.fly(1.0, "imu frozen, roll stick 1560", 0.1,
          lambda m: att.append((m["t"], m["euler_deg"], m["fc"].get("attitude_deg"))))
    rc(c, ch1=1500)
    fault(c, "icm42688p", "freeze", False)
    t_off = c.t()
    m = p.fly(3.0, "imu back", 0.25)
    a0, a1 = att[3], att[-1]      # skip the ~0.1 s MSP attitude lag
    dfdm = max(abs(ang(a1[1][i], a0[1][i])) for i in range(3))
    dfw = max(abs(ang(a1[2][i], a0[2][i])) for i in range(3))
    log(f"imu freeze: from t={a0[0]:.2f} to {a1[0]:.2f}: FDM {f1(a0[1])} -> {f1(a1[1])} (max change {dfdm:.1f} deg), "
        f"firmware {f1(a0[2])} -> {f1(a1[2])} (max change {dfw:.1f} deg); 3 s after off (t_off={t_off:.2f}): {st(m)}")
    check("2e icm42688p.freeze: firmware attitude stops following while the FDM moves",
          dfdm > 3 * max(dfw, 0.5) and dfdm > 2,
          f"FDM moved {dfdm:.1f} deg, firmware {dfw:.1f} deg; events {fmt_ev(events(c, k, ('fault', 'crash', 'failsafe', 'disarmed')))}")

    # ---------------- 3: LQ as the firmware decoded it ----------------
    rc(c, ch6=1000, ch3=1500)
    c.wait_t(c.t() + 1.0)
    rc(c, ch5=1000)
    c.wait_t(c.t() + 2.0)
    log(f"t={c.t():.2f} disarmed={not c.state['fc']['armed']} {st(c.state)}")
    k = len(c.msgs)
    c.send({"type": "cli", "text": "set debug_mode = CRSF_LINK_STATISTICS_UPLINK"})
    c.send({"type": "cli", "text": "save"})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "reboot", 60)
    m = wait_ready(c, 300)
    c.wait_t(c.t() + 1.0)
    d0 = msp.debug()
    fault(c, "crsf", "lq", True, lq=30)
    c.wait_t(c.t() + 1.0)
    d1 = msp.debug()
    fault(c, "crsf", "lq", False)
    c.wait_t(c.t() + 1.0)
    d2 = msp.debug()
    cli = [x["text"] for x in c.msgs[k:] if x["type"] == "log" and x["src"] == "cli"]
    log(f"cli: {cli[-4:]}")
    log(f"debug_mode CRSF_LINK_STATISTICS_UPLINK: MSP_DEBUG[0..3] (RSSI1, RSSI2, LQ, rf mode) before {d0[:4]}, "
        f"lq 30 {d1[:4]}, off {d2[:4]}")
    check("3 crsf.lq 30: the firmware decodes LQ 30 from LINK_STATISTICS", d1[2] == 30 and d0[2] == 100 and d2[2] == 100,
          f"LQ {d0[2]} -> {d1[2]} -> {d2[2]}")
    c.send({"type": "cli", "text": "set debug_mode = GOVERNOR"})
    c.send({"type": "cli", "text": "save"})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "reboot", 60)
    log("debug_mode set back to GOVERNOR (saved)")
    log("SUMMARY " + " ".join(f"{n.split()[0]}:{'PASS' if ok else 'FAIL'}" for n, ok in RESULTS))
    return 0 if all(ok for _, ok in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
