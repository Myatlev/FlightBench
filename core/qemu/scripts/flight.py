#!/usr/bin/env python3
"""Scripted pilot for the nexus-xr bench: a fixed schedule in virtual time + MSP/HMP logging.

usage: flight.py [--plan liftoff|spool|plane-launch|ground-takeoff] [--until SECONDS] [--coll US]
                 [--t0 SECONDS] [--csv FILE] [--quiet]

The whole stick schedule is sent to the machine's pilot socket (TCP 31238) at the start, each
command with an "at" in virtual seconds, so a run is deterministic under -icount whatever the
host speed. Meanwhile the script polls, over MSP on USART1 (31235), MSP_STATUS (arming flags,
ARM box), MSP_ATTITUDE and MSP_DEBUG (debug_mode = GOVERNOR: requested/target/current head
speed), plus the machine's "bench" property over HMP (31236), and prints one compact line per
poll (and, with --csv, every MSP sample with its virtual time). It never sends
MSP_SET_ARMING_DISABLED, so the MSP connection does not block arming.

Channel map of the RS7 (map AECR1T23): 1 roll, 2 pitch, 3 collective, 4 yaw, 5 AUX1 = ARM,
6 throttle (governor switch), 7 AUX2, 8 AUX3.

Plane plans (the trainer on the speedybee-f405-wing, core/qemu/sessions/trainer.json; iNav's default
map AETR): 1 roll, 2 pitch, 3 throttle, 4 yaw, 5 ARM, 6 ANGLE, 7 NAV LAUNCH (models/trainer/inav.cli
`aux`). Besides channels a plan step can carry a `launch` (the pilot socket's {"type":"launch"} line,
docs/protocol.md) and `link` (false = the receiver loses the transmitter, the crsf.link_lost fault).
They start at --t0 (default 20 virtual s: the board configured from the snapshot, gyro calibrated);
the machine logs everything (fdm-log: FDM + the core's MSP view of the firmware), so they need no MSP
polling here (--quiet); flight_report.py --plane summarises the log.
"""
import argparse, json, socket, struct, sys, time

ARMING_FLAGS = ["NO_GYRO", "FAILSAFE", "RX_FAILSAFE", "BAD_RX_RECOVERY", "BOXFAILSAFE", "GOVERNOR",
                "RPM_SIGNAL", "THROTTLE", "ANGLE", "BOOT_GRACE_TIME", "NOPREARM", "LOAD",
                "CALIBRATING", "CLI", "CMS_MENU", "BST", "MSP", "PARALYZE", "GPS", "RESC",
                "RPMFILTER", "REBOOT_REQUIRED", "DSHOT_BITBANG", "ACC_CALIBRATION",
                "MOTOR_PROTOCOL", "OVERRIDE", "ARM_SWITCH"]
MSP_STATUS, MSP_ATTITUDE, MSP_DEBUG = 101, 108, 254


def plans(coll_us, hover_us=1640, doublet_us=100):
    """name -> list of (t_virtual_s, {channel(1-based): us})"""
    base = [(0.0, {1: 1500, 2: 1500, 3: 1500, 4: 1500, 5: 1000, 6: 1000, 7: 1500, 8: 1500})]
    spool = base + [(14.0, {5: 2000}),            # arm (throttle switch low)
                    (15.0, {6: 2000})]            # throttle switch on: governor spools up
    lift = list(spool)
    t0, ramp = 32.0, 2.0                          # collective ramp once head speed is up
    for k in range(11):
        lift.append((t0 + ramp * k / 10, {3: int(round(1500 + (coll_us - 1500) * k / 10))}))
    lift.append((t0 + 5.0, {3: hover_us}))        # back towards hover collective
    # stick doublets (0.3 s each way): the loop must bring every axis back, a symmetric
    # model would otherwise sit at zero even with a wrong-signed loop
    for t, ch in ((t0 + 8.0, 1), (t0 + 11.0, 2), (t0 + 14.0, 4)):
        lift += [(t, {ch: 1500 + doublet_us}), (t + 0.3, {ch: 1500 - doublet_us}), (t + 0.6, {ch: 1500})]
    return {"spool": spool, "liftoff": lift}


