#!/usr/bin/env python3
"""Evidence for `reset aircraft` restarting the firmware's attitude (docs/protocol.md, `reset`).

  core/qemu/run.sh --session <session with its own ports> &
  core/qemu/scripts/reset_attitude_evidence.py --port 31264

A: arm, lift off, hold roll until the FDM is > 60 deg from level, then `reset aircraft`
   (default: FDM on the pad + MCU reset, flash kept): the firmware attitude must be within
   +-2 deg of level within 5 virtual s after the `reboot` event; then arming again after the
   arm switch is cycled low -> high.
B: the same with `"firmware":false` (teleport only), staying armed: the firmware keeps its
   wrong attitude (armed, RotorFlight's estimator trusts the accel only at dcm_kp).
Channel map (map AECR1T23): 1 roll, 2 pitch, 3 collective, 4 yaw, 5 ARM, 6 throttle switch.
"""
import argparse, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools"))
from core_client import Core  # noqa: E402

T0 = time.time()
IDLE = [1500, 1500, 1500, 1500, 1000, 1000, 1500, 1500]
SWITCH_FLAGS = {"ARMSWITCH", "BADRX"}   # what cycling the arm switch clears


def log(msg):
    print(f"[{time.time() - T0:6.1f}s] {msg}", flush=True)


def f1(v):
    if v is None:
        return "-"
    return "[" + ",".join(f"{x:.1f}" for x in v) + "]"


def att(m):
    return (f"fdm={f1(m['euler_deg'])} fw={f1(m['fc'].get('attitude_deg'))} "
            f"on_ground={m['on_ground']} alt={-m['pos_ned'][2]:.2f}")


def rc(c, **ch):
    for k, us in ch.items():
        c.cur[int(k[2:]) - 1] = us
    c.send({"type": "rc", "ch_us": c.cur})


def events(c, k, names=("reset", "reboot", "armed", "disarmed")):
    return [m for m in c.msgs[k:] if m["type"] == "event" and m["name"] in names]


def fmt_ev(evs):
    out = []
    for m in evs:
        extra = {k: v for k, v in m.items() if k not in ("type", "name", "t")}
        out.append(f"{m['name']}@{m['t']:.2f}{extra}")
    return " ".join(out)


def wait_ready(c, timeout=300):
    """fc.msp true and nothing but switches keeping the firmware from arming."""
    return c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"] and
                      set(m["fc"]["arming_flags"]) <= SWITCH_FLAGS, timeout)


def arm(c, label):
    rc(c, ch5=1000, ch6=1000)
    c.wait_t(c.t() + 1.0)
    log(f"{label} t={c.t():.2f} arm switch low: armed={c.state['fc']['armed']} "
        f"flags={c.state['fc']['arming_flags']}")
    rc(c, ch5=2000)
    m = c.wait_for(lambda m: m["type"] == "state" and m["fc"]["armed"], 20)
    log(f"{label} t={c.t():.2f} arm switch high: armed={m is not None} flags={c.state['fc']['arming_flags']}")
    return m is not None


