#!/usr/bin/env python3
"""Step C evidence: drives the core's control server (docs/protocol.md) through every stage
against a running core and prints one compact line per check.

  core/qemu/run.sh --session core/qemu/sessions/rs7.json &
  core/qemu/scripts/ctl_evidence.py [--dump evidence/ctl-dump.jsonl] [--elf E] [--hex H]

Stages: 1 hello/state/signals (+ 1 s dump), 2 rc -> MSP_RC and link -> failsafe,
3 pause/step/run/realtime, 4 reset aircraft/firmware/all + power + firmware/watch,
5 cli mode + events + subscribe/samples. MSP_RC is read over USART1 (--msp, 31235) by this script
while the core polls the same UART, so it also shows the two MSP masters coexisting.
"""
import argparse, json, os, shutil, socket, struct, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools"))
from core_client import Core  # noqa: E402

T0 = time.time()


def log(msg):
    print(f"[{time.time() - T0:6.1f}s] {msg}", flush=True)


def msp_req(port, cmd, timeout=5.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(bytes([ord('$'), ord('M'), ord('<'), 0, cmd, cmd]))
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            buf += s.recv(512)
        except socket.timeout:
            break
        i = buf.find(b"$M>")
        if i >= 0 and len(buf) >= i + 5 and len(buf) >= i + 6 + buf[i + 3]:
            n = buf[i + 3]
            if buf[i + 4] == cmd:
                s.close()
                return buf[i + 5:i + 5 + n]
            buf = buf[i + 6 + n:]
    s.close()
    return None


def events(c, since):
    return [m for m in c.msgs[since:] if m["type"] == "event"]


def fmt_ev(evs):
    return ", ".join(f"{e['name']}{json.dumps(e['data']) if e['data'] else ''}@{e['t']:.2f}" for e in evs)


def cli(c, text, show=6, grep=None):
    lines = c.cli(text)
    shown = [l for l in lines if l.strip() and (grep is None or l.startswith(grep))][:show]
    log(f"cli '{text}': {len(lines)} lines, fc.cli={c.state['fc']['cli']}: " + " | ".join(shown))
    return lines


def wait_msp(c, timeout=120):
    m = c.wait_for(lambda m: m["type"] == "state" and m["fc"]["msp"], timeout)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31250)
    ap.add_argument("--msp", type=int, default=31235)
    ap.add_argument("--dump")
    ap.add_argument("--elf", default=os.path.expanduser(
        "~/flightbench-spike/fw-msp/obj/main/rotorflight_STM32F7X2_OSD.elf"))
    ap.add_argument("--hex", default=os.path.expanduser(
        "~/flightbench-spike/fw-msp/obj/rotorflight_4.6.0_STM32F7X2_OSD.hex"))
    ap.add_argument("--skip-flight", action="store_true")
    a = ap.parse_args()

    # ---------------- 1: hello + state + signals ----------------
    c = Core(a.port)
    log(f"hello: {json.dumps(c.hello)}")
    log(f"signals: {len(c.signals['items'])} ids, e.g. " +
        ", ".join(i["id"] for i in c.signals["items"][::12]))
    m = wait_msp(c)
    log(f"t={m['t']:.2f} fc from MSP: {json.dumps(m['fc'])}")
    if a.dump:
        c2 = socket.create_connection(("127.0.0.1", a.port))
        f2 = c2.makefile("rb")
        n, t_first, counts = 0, None, {}
        with open(a.dump, "w") as out:
            for line in f2:
                out.write(line.decode())
                mm = json.loads(line)
                counts[mm["type"]] = counts.get(mm["type"], 0) + 1
                if mm["type"] == "state":
                    t_first = mm["t"] if t_first is None else t_first
                    if mm["t"] - t_first >= 1.0 - 1e-9:
                        break
        c2.close()
        log(f"dump {a.dump}: {counts} over t={t_first}..{mm['t']}")
    st = c.state
    log(f"state keys: {sorted(st)}; fc keys: {sorted(st['fc'])}")
    n0, t0 = c.nstate, c.t()
    c.wait_t(t0 + 2.0)
    log(f"state rate: {c.nstate - n0} states in {c.t() - t0:.2f} virtual s")

    # subscribe: samples of extra signals at 20 Hz; a request with an unknown id (or a bad type)
    # is refused whole and leaves the subscription as it was (100 Hz state, no samples)
    sigs = ["fdm.alt_m", "imu.noisy.gyro_z_dps", "imu.noisy.temp_c", "out.motor1_dshot", "rc.ch3_us"]
    c.send({"type": "subscribe", "state_hz": 20, "signals": sigs + ["no.such"]})
    err = c.wait_for(lambda m: m["type"] == "error", 5)
    log(f"subscribe unknown id -> {json.dumps(err)}")
    c.send({"type": "subscribe", "state_hz": 20, "log": "false"})
    err = c.wait_for(lambda m: m["type"] == "error", 5)
    log(f"subscribe log:\"false\" -> {json.dumps(err)}")
    n0, t0 = c.nstate, c.t()
    k = len(c.msgs)
    c.wait_t(t0 + 1.0)
    log(f"after the refused subscribes: {c.nstate - n0} states in {c.t() - t0:.2f} virtual s, "
        f"{sum(1 for m in c.msgs[k:] if m['type'] == 'samples')} samples (unchanged: 100 Hz, none)")
    c.send({"type": "subscribe", "state_hz": 20, "signals": sigs})
    smp = []
    c.drain()
    end = time.time() + 6
    while time.time() < end and len(smp) < 20:
        mm = c.wait_for(lambda m: m["type"] == "samples", 5)
        if mm:
            smp.append(mm)
    log(f"samples: {len(smp)} at t={smp[0]['t']}..{smp[-1]['t']} (20 Hz asked), last {json.dumps(smp[-1])}")
    c.send({"type": "subscribe", "state_hz": 100, "signals": []})

    # a client that connects and never reads must not slow the machine down
    lazy = socket.create_connection(("127.0.0.1", a.port))
    tv, th = c.t(), time.time()
    time.sleep(8.0)
    log(f"with a non-reading client for 8 host s: {(c.t() - tv) / (time.time() - th):.3f}x realtime, "
        f"core says {c.state['timing']['realtime_factor']}")
    lazy.close()

    # ---------------- 2: rc + link ----------------
    ch = [1100, 1200, 1300, 1400, 1000, 1000, 1700, 1800]
    c.send({"type": "rc", "ch_us": ch})
    c.wait_t(c.t() + 0.3)
    rc = msp_req(a.msp, 105)
    got = list(struct.unpack(f"<{len(rc) // 2}H", rc)) if rc else None
    log(f"rc sent {ch}; state rc_us[:8]={c.state['rc_us'][:8]}; MSP_RC from firmware[:8]={got[:8] if got else None}")
    c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 1000, 1000, 1500, 1500]})
    c.send({"type": "rc", "ch_us": [2500]})
    log(f"rc out of range -> {json.dumps(c.wait_for(lambda m: m['type'] == 'error', 5))}")

    lines = cli(c, "status")
    cli(c, "beacon RX_SET")
    cli(c, "aux 5 13 2 1700 2100 0")      # BEEPER mode on AUX3 = ch8 (map AECR1T23); aux 0 is ARM
    cli(c, "set name = BENCH")
    k = len(c.msgs)
    cli(c, "save")
    wait_msp(c)
    log(f"after save: fc.cli={c.state['fc']['cli']}, events: {fmt_ev(events(c, k))}")

    k = len(c.msgs)
    c.send({"type": "link", "up": False})
    t1 = c.t()
    c.wait_t(t1 + 4.0)
    log(f"link DOWN at t={t1:.2f}: flags={c.state['fc']['arming_flags']} link={c.state['fc']['link']}; events: {fmt_ev(events(c, k))}")
    k = len(c.msgs)
    c.send({"type": "link", "up": True, "rssi_dbm": -85, "lq": 70, "snr_db": 3})
    c.wait_t(c.t() + 2.0)
    log(f"link up: flags={c.state['fc']['arming_flags']} link={c.state['fc']['link']}; events: {fmt_ev(events(c, k))}")

    # BEEPER switch on: the Nexus-XR has no beeper pin, the firmware beeps through the ESC
    # (DShot beacon commands, allowed 3 s after the motors were enabled) -> motor_beep events
    k = len(c.msgs)
    c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 1000, 1000, 1500, 2000]})
    t1 = c.t()
    c.wait_t(t1 + 1.5)
    mode_on = c.state["fc"]["mode"]
    c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 1000, 1000, 1500, 1500]})
    c.wait_t(c.t() + 0.5)
    log(f"BEEPER switch t={t1:.2f}..{t1 + 1.5:.2f}: fc.mode {mode_on!r}; events {fmt_ev(events(c, k))}")


    # ---------------- 3: time ----------------
    c.send({"type": "time", "mode": "pause"})
    time.sleep(1.0)
    tp, np_ = c.t(), c.nstate
    time.sleep(2.0)
    log(f"pause: t={tp} -> {c.t()} after 2 host s, {c.nstate - np_} states")
    for _ in range(3):
        tb = c.t()
        c.send({"type": "time", "mode": "step", "step_s": 0.01})
        time.sleep(1.0)
        log(f"step 0.01: t {tb} -> {c.t()}")
    c.send({"type": "time", "mode": "run"})
    tr = c.t()
    time.sleep(2.0)
    log(f"run: t {tr} -> {c.t()} in 2 host s")
    for rt, sp in ((True, 1.0), (True, 0.25), (True, 0.1), (False, 1.0)):
        c.send({"type": "time", "mode": "run", "realtime": rt, "speed": sp})
        time.sleep(1.5)
        tv, th = c.t(), time.time()
        time.sleep(5.0)
        log(f"realtime={rt} speed={sp}: {c.t() - tv:.3f} virtual s in {time.time() - th:.2f} host s "
            f"(measured {(c.t() - tv) / (time.time() - th):.3f}x), core says {c.state['timing']['realtime_factor']}")

    # ---------------- 4: aircraft / firmware / power ----------------
    if not a.skip_flight:
        k = len(c.msgs)
        t = c.t()
        c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 2000, 1000]})
        c.wait_t(t + 1.0)
        c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 2000, 2000]})
        log(f"t={c.t():.2f} arm + throttle on: armed={c.state['fc']['armed']} flags={c.state['fc']['arming_flags']}")
        c.wait_t(t + 12.0)
        log(f"t={c.t():.2f} head rpm {c.state['rpm']}")
        for k10 in range(11):
            c.send({"type": "rc", "ch_us": [1500, 1500, 1500 + 12 * k10]})
            c.wait_t(c.t() + 0.2)
        m = c.wait_for(lambda m: m["type"] == "state" and -m["pos_ned"][2] > 0.5, 600)
        c.send({"type": "rc", "ch_us": [1500, 1500, 1606]})
        log(f"t={m['t']:.2f} airborne: alt={-m['pos_ned'][2]:.2f} m on_ground={m['on_ground']} euler={m['euler_deg']} "
            f"actuators={m['actuators']}")
        c.wait_t(m["t"] + 1.0)
        log(f"t={c.t():.2f} alt={-c.state['pos_ned'][2]:.2f} m, reset aircraft")
        c.send({"type": "reset", "what": "aircraft"})
        m = c.wait_t(c.t() + 0.05)
        log(f"t={m['t']:.2f} after reset aircraft: pos={m['pos_ned']} vel={m['vel_ned']} on_ground={m['on_ground']}")
        c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 1000, 1000]})
        c.wait_t(c.t() + 1.0)
        log(f"t={c.t():.2f} disarmed; events: {fmt_ev(events(c, k))}")

    k = len(c.msgs)
    c.send({"type": "reset", "what": "firmware"})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "reboot", 30)
    wait_msp(c)
    log(f"reset firmware: events {fmt_ev(events(c, k))}")
    cli(c, "get name", 3, grep="name =")
    cli(c, "status", 4)
    cli(c, "exit", 3)
    wait_msp(c)
    log(f"after exit: fc.cli={c.state['fc']['cli']}")

    k = len(c.msgs)
    c.send({"type": "power", "on": False})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "reboot", 30)
    tp = c.t()
    c.wait_t(tp + 2.0)
    s = c.state
    log(f"power off: t {tp:.2f} -> {s['t']:.2f} (FDM keeps time), fc.power={s['fc']['power']} dshot={s['dshot']} "
        f"imu.odr? motor_rpm={s['motor_rpm']}")
    rc2 = msp_req(a.msp, 101, 3.0)
    log(f"power off: MSP_STATUS over 31235 -> {'no reply' if rc2 is None else 'reply?!'}")
    c.send({"type": "power", "on": True})
    wait_msp(c)
    log(f"power on: events {fmt_ev(events(c, k))}")
    cli(c, "get name", 3, grep="name =")
    cli(c, "status", 4)
    cli(c, "exit", 1)
    wait_msp(c)

    k = len(c.msgs)
    c.send({"type": "reset", "what": "all"})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "reboot", 30)
    wait_msp(c)
    log(f"reset all: events {fmt_ev(events(c, k))}")
    cli(c, "get name", 3, grep="name =")
    cli(c, "status", 4)
    cli(c, "exit", 1)
    wait_msp(c)

    k = len(c.msgs)
    c.send({"type": "firmware", "path": a.hex})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "firmware_loaded", 30)
    wait_msp(c)
    c.wait_t(c.t() + 1.5)
    log(f"firmware {os.path.basename(a.hex)}: events {fmt_ev(events(c, k))}; logs: " +
        " | ".join(m["text"] for m in c.msgs[k:] if m["type"] == "log" and m["src"] == "core"))
    cli(c, "status", 4)
    cli(c, "exit", 1)
    wait_msp(c)
    c.send({"type": "firmware", "path": "/nonexistent.elf"})
    log(f"bad firmware path -> {json.dumps(c.wait_for(lambda m: m['type'] == 'error', 10))}")

    for bad in ({"type": "power", "on": "false"}, {"type": "power", "on": 0},
                {"type": "fault", "device": "crsf", "kind": "link_lost", "on": "true"},
                {"type": "fault", "device": "crsf", "kind": "lq", "params": [30]},
                {"type": "fault", "device": "crsf", "kind": "lq", "params": {"lq": "30"}},
                {"type": "fault", "device": "crsf", "kind": "lq", "params": {"lq": 30, "lqq": 5}}):
        k = len(c.msgs)
        c.send(bad)
        err = c.wait_for(lambda m: m["type"] == "error", 5)
        c.wait_t(c.t() + 0.2)
        log(f"{json.dumps(bad)} -> {err and err['what']}; power={c.state['fc']['power']} "
            f"faults={c.state['faults']} events={fmt_ev(events(c, k))}")

    wfile = os.path.join(tempfile.mkdtemp(prefix="fbwatch."), "fbwatch.elf")  # per run: parallel runs
    shutil.copy(a.elf, wfile)
    k = len(c.msgs)
    c.send({"type": "firmware", "path": wfile, "watch": True})
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "firmware_loaded", 30)
    wait_msp(c)
    time.sleep(2.0)
    k2 = len(c.msgs)
    os.utime(wfile)
    log("touched the watched file")
    c.wait_for(lambda m: m["type"] == "event" and m["name"] == "firmware_loaded", 30)
    wait_msp(c)
    log(f"watch: " + " | ".join(m["text"] for m in c.msgs[k2:] if m["type"] == "log" and m["src"] == "core")
        + f"; events {fmt_ev(events(c, k2))}")
    c.send({"type": "firmware", "path": wfile, "watch": False})
    log(f"all messages other than state/samples this run: {len(c.msgs)}; events total "
        f"{sum(1 for m in c.msgs if m['type'] == 'event')}")
    log(f"final state: {json.dumps(c.state)}")


if __name__ == "__main__":
    main()
