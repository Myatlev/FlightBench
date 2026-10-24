#!/usr/bin/env python3
"""The configurators' connect over TCP, replayed against a core's MSP port.

  configurator_handshake.py <address> [inav|rotorflight] [--hex] [--timeout=<s>] [--tabs]

<address> is what the user pastes into the configurator (the app's "Copy configurator address").
First it is parsed the way the configurator parses it (the connect either opens or fails like the
real one: iNav 9 `js/connection/connectionTcp.js` splits "host:port" on ':' and Node's net.connect
throws on the resulting port -> "Failed to open MSP connection"; RotorFlight `src/js/serial.js`
accepts only tcp://host[:port]). Then the request chain of the connect, one request at a time as
the configurator's queue does, 10 s for the whole identification (its 'connecting' timeout):
  inav:        MSP v2  API_VERSION, FC_VARIANT, FC_VERSION, NAME, BOXIDS, DATAFLASH_SUMMARY
               (serial_backend.js onOpen/onConnect; MSP.protocolVersion = PROTOCOL_V2)
  rotorflight: MSP v1  API_VERSION, FC_VARIANT, FC_VERSION, BUILD_INFO, BOARD_INFO, NAME
A request without a reply is sent again after 3 s, up to 5 times (the queue's retry).
One line per frame (t in host ms since connect; --hex: the bytes both ways). Exit 0 = connected.
--tabs (inav): then what iNav Configurator 9.1.1 asks next and decides from it -- BUILD_INFO,
BOARD_INFO, UID (serial_backend.js onValidFirmware); the "Default values" dialog's applied_defaults
(SETTING_INFO + SETTING, defaults_dialog.js init: shown unless > 0); the Status tab's load (FEATURE,
INAV_MIXER, INAV_MISC, CF_SERIAL_CONFIG; its red "Mixer not configured" when the mixer's
appliedMixerPreset is -1, tabs/setup.js); the Mixer tab's rules (INAV_SERVO_MIXER,
COMMON_MOTOR_MIXER) and the Outputs tab's servo config and its "Enable motor and servo output"
(feature PWM_OUTPUT_ENABLE). Exit 3 = the dialog or the banner shows, or the outputs are off.
--poll[=<host s>] (inav): then stay connected like the configurator on its Setup tab (after --tabs,
if given): periodicStatusUpdater every 300 ms (SENSOR_STATUS, INAV_STATUS, ACTIVEBOXES, INAV_ANALOG),
setup.js ATTITUDE every 50 ms and RAW_GPS every 250 ms, through one queue (one request in flight,
a code already queued is not queued again, 3 s timeout, 5 retries), until the time is up or SIGTERM.
One line per anomaly (a timeout, a reply to another request, a bad checksum, the link icon going
dark: no INAV_ANALOG reply for 900 ms, periodicStatusUpdater.js updateView); a summary at the end.
--ctl=<port>: the lines carry the core's virtual t too (its control server). Exit 4 = any timeout.
--reboot=<host s> (with --poll): that far into the polling, what a tab's "Save and Reboot" does
(tabs/configuration.js: MSP_SET_REBOOT, then serial_backend.js GUI.handleReconnect: close 0.1 s later,
connect again 5 s later with the connect's 10 s identification window); then polling again.
Exit 5 = the reconnect failed (the configurator shows "No configuration received within 10 seconds"
and disconnects).
"""
import signal
import re
import socket
import sys
import time

INAV = [(1, "API_VERSION"), (2, "FC_VARIANT"), (3, "FC_VERSION"), (10, "NAME"), (119, "BOXIDS"),
        (70, "DATAFLASH_SUMMARY")]
REQ_TIMEOUT, RETRIES = 3.0, 5     # serial_queue.js: connection getTimeout() at >= 57600 (TCP 115200), retryCounter
RF = [(1, "API_VERSION"), (2, "FC_VARIANT"), (3, "FC_VERSION"), (5, "BUILD_INFO"), (4, "BOARD_INFO"),
      (10, "NAME")]


