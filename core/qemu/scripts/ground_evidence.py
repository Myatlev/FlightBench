#!/usr/bin/env python3
"""Evidence for ground contact without quaternion jumps (fdm/include/fb/body.h, docs/protocol.md
`crash`, `reset`): every attitude change on the ground is a rotation the firmware's gyro reads.

  core/qemu/run.sh --session <session with its own ports> &
  core/qemu/scripts/ground_evidence.py --port 31264

1: arm, lift off, yaw to ~90 deg with the rudder, land gently (collective eased down): FDM yaw and
   the firmware's yaw before/after touchdown unchanged within a few degrees, no `crash` event.
2: take off, full roll stick + negative collective into the ground: one `crash` event with data;
   within ~1 s the FDM is upright and the firmware within 5 deg of it, no reboot.
3: `reset aircraft` (default, teleport only) from there: FDM level at heading 0, the firmware's yaw
   follows to ~0, no `reboot` event; arming again works.
Channel map (map AECR1T23): 1 roll, 2 pitch, 3 collective, 4 yaw, 5 ARM, 6 throttle switch.
"""
import argparse, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools"))
from core_client import Core  # noqa: E402

T0 = time.time()
IDLE = [1500, 1500, 1500, 1500, 1000, 1000, 1500, 1500]
SWITCH_FLAGS = {"ARMSWITCH", "BADRX"}
RESULTS = []


def log(msg):
    print(f"[{time.time() - T0:6.1f}s] {msg}", flush=True)


def check(name, ok, detail):
    RESULTS.append((name, ok))
    log(f"CHECK {name}: {detail} -> {'PASS' if ok else 'FAIL'}")


def f1(v):
    return "-" if v is None else "[" + ",".join(f"{x:.1f}" for x in v) + "]"


def fw_att(m):
    return m["fc"].get("attitude_deg")


def att(m):
    v = m["vel_ned"]
    return (f"fdm={f1(m['euler_deg'])} fw={f1(fw_att(m))} on_ground={m['on_ground']} "
            f"alt={-m['pos_ned'][2]:.2f} vd={v[2]:.2f} vh={(v[0]**2 + v[1]**2) ** 0.5:.2f}")


def ang(a, b):
    """signed a - b in degrees, wrapped to -180..180"""
    return (a - b + 180.0) % 360.0 - 180.0


def rc(c, **ch):
    for k, us in ch.items():
        c.cur[int(k[2:]) - 1] = us
    c.send({"type": "rc", "ch_us": c.cur})


def events(c, k, names):
    return [m for m in c.msgs[k:] if m["type"] == "event" and m["name"] in names]


def fmt_ev(evs):
    return " ".join(f"{m['name']}@{m['t']:.2f}{m.get('data', {})}" for m in evs) or "none"


def states(c, seconds, every=0.1, label=""):
    """log states for `seconds` of virtual time; return the last one"""
    t0, last, m = c.t(), -1.0, c.state
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        if m["t"] - last >= every - 1e-6:
            last = m["t"]
            log(f"{label} t={m['t']:.2f} (+{m['t'] - t0:.2f}) {att(m)}")
        if m["t"] - t0 >= seconds:
            return m


def wait_ready(c, timeout=300):
    return c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"] and
                      set(m["fc"]["arming_flags"]) <= SWITCH_FLAGS, timeout)


def arm(c, label):
    rc(c, ch5=1000, ch6=1000)
    c.wait_t(c.t() + 1.0)
    rc(c, ch5=2000)
    m = c.wait_for(lambda m: m["type"] == "state" and m["fc"]["armed"], 20)
    log(f"{label} t={c.t():.2f} arm switch low->high: armed={m is not None} flags={c.state['fc']['arming_flags']}")
    return m is not None