def fly_and_roll(c, label):
    if not arm(c, label):
        return None
    rc(c, ch6=2000)
    c.wait_t(c.t() + 12.0)
    log(f"{label} t={c.t():.2f} throttle on, head rpm {c.state['rpm']:.0f}")
    for k10 in range(11):
        rc(c, ch3=1500 + 12 * k10)
        c.wait_t(c.t() + 0.2)
    m = c.wait_for(lambda m: m["type"] == "state" and -m["pos_ned"][2] > 0.5, 600)
    rc(c, ch3=1606)
    log(f"{label} t={m['t']:.2f} airborne: {att(m)}")
    c.wait_t(m["t"] + 1.0)
    rc(c, ch1=1750)
    tr, last = c.t(), 0.0
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        if m["t"] - last >= 0.1:
            last = m["t"]
            log(f"{label} t={m['t']:.2f} roll input: {att(m)}")
        if abs(m["euler_deg"][0]) > 60 or m["t"] - tr > 5:
            break
    c.wait_t(m["t"] + 0.15)      # the ~10 Hz MSP attitude catches up
    m = c.state
    log(f"{label} t={m['t']:.2f} BEFORE reset: {att(m)}")
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31264)
    a = ap.parse_args()
    c = Core(a.port)
    c.cur = list(IDLE)
    c.send({"type": "rc", "ch_us": c.cur})
    log(f"hello: {c.hello}")
    m = wait_ready(c)
    log(f"t={m['t']:.2f} fc ready: flags={m['fc']['arming_flags']} {att(m)}")

    # ---------------- A: reset aircraft (default: with the MCU reset) ----------------
    if fly_and_roll(c, "A") is None:
        log("A: arming failed")
        return 1
    k = len(c.msgs)
    rc(c, ch1=1500, ch3=1500, ch6=1000)      # sticks centred, throttle switch off, ARM left high
    c.send({"type": "reset", "what": "aircraft"})
    rb = c.wait_for(lambda m: m["type"] == "event" and m["name"] == "reboot", 30)
    log(f"A reboot event at t={rb['t']:.3f}; events: {fmt_ev(events(c, k))}")
    m = c.wait_t(rb["t"] + 0.02)
    log(f"A t={m['t']:.2f} right after: {att(m)} msp={m['fc']['msp']}")
    t_level, last = None, 0.0
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        dt = m["t"] - rb["t"]
        if m["t"] - last >= 0.5:
            last = m["t"]
            log(f"A t={m['t']:.2f} (+{dt:.2f}) {att(m)} msp={m['fc']['msp']} flags={m['fc']['arming_flags']}")
        a_ = m["fc"].get("attitude_deg")
        if t_level is None and a_ and abs(a_[0]) <= 2 and abs(a_[1]) <= 2:
            t_level = dt
            log(f"A t={m['t']:.2f} fw attitude within +-2 deg of level {dt:.2f} s after reboot: {att(m)}")
        if dt >= 5.0:
            break
    a_ = m["fc"].get("attitude_deg")
    ok = t_level is not None and m["on_ground"] and a_ and abs(a_[0]) <= 2 and abs(a_[1]) <= 2
    log(f"A at +5.00 s: {att(m)} -> {'PASS' if ok else 'FAIL'}")
    m = wait_ready(c)
    log(f"A t={m['t']:.2f} ready, ARM still high: armed={m['fc']['armed']} flags={m['fc']['arming_flags']}")
    ok = arm(c, "A re-arm")
    log(f"A re-arm after cycling the switch: {'PASS' if ok else 'FAIL'}")
    rc(c, ch5=1000)
    c.wait_t(c.t() + 1.0)

    # ---------------- B: reset aircraft, firmware:false ----------------
    if fly_and_roll(c, "B") is None:
        log("B: arming failed")
        return 1
    k = len(c.msgs)
    # stays ARMED, as when flying by hand: RotorFlight's imu.c resets its attitude on its own
    # only after a disarm (250 ms gyro quiet, then high Kp for 500 ms); armed it keeps dcm_kp
    rc(c, ch1=1500, ch3=1500, ch6=1000)
    c.send({"type": "reset", "what": "aircraft", "firmware": False})
    tb, last = c.t(), 0.0
    while True:
        m = c.wait_for(lambda m: m["type"] == "state", 30)
        if m["t"] - last >= 0.5:
            last = m["t"]
            log(f"B t={m['t']:.2f} (+{m['t'] - tb:.2f}) {att(m)} msp={m['fc']['msp']}")
        if m["t"] - tb >= 5.0:
            break
    a_ = m["fc"].get("attitude_deg") or [0, 0, 0]
    wrong = abs(a_[0]) > 2 or abs(a_[1]) > 2
    ok = wrong and m["on_ground"] and not events(c, k, ("reboot",))
    log(f"B at +5.00 s: {att(m)} fw off level={wrong}; events: {fmt_ev(events(c, k))} -> "
        f"{'PASS (bug reproduced, no reboot)' if ok else 'FAIL'}")
    rc(c, ch5=1000)
    c.wait_t(c.t() + 1.5)
    log(f"B t={c.t():.2f} 1.5 s after disarm (firmware's own disarm attitude reset): {att(c.state)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