def crc8_dvb_s2(crc, data):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def v1(cmd, payload=b""):
    body = bytes([len(payload), cmd]) + payload
    x = 0
    for b in body:
        x ^= b
    return b"$M<" + body + bytes([x])


def v2(cmd, payload=b""):
    body = bytes([0, cmd & 0xFF, cmd >> 8, len(payload) & 0xFF, len(payload) >> 8]) + payload
    return b"$X<" + body + bytes([crc8_dvb_s2(0, body)])


def parse_address(addr, flavour):
    """(host, port) as the configurator derives it, or (None, why) when its connect fails"""
    if flavour == "inav":
        p = addr.split(":")
        host, port = (p[0], p[1]) if len(p) >= 2 else (addr[0], "5761")
        m = re.match(r"\s*[+-]?\d+", port)          # parseInt
        if not m or not 0 <= int(m.group()) < 65536:
            return None, (f"host '{host}', port parseInt('{port}') = NaN: net.connect throws "
                          "ERR_SOCKET_BAD_PORT -> 'Failed to open MSP connection'")
        return host, int(m.group())
    m = re.match(r"^tcp://([A-Za-z0-9.-]+)(?::(\d+))?$", addr)
    if not m:
        return None, "not tcp://host[:port]: the configurator opens it as a serial port name"
    return m.group(1), int(m.group(2) or 5761)


class Reader:
    """MSP v1/v2 replies out of the TCP stream; anything else is counted as text"""
    def __init__(self, s, t0, hexdump):
        self.s, self.t0, self.hex, self.buf, self.text = s, t0, hexdump, b"", 0

    def frame(self, deadline):
        while True:
            f = self._take()
            if f:
                return f
            left = deadline - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return None
            if not d:
                raise ConnectionError("the core closed the connection")
            if self.hex:
                print(f"  {self.ms():8.1f} FC>  {d.hex(' ')}")
            self.buf += d

    def ms(self):
        return (time.time() - self.t0) * 1000

    def _take(self):
        b = self.buf
        i = b.find(b"$")
        if i < 0:
            self.text += len(b)
            self.buf = b""
            return None
        self.text += i
        b = self.buf = b[i:]
        if len(b) >= 2 and b[1:2] not in (b"M", b"X"):
            self.buf = b[1:]
            self.text += 1
            return self._take()
        if b[1:2] == b"M" and len(b) >= 5:
            n = 6 + b[3]
            if len(b) >= n:
                self.buf = b[n:]
                ok = crc8_x(b[3:n - 1]) == b[n - 1]
                return ("v1", b[2:3].decode(), b[4], b[5:n - 1], ok)
        if b[1:2] == b"X" and len(b) >= 8:
            n = 9 + b[6] + (b[7] << 8)
            if len(b) >= n:
                self.buf = b[n:]
                ok = crc8_dvb_s2(0, b[3:n - 1]) == b[n - 1]
                return ("v2", b[2:3].decode(), b[4] | b[5] << 8, b[8:n - 1], ok)
        return None


def crc8_x(data):
    c = 0
    for x in data:
        c ^= x
    return c


def show(cmd, payload):
    if cmd == 1 and len(payload) >= 3:
        return f"protocol {payload[0]} api {payload[1]}.{payload[2]}"
    if cmd in (2, 10) or cmd == 5:
        return repr(payload.decode("latin1"))
    if cmd == 3 and len(payload) >= 3:
        return ".".join(str(x) for x in payload[:3])
    return f"{len(payload)} B"