def lift_off(c, label, alt):
    for k10 in range(11):
        rc(c, ch3=1500 + 12 * k10)
        c.wait_t(c.t() + 0.2)
    m = c.wait_for(lambda m: m["type"] == "state" and -m["pos_ned"][2] > alt, 600)
    log(f"{label} t={m['t']:.2f} airborne: {att(m)}")
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31264)
    a = ap.parse_args()
    c = Core(a.port)
    c.cur = list(IDLE)
    c.send({"type": "rc", "ch_us": c.cur})
    log(f"hello: core={c.hello.get('core')} firmware={c.hello.get('firmware')}")
    m = wait_ready(c)
    log(f"t={m['t']:.2f} fc ready: flags={m['fc']['arming_flags']} {att(m)}")

    # ---------------- 1: yaw to ~90, gentle landing ----------------
    if not arm(c, "1"):
        log("1: arming failed")
        return 1
    rc(c, ch6=2000)
    c.wait_t(c.t() + 12.0)
    log(f"1 t={c.t():.2f} throttle on, head rpm {c.state['rpm']:.0f}")
    lift_off(c, "1", 0.8)
    rc(c, ch3=1600)
    k = len(c.msgs)
    rc(c, ch4=1650)
    t0, last = c.t(), 0.0
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        if m["t"] - last >= 0.2:
            last = m["t"]
            log(f"1 t={m['t']:.2f} yaw input: {att(m)}")
        if abs(ang(m["euler_deg"][2], 90)) < 8 or m["t"] - t0 > 20:
            break
    rc(c, ch4=1500)
    states(c, 1.5, 0.25, "1 yaw released")
    # ease the collective down: aim for 0.3..0.6 m/s sink
    coll, last, t0 = 1600, 0.0, c.t()
    prev = c.state
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        if m["on_ground"]:
            break
        prev = m
        if m["t"] - last >= 0.1:
            last = m["t"]
            vd = m["vel_ned"][2]
            coll += -2 if vd < 0.3 else (3 if vd > 0.6 else 0)
            rc(c, ch3=coll)
            if int(m["t"] * 10) % 5 == 0:
                log(f"1 t={m['t']:.2f} descent ch3={coll}: {att(m)}")
        if m["t"] - t0 > 60:
            log("1: no touchdown in 60 s")
            return 1
    log(f"1 t={prev['t']:.2f} last airborne: {att(prev)}")
    log(f"1 t={m['t']:.2f} touchdown: {att(m)}")
    before = prev
    rc(c, ch3=1500)
    after = states(c, 2.0, 0.25, "1 on ground")
    # the ~10 Hz MSP attitude of `before` may lag the FDM by up to ~0.1 s; compare deltas
    dy_fdm = ang(after["euler_deg"][2], before["euler_deg"][2])
    dy_fw = ang(fw_att(after)[2], fw_att(before)[2])
    crash = events(c, k, ("crash",))
    check("1 gentle landing keeps heading",
          abs(dy_fdm) < 3 and abs(dy_fw) < 3 and not crash and abs(ang(after["euler_deg"][2], 90)) < 15,
          f"FDM yaw {before['euler_deg'][2]:.1f} -> {after['euler_deg'][2]:.1f} (d {dy_fdm:+.1f}), "
          f"fw yaw {fw_att(before)[2]:.1f} -> {fw_att(after)[2]:.1f} (d {dy_fw:+.1f}), "
          f"level fdm {f1(after['euler_deg'][:2])} fw {f1(fw_att(after)[:2])}, crash events: {fmt_ev(crash)}")

    # ---------------- 2: roll hard into the ground ----------------
    lift_off(c, "2", 1.5)
    rc(c, ch3=1640)
    m = c.wait_for(lambda m: m["type"] == "state" and -m["pos_ned"][2] > 2.5, 60)
    log(f"2 t={m['t']:.2f} climbing: {att(m)}")
    k = len(c.msgs)
    rc(c, ch1=2000, ch3=1350)
    t0, last = c.t(), 0.0
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        if m["t"] - last >= 0.05:
            last = m["t"]
            log(f"2 t={m['t']:.2f} roll+down: {att(m)}")
        if m["on_ground"] or m["t"] - t0 > 10:
            break
    evs = events(c, k, ("crash",))
    ev = evs[0] if evs else c.wait_for(lambda m: m["type"] == "event" and m["name"] == "crash", 5)
    log(f"2 t={m['t']:.2f} contact: {att(m)}; crash event: {fmt_ev([ev] if ev else [])}")
    rc(c, ch1=1500, ch3=1500)
    tc = ev["t"] if ev else m["t"]
    last = -1.0
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        d = m["t"] - tc
        if d - last >= 0.05 - 1e-6:
            last = d
            log(f"2 +{d:.2f} s after the crash event: {att(m)}")
        if d >= 1.0 - 1e-6:
            break
    fdm, fw = m["euler_deg"], fw_att(m)
    diff = max(abs(ang(fw[0], fdm[0])), abs(ang(fw[1], fdm[1])))
    rest = states(c, 2.0, 0.5, "2 lying")
    fdm2, fw2 = rest["euler_deg"], fw_att(rest)
    diff2 = max(abs(ang(fw2[0], fdm2[0])), abs(ang(fw2[1], fdm2[1])))
    nev = len(events(c, k, ("crash",)))
    reb = events(c, k, ("reboot",))
    check("2 crash reported, uprighted, firmware follows",
          ev is not None and nev == 1 and abs(fdm[0]) < 2 and abs(fdm[1]) < 2 and diff < 5 and diff2 < 5 and not reb,
          f"crash events {nev} data {ev.get('data') if ev else None}; 1.0 s after: fdm {f1(fdm)} fw {f1(fw)} "
          f"(|fw-fdm| {diff:.1f}); 3.0 s after: fdm {f1(fdm2)} fw {f1(fw2)} (|fw-fdm| {diff2:.1f}); "
          f"reboot events: {fmt_ev(reb)}")

    # ---------------- 3: reset aircraft (teleport only) at some heading ----------------
    rc(c, ch6=1000)
    c.wait_t(c.t() + 1.0)
    m0 = c.state
    log(f"3 t={m0['t']:.2f} before reset: {att(m0)} armed={m0['fc']['armed']}")
    k = len(c.msgs)
    c.send({"type": "reset", "what": "aircraft"})
    m = states(c, 5.0, 0.25, "3 after reset")
    evs = events(c, k, ("reset", "reboot", "armed", "disarmed"))
    fw = fw_att(m)
    reb = events(c, k, ("reboot",))
    check("3 reset aircraft: rotation the firmware follows, no reboot",
          not reb and all(abs(x) < 0.5 for x in m["euler_deg"]) and abs(ang(fw[2], 0)) < 5
          and abs(fw[0]) < 5 and abs(fw[1]) < 5 and m["pos_ned"] == [0, 0, 0],
          f"fdm yaw {m0['euler_deg'][2]:.1f} -> {m['euler_deg'][2]:.1f}, fw yaw {fw_att(m0)[2]:.1f} -> {fw[2]:.1f}, "
          f"fw {f1(fw)}, pos {m['pos_ned']}; events: {fmt_ev(evs)}; armed={m['fc']['armed']}")
    m = wait_ready(c, 30) if not m["fc"]["armed"] else m
    log(f"3 t={c.t():.2f} armed={c.state['fc']['armed']} flags={c.state['fc']['arming_flags']}")
    ok = arm(c, "3 arm")
    check("3 arming after the reset", ok, f"arm switch low->high: armed={ok} flags={c.state['fc']['arming_flags']}")
    rc(c, ch5=1000)
    c.wait_t(c.t() + 1.0)
    log("SUMMARY " + " ".join(f"{n.split()[0]}:{'PASS' if ok else 'FAIL'}" for n, ok in RESULTS))
    return 0 if all(ok for _, ok in RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