def plane_plans(t0):
    """name -> list of (t_virtual_s, {channel: us, "launch": {...}, "link": bool})"""
    base = [(0.0, {1: 1500, 2: 1500, 3: 1000, 4: 1500, 5: 1000, 6: 1000, 7: 1000, 8: 1500})]
    # NAV LAUNCH: the launch switch on before arming (iNav navigation.c: launch mode can only be
    # entered from a box turned on while disarmed), arm with the throttle low, throttle up (iNav
    # waits for it, motor idle), throw; after the launch (iNav: nav_fw_launch_motor_delay, 100 ms in
    # models/trainer/inav.cli, spin-up, climb at 18 deg, ends after nav_fw_launch_end_time or at
    # nav_fw_launch_max_altitude) launch switch off: ANGLE (ch6 on from the start),
    # sticks centred, cruise throttle, straight and level to the end
    launch = base + [(t0, {6: 2000, 7: 2000}),                    # ANGLE, NAV LAUNCH switches on
                     (t0 + 1.0, {5: 2000}),                       # arm
                     (t0 + 3.0, {3: 1700}),                       # throttle up: launch waits
                     (t0 + 4.0, {"launch": {"speed_kmh": 50, "duration_s": 0.5, "pitch_deg": 10,
                                            "heading_deg": None}}),
                     (t0 + 14.0, {7: 1000, 3: 1550})]             # launch off: ANGLE, cruise
    # ground take-off: ANGLE, arm, full throttle on the runway, rotate at 12 m/s (~2.5 s), hold the
    # climb, level off; then the receiver loses the link for 4 s in flight (iNav's failsafe)
    ground = base + [(t0, {6: 2000}),
                     (t0 + 1.0, {5: 2000}),
                     (t0 + 2.0, {3: 2000}),
                     (t0 + 4.5, {2: 1350}),                        # rotate: pitch stick back
                     (t0 + 9.0, {2: 1500, 3: 1650}),              # level off, cruise
                     (t0 + 14.0, {"link": False}),                # link lost in flight
                     (t0 + 18.0, {"link": True})]
    return {"plane-launch": launch, "ground-takeoff": ground}


def pilot_lines(t, step):
    """the pilot socket lines of one plan step"""
    ch = {str(k): v for k, v in step.items() if isinstance(k, int)}
    out = [{"at": t, "set": ch}] if ch else []
    if "launch" in step:
        out.append(dict(step["launch"], type="launch", at=t))
    if "link" in step:
        out.append({"at": t, "link": step["link"]})
    return out


class Msp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(0.2)
        self.buf = b""

    def req(self, cmd, timeout=2.0):
        body = bytes([0, cmd])
        self.s.sendall(b"$M<" + body + bytes([body[0] ^ body[1]]))
        end = time.time() + timeout
        while time.time() < end:
            try:
                self.buf += self.s.recv(1024)
            except socket.timeout:
                continue
            while True:
                i = self.buf.find(b"$M>")
                if i < 0 or len(self.buf) < i + 6 or len(self.buf) < i + 6 + self.buf[i + 3]:
                    break
                n, c = self.buf[i + 3], self.buf[i + 4]
                payload = self.buf[i + 5:i + 5 + n]
                self.buf = self.buf[i + 6 + n:]
                if c == cmd:
                    return payload
        return None


class Line:
    def __init__(self, port, timeout=2.0):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(timeout)
        self.f = self.s.makefile("rwb")

    def send(self, obj):
        self.f.write((json.dumps(obj) + "\n").encode()); self.f.flush()
        return self.f.readline().decode().strip()