def request(s, r, enc, cmd, name, flavour, deadline, limit, hexdump, payload=b""):
    """one request as the configurator's queue sends it (3 s retry); the reply frame or None (printed)"""
    req, sends = enc(cmd, payload), 0
    while True:
        if sends == 0 or time.time() >= resend:
            if hexdump:
                print(f"  {r.ms():8.1f} >FC  {req.hex(' ')}")
            s.sendall(req)
            sends += 1
            resend = time.time() + REQ_TIMEOUT if sends <= RETRIES else deadline
        f = r.frame(min(deadline, resend))
        if f is None and time.time() < deadline:
            continue                        # the request timed out: sent again
        if f is None:
            print(f"{flavour}: {name}: no reply within the {limit:g} s timeout "
                  f"(t {r.ms():.0f} ms, {r.text} text bytes seen) -> FAILED")
            return None
        ver, d, c, body, ok = f
        if c == cmd:
            break
        print(f"  {r.ms():8.1f} other frame {ver} '{d}' cmd {c} ({len(body)} B) skipped")
    print(f"{r.ms():8.1f} ms {name:17s} {'x%d ' % sends if sends > 1 else ''}{ver} '{d}' crc {'ok' if ok else 'BAD'}: "
          f"{show(cmd, body)}")
    if d != ">" or not ok:
        print(f"{flavour}: {name}: {'error reply' if d == '!' else 'bad checksum'} -> FAILED")
        return None
    return body


