#!/usr/bin/env python3
"""Merge the machine's FDM log with the MSP samples of flight.py and summarise the flight.

usage: flight_report.py <fdm.csv> <msp.csv> [merged.csv]
       flight_report.py --plane <fdm.csv> [--every S] [--from T] [--launch-end T] [--motor-delay S]

--plane: the plane FDM log alone (fb_fdm.c.inc: the FDM every fdm-log-ms plus what the core polls
over MSP - fc_armed, fc_mode, fc_flags (iNav's arming flag word), fc_roll/pitch/yaw); prints the
arming, mode and flag changes, the launch (ramp, release, motor start = iNav's detection + its
nav_fw_launch_motor_delay, --motor-delay, default iNav's 0.5 s), the climb, the level flight after --launch-end, the failsafe window
(fc_mode FAILSAFE) and a table every --every virtual s.

fdm.csv (-M nexus-xr,fdm-log=...): t,alt,vz,head_rpm,motor_rpm,throttle,dshot,roll,pitch,yaw,
servo1..4,coll,on_ground - the FDM's truth every fdm-log-ms of virtual time.
msp.csv (flight.py --csv): t,armed,flags,msp_roll,msp_pitch,msp_yaw,gov_target,gov_current - what
the firmware says, time-stamped with the virtual clock.
merged.csv: every MSP sample next to the FDM row closest in time.

Sign conventions: the FDM's roll/pitch/yaw are aviation (NED, pitch + = nose up); MSP_ATTITUDE
pitch is + = nose down (imu.c: pitch = asin(-rMat[2][0]) in the firmware's +Z-up board frame),
roll + = right side down in both, MSP yaw is the heading 0..359. The comparison below uses
-pitch for the firmware. Doublet windows (stick inputs from flight.py, 0.9 s + 1.5 s to settle)
are excluded from the "hands off" attitude numbers.
"""
import csv, sys

DOUBLETS = (40.0, 43.0, 46.0)     # flight.py plan "liftoff": t0 + 8, 11, 14 s


def hands_off(t):
    return not any(d <= t < d + 2.4 for d in DOUBLETS)


def load(path):
    with open(path) as f:
        return [{k: (float(v) if k != "flags" else v) for k, v in r.items()} for r in csv.DictReader(f)]