def hmp(port, cmd):
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(0.3)
    out = b""
    try:
        while True:
            out += s.recv(4096)
    except socket.timeout:
        pass
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.1)
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            out += d
    except socket.timeout:
        pass
    s.close()
    txt = out.decode(errors="replace")
    i = txt.find('"')
    return txt[i + 1:txt.rfind('"')] if i >= 0 else txt.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", default="liftoff")
    ap.add_argument("--until", type=float, default=50.0)
    ap.add_argument("--doublet", type=int, default=100, help="stick doublet amplitude, us (heli plans)")
    ap.add_argument("--coll", type=int, default=1720, help="collective stick at the top of the ramp, us (the model hovers at about 1625: 3 deg of 12 at full stick)")
    ap.add_argument("--msp", type=int, default=31235)
    ap.add_argument("--mon", type=int, default=31236)
    ap.add_argument("--pilot", type=int, default=31238)
    ap.add_argument("--poll", type=float, default=0.25, help="host seconds between MSP samples")
    ap.add_argument("--print-every", type=float, default=2.0, help="virtual seconds between printed lines")
    ap.add_argument("--quiet", action="store_true",
                    help="no MSP/HMP polling, only the virtual clock (1 s host period) - for speed runs")
    ap.add_argument("--speed-from", type=float, default=40.0,
                    help="report host s per 10 virtual s from this virtual time to --until")
    ap.add_argument("--csv", help="MSP samples: t,armed,flags,msp_roll,msp_pitch,msp_yaw,gov_target,gov_current")
    ap.add_argument("--t0", type=float, default=20.0, help="plane plans: virtual s of the first step after the start")
    a = ap.parse_args()
    table = dict(plans(a.coll, doublet_us=a.doublet), **plane_plans(a.t0))
    if a.plan not in table:
        sys.exit(f"flight.py: no plan {a.plan} (plans: {', '.join(table)})")
    plan = table[a.plan]
    if a.plan in plane_plans(a.t0):
        a.quiet = True                          # the machine logs the firmware's view (fdm-log)

    for _ in range(100):
        try:
            pilot = Line(a.pilot); break
        except OSError:
            time.sleep(0.1)
    for t, step in plan:
        for line in pilot_lines(t, step):
            r = pilot.send(line)
            if not r.startswith("ok"):
                sys.exit(f"pilot: {r}")
    print(f"# plan {a.plan} queued: {plan}", flush=True)
    msp = None if a.quiet else Msp(a.msp)      # quiet: no MSP connection (the port serves one client)
    out = open(a.csv, "w") if a.csv else None
    if out:
        out.write("t,armed,flags,msp_roll,msp_pitch,msp_yaw,gov_target,gov_current\n")
    next_print = 0.0
    clock = []                                  # (virtual s, host s)
    while a.quiet:
        tv = float(pilot.send({}).split("t=")[1])
        clock.append((tv, time.time()))
        if tv >= next_print:
            next_print = tv + a.print_every * 5
            print(f"t={tv:.2f}", flush=True)
        if tv >= a.until:
            break
        time.sleep(1.0)
    while not a.quiet:
        t1 = float(pilot.send({}).split("t=")[1])
        st, att, dbg = msp.req(MSP_STATUS), msp.req(MSP_ATTITUDE), msp.req(MSP_DEBUG)
        t2 = float(pilot.send({}).split("t=")[1])
        tv = (t1 + t2) / 2                      # the three replies came between t1 and t2
        clock.append((tv, time.time()))
        armed, names, r, p, y, d = None, [], None, None, None, None
        if st and len(st) >= 16 and len(st) >= 21 + st[15]:
            armed = struct.unpack_from("<I", st, 6)[0] & 1
            # msp.c: u8 extra flight-mode byte count @15 (+ bytes), u8 flag count, u32 flags
            flags = struct.unpack_from("<I", st, 17 + st[15])[0]
            names = [n for i, n in enumerate(ARMING_FLAGS) if flags >> i & 1]
        if att and len(att) >= 6:
            r, p, y = struct.unpack("<hhh", att[:6])
        if dbg and len(dbg) >= 32:
            d = struct.unpack("<8i", dbg[:32])
        if out and None not in (armed, r, d):
            out.write(f"{tv:.3f},{armed},{'|'.join(names) or '-'},{r / 10:.1f},{p / 10:.1f},{y},{d[1]},{d[2]}\n")
            out.flush()
        if tv >= next_print or t2 >= a.until:
            next_print = tv + a.print_every
            fields = [f"t={tv:.2f} (+-{(t2 - t1) / 2:.2f})"]
            if armed is not None:
                fields.append(("ARMED" if armed else "disarmed") + " flags=" + ("|".join(names) or "-"))
            if r is not None:
                fields.append(f"att={r / 10:.1f},{p / 10:.1f},{y}")
            if d is not None:
                fields.append(f"gov tgt/cur={d[1]}/{d[2]}")
            b = dict(kv.split("=", 1) for kv in hmp(a.mon, "qom-get /machine bench").split() if "=" in kv)
            fields.append(" ".join(f"{k}={b.get(k)}" for k in ("servo_us", "dshot", "head_rpm", "alt")))
            print(" ".join(fields), flush=True)
        if t2 >= a.until:
            break
        time.sleep(a.poll)
    w = [c for c in clock if c[0] >= a.speed_from]
    if len(w) >= 2 and w[-1][0] > w[0][0]:
        print(f"speed: {10 * (w[-1][1] - w[0][1]) / (w[-1][0] - w[0][0]):.1f} host s per 10 virtual s "
              f"(virtual {w[0][0]:.1f}..{w[-1][0]:.1f} s{', MSP/HMP polling on' if not a.quiet else ', no polling'})")


if __name__ == "__main__":
    main()