def tabs(ask):
    """iNav Configurator 9.1.1 after the connect: what decides the defaults dialog, the Status tab's
    mixer banner and the Outputs tab's output switch, and the mixer the Mixer tab shows. True = the
    configurator sees a configured airplane (no dialog, no banner, outputs on)."""
    # serial_backend.js:295-313 onValidFirmware, then defaults_dialog.js:32-41 init()
    for cmd, name in ((5, "BUILD_INFO"), (4, "BOARD_INFO"), (160, "UID")):
        if ask(cmd, name) is None:
            return False
    # mspHelper.getSetting("applied_defaults"): SETTING_INFO by name, then SETTING by its index
    info = ask(0x1007, "SETTING_INFO", b"applied_defaults\0")
    if info is None:
        return False
    i = info.index(0) + 1                                   # name, pg u16, type, section, mode, min, max, index
    typ, index = info[i + 2], int.from_bytes(info[i + 13:i + 15], "little")
    val = ask(0x1003, "SETTING", bytes([0]) + index.to_bytes(2, "little"))
    if val is None:
        return False
    applied = int.from_bytes(val, "little", signed=typ in (1, 3))
    dialog = applied <= 0
    print(f"defaults dialog: applied_defaults = {applied} -> {'SHOWN' if dialog else 'not shown'} "
          "(js/defaults_dialog.js:33-36: shown unless > 0)")
    # tabs/setup.js:33-38 load chain; the banner: tabs/setup.js:318
    feat = ask(36, "FEATURE")
    mix = ask(0x2010, "INAV_MIXER")
    if feat is None or mix is None or ask(0x2003, "INAV_MISC") is None or ask(0x1009, "CF_SERIAL_CONFIG") is None:
        return False
    mask = int.from_bytes(feat[:4], "little")
    preset = int.from_bytes(mix[5:7], "little", signed=True)
    banner = preset == -1
    print(f"MSP2_INAV_MIXER: platform {mix[3]} ({['MULTIROTOR', 'AIRPLANE', 'HELICOPTER', 'TRICOPTER', 'ROVER', 'BOAT'][mix[3]] if mix[3] < 6 else '?'}), "
          f"motorstop_on_low {mix[2]}, has_flaps {mix[4]}, appliedMixerPreset {preset}")
    print(f"mixer banner: appliedMixerPreset = {preset} -> {'SHOWN' if banner else 'not shown'} "
          "(tabs/setup.js:318: 'Mixer not configured' when -1)")
    # tabs/mixer.js:38-46 (the rules), tabs/outputs.js (features, servo config)
    smix, mmix, scfg = ask(0x2020, "INAV_SERVO_MIXER"), ask(0x1005, "COMMON_MOTOR_MIXER"), ask(0x2200, "INAV_SERVO_CONFIG")
    if smix is None or mmix is None or scfg is None:
        return False
    rules = []
    for k in range(0, len(smix) - 5, 6):
        tgt, inp, rate, speed, cond = smix[k], smix[k + 1], int.from_bytes(smix[k + 2:k + 4], "little", signed=True), smix[k + 4], smix[k + 5]
        if rate:
            rules.append(f"servo {tgt} <- input {inp} {rate:+d} speed {speed} cond {cond - 256 if cond > 127 else cond}")
    motors = []
    for k in range(0, len(mmix) - 7, 8):
        t, ro, pi, ya = (int.from_bytes(mmix[k + 2 * j:k + 2 * j + 2], "little") / 1000 - 2 for j in range(4))
        if t:
            motors.append(f"motor {k // 8} thr {t:.1f} roll {ro:.1f} pitch {pi:.1f} yaw {ya:.1f}")
    rates = [f"servo {n} rate {int.from_bytes(scfg[7 * n + 6:7 * n + 7], 'little', signed=True)}"
             for n in range(len(scfg) // 7) if scfg[7 * n + 6] != 100]
    print("Mixer tab: " + "; ".join(motors + rules))
    print("Outputs tab: " + ("; ".join(rates) or "no reversed servo"))
    out_on = bool(mask >> 28 & 1)
    print(f"outputs switch: feature PWM_OUTPUT_ENABLE (bit 28) {'on' if out_on else 'OFF'} "
          "(tabs/outputs.html:11 'Enable motor and servo output')")
    return not dialog and not banner and out_on


POLL_SLOW = [(151, "SENSOR_STATUS"), (0x2000, "INAV_STATUS"), (113, "ACTIVEBOXES"), (0x2002, "INAV_ANALOG")]


def poll(s, r, enc, duration, vt, hexdump, stop=None):
    """the configurator's Setup tab + status bar: a queue fed by three intervals; True = no timeout"""
    def at():
        v = vt()
        return f"host {r.ms() / 1000:8.3f} s" + (f" t={v:.3f}" if v is not None else "")
    stop = stop or [False]
    queue, due = [], {"fast": 0.0, "gps": 0.0, "slow": 0.0}
    n = {"sent": 0, "replies": 0, "timeouts": 0, "other": 0, "bad": 0, "dark": 0}
    lat, by = [], {}
    last_analog = time.time()
    dark = False
    end = time.time() + duration
    t_start = time.time()
    print(f"poll: {at()} start (Setup tab + status bar, 3 s timeout)", flush=True)
    while not stop[0] and time.time() < end:
        now = time.time()
        for key, period, reqs in (("fast", 0.05, [(108, "ATTITUDE")]), ("gps", 0.25, [(106, "RAW_GPS")]),
                                  ("slow", 0.3, POLL_SLOW)):
            if now >= due[key]:
                due[key] = now + period
                queue.extend(q for q in reqs if q not in queue)
        if not queue:
            time.sleep(0.005)
            continue
        cmd, name = queue.pop(0)
        req, tries = enc(cmd), 0
        while not stop[0]:
            if hexdump:
                print(f"  {r.ms():8.1f} >FC  {req.hex(' ')}")
            s.sendall(req)
            n["sent"] += 1
            sent = time.time()
            got = None
            while got is None and time.time() < sent + REQ_TIMEOUT:
                f = r.frame(min(sent + REQ_TIMEOUT, time.time() + 0.1))
                if not dark and time.time() - last_analog > 0.9:
                    dark = True
                    n["dark"] += 1
                    print(f"poll: {at()} link icon dark (no INAV_ANALOG reply for 0.9 s; waiting for {name})",
                          flush=True)
                if f is None:
                    continue
                ver, d, c, body, ok = f
                if not ok:
                    n["bad"] += 1
                    print(f"poll: {at()} bad checksum on cmd {c} ({len(body)} B)", flush=True)
                elif c != cmd:
                    n["other"] += 1
                    print(f"poll: {at()} reply to {c} while waiting for {name} ({cmd}): {body.hex(' ')}", flush=True)
                else:
                    got = f
            if got:
                break
            n["timeouts"] += 1
            print(f"poll: {at()} {name} ({cmd}) no reply in {REQ_TIMEOUT:g} s (try {tries + 1})", flush=True)
            tries += 1
            if tries > RETRIES:
                break
        if not got:
            continue
        n["replies"] += 1
        dt = time.time() - sent
        lat.append(dt)
        by[name] = by.get(name, 0) + 1
        if cmd == 0x2002:
            last_analog = time.time()
            if dark:
                dark = False
                print(f"poll: {at()} link icon lit again", flush=True)
    lat.sort()
    q = (lambda x: lat[min(len(lat) - 1, int(x * len(lat)))] * 1000) if lat else (lambda x: float("nan"))
    print(f"poll: {at()} end after {time.time() - t_start:.1f} host s: {n['sent']} requests, {n['replies']} replies, "
          f"{n['timeouts']} timeouts, {n['other']} replies to another request, {n['bad']} bad checksums, "
          f"link icon dark {n['dark']}x; latency p50 {q(0.5):.0f} ms, p99 {q(0.99):.0f} ms, max "
          f"{(lat[-1] * 1000 if lat else float('nan')):.0f} ms; " + ", ".join(f"{k} {v}" for k, v in by.items()), flush=True)
    return n["timeouts"] == 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    hexdump = "--hex" in sys.argv
    limit = float(next((a.split("=")[1] for a in sys.argv if a.startswith("--timeout=")), "10"))
    if not args:
        print(__doc__)
        return 2
    addr, flavour = args[0], (args[1] if len(args) > 1 else "inav")
    host, port = parse_address(addr, flavour)
    if host is None:
        print(f"{flavour}: '{addr}': FAILED to open: {port}")
        return 1
    chain, enc = (INAV, v2) if flavour == "inav" else (RF, v1)
    t0 = time.time()

    def connect():
        s = socket.create_connection((host, port), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        r = Reader(s, t0, hexdump)
        deadline = time.time() + limit
        t1 = r.ms()
        for cmd, name in chain:
            if request(s, r, enc, cmd, name, flavour, deadline, limit, hexdump) is None:
                return s, None
        print(f"{flavour}: connected ({len(chain)}/{len(chain)} replies, {r.ms() - t1:.0f} ms, {r.text} text bytes)")
        return s, r
    s, r = connect()
    if r is None:
        return 1
    if "--tabs" in sys.argv and flavour == "inav":
        t1 = time.time()
        ok = tabs(lambda cmd, name, payload=b"": request(s, r, enc, cmd, name, flavour, t1 + limit, limit,
                                                          hexdump, payload))
        print(f"{flavour}: tabs: {'configured airplane' if ok else 'NOT as configured through the configurator'}")
        if not ok:
            return 3
    pl = next((a for a in sys.argv if a == "--poll" or a.startswith("--poll=")), None)
    if pl and flavour == "inav":
        vt = lambda: None
        ctl = next((a.split("=")[1] for a in sys.argv if a.startswith("--ctl=")), None)
        if ctl:
            import os
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../tools"))
            from core_client import Core
            core = Core(int(ctl))
            vt = lambda: core.t()
        stop = [False]
        signal.signal(signal.SIGTERM, lambda *a: stop.__setitem__(0, True))
        total = float(pl.split("=")[1]) if "=" in pl else float("inf")
        reboot = next((float(a.split("=")[1]) for a in sys.argv if a.startswith("--reboot=")), None)
        ok = poll(s, r, enc, min(total, reboot) if reboot is not None else total, vt, hexdump, stop)
        if reboot is not None and not stop[0]:
            v = vt()
            print(f"reboot: host {r.ms() / 1000:.3f} s" + (f" t={v:.3f}" if v is not None else "") +
                  ": MSP_SET_REBOOT, then the configurator's reconnect (close +0.1 s, connect +5 s, 10 s window)",
                  flush=True)
            request(s, r, enc, 68, "SET_REBOOT", flavour, time.time() + limit, limit, hexdump)
            time.sleep(0.1)
            s.close()
            time.sleep(4.9)
            v = vt()
            print(f"reconnect: host {(time.time() - t0):.3f} s" + (f" t={v:.3f}" if v is not None else ""), flush=True)
            s, r = connect()
            v = vt()
            if r is None:
                print(f"reconnect: FAILED at t={v} -> 'No configuration received within 10 seconds', the "
                      "configurator disconnects", flush=True)
                return 5
            ok = poll(s, r, enc, total - reboot - 5, vt, hexdump, stop) and ok
        if not ok:
            return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