def main():
    fdm, msp = load(sys.argv[1]), load(sys.argv[2])
    j = 0
    merged = []
    for m in msp:
        while j + 1 < len(fdm) and abs(fdm[j + 1]["t"] - m["t"]) <= abs(fdm[j]["t"] - m["t"]):
            j += 1
        f = fdm[j]
        merged.append({"t": m["t"], "alt": f["alt"], "vz": f["vz"], "head_rpm": f["head_rpm"],
                       "roll": f["roll"], "pitch": f["pitch"], "yaw": f["yaw"],
                       "servo1": int(f["servo1"]), "servo2": int(f["servo2"]),
                       "servo3": int(f["servo3"]), "servo4": int(f["servo4"]),
                       "coll": f["coll"], "on_ground": int(f["on_ground"]), "armed": int(m["armed"]),
                       "msp_roll": m["msp_roll"], "msp_pitch": m["msp_pitch"], "msp_yaw": m["msp_yaw"],
                       "gov_target": int(m["gov_target"]), "gov_current": int(m["gov_current"])})
    if len(sys.argv) > 3:
        with open(sys.argv[3], "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(merged[0]))
            w.writeheader()
            w.writerows(merged)

    air = [r for r in fdm if not r["on_ground"]]
    if not air:
        print("never left the ground")
        return
    t_off, t_end = air[0]["t"], air[-1]["t"]
    landed = [r for r in fdm if r["t"] > t_off and r["on_ground"]]
    print(f"lift-off at t={t_off:.2f} s; airborne until {t_end:.2f} s "
          f"({t_end - t_off:.1f} virtual s{', touched down at %.2f' % landed[0]['t'] if landed else ', never touched down'})")
    print(f"altitude: max {max(r['alt'] for r in air):.2f} m, at end {air[-1]['alt']:.2f} m, "
          f"climb rate at end {air[-1]['vz']:.2f} m/s")
    print(f"FDM attitude airborne: |roll| max {max(abs(r['roll']) for r in air):.2f} deg, "
          f"|pitch| max {max(abs(r['pitch']) for r in air):.2f} deg")
    print(f"head speed airborne: {min(r['head_rpm'] for r in air):.0f}..{max(r['head_rpm'] for r in air):.0f} rpm")
    ma = [r for r in merged if r["t"] >= t_off and not r["on_ground"]]
    if ma:
        print(f"MSP_ATTITUDE airborne ({len(ma)} samples): |roll| max {max(abs(r['msp_roll']) for r in ma):.1f}, "
              f"|pitch| max {max(abs(r['msp_pitch']) for r in ma):.1f} deg; "
              f"|msp - fdm| max roll {max(abs(r['msp_roll'] - r['roll']) for r in ma):.1f}, "
              f"pitch {max(abs(-r['msp_pitch'] - r['pitch']) for r in ma):.1f} deg (msp pitch + = nose down)")
        ho = [r for r in ma if hands_off(r["t"])]
        print(f"MSP_ATTITUDE airborne, hands off ({len(ho)} samples, doublet windows excluded): "
              f"|roll| max {max(abs(r['msp_roll']) for r in ho):.1f}, "
              f"|pitch| max {max(abs(r['msp_pitch']) for r in ho):.1f} deg")
        print(f"governor airborne: target {min(r['gov_target'] for r in ma)}..{max(r['gov_target'] for r in ma)}, "
              f"current {min(r['gov_current'] for r in ma)}..{max(r['gov_current'] for r in ma)} rpm; "
              f"armed in all samples: {all(r['armed'] for r in ma)}")


# iNav fc/runtime_config.h: bit 2 ARMED; the blockers from bit 6 (the core's inav_blocker_names)
INAV_BLOCKERS = ["GEOZONE", "FS", "ANGLE", "CAL", "OVRLD", "NAV", "COMPASS", "ACC", "ARMSW", "HWFAIL",
                 "BOXFS", "PLACEHOLDER", "RX", "THR", "CLI", "CMS", "OSD", "ROLL/PITCH", "AUTOTRIM",
                 "OOM", "SETTINGFAIL", "PWMOUT", "NOPREARM", "DSHOTBEEPER", "LANDED"]
MOTOR_DELAY_S = 0.5               # nav_fw_launch_motor_delay default (iNav 9.1 settings.yaml)


def pulse_protocol(us):
    """the ESC protocol by the pulse width, as the machine tells it (nexus_xr.c esc_pulse)"""
    return "PWM" if us >= 500 else "OneShot125" if us >= 60 else "Multishot"


def flag_names(word):
    w = int(word, 16)
    return "|".join(n for i, n in enumerate(INAV_BLOCKERS) if w >> (6 + i) & 1) or "-"


def plane_report(path, every=1.0, t_from=None, launch_end=None, motor_delay=MOTOR_DELAY_S):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    num = [k for k in rows[0] if k not in ("fc_mode", "fc_flags", "fc_roll", "fc_pitch", "fc_yaw")]
    for r in rows:
        for k in num:
            r[k] = float(r[k])
        for k in ("fc_roll", "fc_pitch", "fc_yaw"):
            r[k] = float(r[k]) if r[k] else None
    print(f"# {path}: {len(rows)} rows, t {rows[0]['t']:.3f}..{rows[-1]['t']:.3f} s")
    print("## changes (virtual s): armed, mode (active boxes but ARM), arming blockers")
    prev = None
    for r in rows:
        cur = (int(r["fc_armed"]), r["fc_mode"], flag_names(r["fc_flags"]))
        if cur != prev:
            print(f"t={r['t']:8.3f} armed={cur[0]} mode={cur[1]} blockers={cur[2]}")
            prev = cur
    ramp = [r for r in rows if r["launch"] == 2]
    if ramp:
        t_start, t_rel = ramp[0]["t"], ramp[-1]["t"]
        after = [r for r in rows if r["t"] > t_rel]
        rel = after[0] if after else ramp[-1]
        print("## launch")
        print(f"ramp {t_start:.3f}..{t_rel:.3f} s: IMU specific force X (body, m/s^2) mean "
              f"{sum(r['ax'] for r in ramp) / len(ramp):.2f}, min {min(r['ax'] for r in ramp):.2f}; "
              f"released at {rel['t']:.3f} s, airspeed {rel['airspeed']:.2f} m/s, height {rel['alt']:.2f} m")
        mot = [r for r in rows if r["t"] > t_start and r["throttle"] > 0.02]
        if mot:
            m = mot[0]
            det = m["t"] - motor_delay
            print(f"motor start (ESC throttle > 2 %) at {m['t']:.3f} s = ramp start + {m['t'] - t_start:.3f} s; "
                  f"iNav's detection = motor start - nav_fw_launch_motor_delay {motor_delay:g} s = {det:.3f} s "
                  f"(ramp start + {1000 * (det - t_start):.0f} ms; row every "
                  f"{1000 * (rows[1]['t'] - rows[0]['t']):.0f} ms)")
            full = [r for r in rows if r["t"] >= m["t"] and r["motor_pulse_us"] >= 0.95 * max(x["motor_pulse_us"] for x in rows)]
            gap = [r for r in rows if t_rel < r["t"] < m["t"]]
            if gap:
                print(f"release -> motor start: {m['t'] - rel['t']:.3f} s gliding, airspeed min "
                      f"{min(r['airspeed'] for r in gap):.2f} m/s, alpha max {max(r['alpha'] for r in gap):.2f} deg, "
                      f"height {gap[-1]['alt']:.2f} m")
            if full:
                print(f"motor pulse {full[0]['motor_pulse_us']:.1f} us ({pulse_protocol(full[0]['motor_pulse_us'])}) from {full[0]['t']:.3f} s")
            end = launch_end or rows[-1]["t"]
            cl = [r for r in rows if m["t"] + 1.0 <= r["t"] <= end]
            if cl:
                print(f"climb {cl[0]['t']:.1f}..{cl[-1]['t']:.1f} s: pitch mean {sum(r['pitch'] for r in cl) / len(cl):.1f} deg, "
                      f"climb rate mean {sum(r['vz'] for r in cl) / len(cl):.2f} m/s, airspeed "
                      f"{min(r['airspeed'] for r in cl):.1f}..{max(r['airspeed'] for r in cl):.1f} m/s, "
                      f"motor {sum(r['motor_rpm'] for r in cl) / len(cl):.0f} rpm, height {cl[0]['alt']:.1f} -> {cl[-1]['alt']:.1f} m")
                pk = max(r["pitch"] for r in cl)
                pl = [r for r in cl if r["pitch"] >= pk - 1.0]
                print(f"climb plateau (pitch within 1 deg of its max) {pl[0]['t']:.1f}..{pl[-1]['t']:.1f} s: FDM pitch "
                      f"{sum(r['pitch'] for r in pl) / len(pl):.1f} deg (max {pk:.1f}), firmware "
                      f"{-sum(r['fc_pitch'] for r in pl if r['fc_pitch'] is not None) / len(pl):.1f} deg nose up, climb "
                      f"{sum(r['vz'] for r in pl) / len(pl):.2f} m/s at {sum(r['airspeed'] for r in pl) / len(pl):.1f} m/s")
    air = [r for r in rows if not r["on_ground"] and not r["held"]]
    if air:
        print(f"## airborne {air[0]['t']:.3f}..{air[-1]['t']:.3f} s: max height {max(r['alt'] for r in air):.1f} m, "
              f"airspeed {min(r['airspeed'] for r in air):.1f}..{max(r['airspeed'] for r in air):.1f} m/s, "
              f"alpha max {max(r['alpha'] for r in air):.1f} deg")
    if launch_end is not None:
        lv = [r for r in rows if r["t"] >= launch_end + 3.0]
        if lv:
            def st(k):
                v = [r[k] for r in lv]
                m = sum(v) / len(v)
                return m, (sum((x - m) ** 2 for x in v) / len(v)) ** 0.5
            print(f"## after the launch mode ({lv[0]['t']:.1f}..{lv[-1]['t']:.1f} s, {lv[-1]['t'] - lv[0]['t']:.1f} s)")
            print("roll %.1f +- %.1f deg, pitch %.1f +- %.1f deg, airspeed %.1f +- %.1f m/s, climb %.2f +- %.2f m/s, "
                  % (st("roll") + st("pitch") + st("airspeed") + st("vz")) +
                  f"height {lv[0]['alt']:.1f} -> {lv[-1]['alt']:.1f} m, heading {lv[0]['yaw']:.1f} -> {lv[-1]['yaw']:.1f} deg; "
                  f"firmware attitude roll/pitch vs FDM max |diff| "
                  f"{max(abs(r['fc_roll'] - r['roll']) for r in lv if r['fc_roll'] is not None):.1f} / "
                  f"{max(abs(-r['fc_pitch'] - r['pitch']) for r in lv if r['fc_pitch'] is not None):.1f} deg")
    fs = [r for r in rows if "FAILSAFE" in r["fc_mode"]]
    if fs:
        print(f"## failsafe (FAILSAFE box) {fs[0]['t']:.3f}..{fs[-1]['t']:.3f} s: throttle "
              f"{min(r['throttle'] for r in fs):.2f}..{max(r['throttle'] for r in fs):.2f}, roll "
              f"{min(r['roll'] for r in fs):.1f}..{max(r['roll'] for r in fs):.1f}, pitch "
              f"{min(r['pitch'] for r in fs):.1f}..{max(r['pitch'] for r in fs):.1f} deg, height "
              f"{fs[0]['alt']:.1f} -> {fs[-1]['alt']:.1f} m, modes {sorted(set(r['fc_mode'] for r in fs))}")
    print(f"## every {every} s")
    print("t,alt_m,vz_mps,airspeed_mps,alpha_deg,roll_deg,pitch_deg,yaw_deg,motor_rpm,motor_pulse_us,"
          "ail_l_us,ail_r_us,elev_us,rud_us,on_ground,held,launch,armed,mode,blockers,fc_roll,fc_pitch")
    nxt = t_from if t_from is not None else rows[0]["t"]
    for r in rows:
        if r["t"] + 1e-9 >= nxt:
            nxt += every
            print(f"{r['t']:.2f},{r['alt']:.2f},{r['vz']:.2f},{r['airspeed']:.2f},{r['alpha']:.1f},{r['roll']:.1f},"
                  f"{r['pitch']:.1f},{r['yaw']:.1f},{r['motor_rpm']:.0f},{r['motor_pulse_us']:.1f},{int(r['in1'])},"
                  f"{int(r['in2'])},{int(r['in3'])},{int(r['in4'])},{int(r['on_ground'])},{int(r['held'])},"
                  f"{int(r['launch'])},{int(r['fc_armed'])},{r['fc_mode']},{flag_names(r['fc_flags'])},"
                  f"{r['fc_roll']},{r['fc_pitch']}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--plane":
        import argparse
        ap = argparse.ArgumentParser()
        ap.add_argument("--plane", required=True)
        ap.add_argument("--every", type=float, default=1.0)
        ap.add_argument("--from", dest="t_from", type=float)
        ap.add_argument("--launch-end", type=float)
        ap.add_argument("--motor-delay", type=float, default=MOTOR_DELAY_S)
        a = ap.parse_args()
        plane_report(a.plane, a.every, a.t_from, a.launch_end, a.motor_delay)
    else:
        main()
