#!/usr/bin/env python3
"""REFERENCE IMPLEMENTATION: the core resolves benches itself (-M nexus-xr,session=<bench>; the C port
is devices/src/session.c) and run.sh no longer calls this on the launch path. Kept as the reference
the C resolver is checked against (core/qemu/tests/test_session_equiv.sh: same report, warnings,
errors, blob and manifest bytes on every bench); a change of rules goes into both.

The wiring model: board x devices x assembly, the one table behind the manifest, the session
check, the machine's device instantiation, the actuator map into the FDM and fault availability.

Three inputs, each knowing only its own side:
- board: its pads and onboard (soldered) devices. On a RotorFlight board (nexus-xr) firmware ids,
  pins, timers, DMA and buses come from the unified config (`resource`/`timer`/`dma`/`set` lines,
  `config_pads`); printed labels, onboard devices (`detachable`) and extra kind flags from
  `boards/<id>/board.json` (labels are not in the config). A board configured by a CLI script
  (board.json `"config": "cli"`, the iNav speedybee-f405-wing) has no unified config: its pads come
  from board.json alone (`kind`, `group`, `pins`, `bus`, `timer`, `serial`, ...). A pad is addressed
  by its firmware id (`uart4`, `servo1`, `gyro1`) or its label (`PORT-A`, `TAIL`). A pad of kind
  `output` (iNav's TIM_USE_OUTPUT_AUTO) takes a motor or a servo port; every other kind must match.
- devices: the C descriptors (devices/include/fb/device.h, printed by devices/build/fb_manifest):
  ports with pad kind and direction (ESC: signal/motor in, tlm/uart out, rpm/capture out).
- assembly: the session's `wiring`, names only:
    {"device":"esc","port":"signal","pad":"motor1"}, {"device":"esc","port":"tlm","pad":"TELEM"},
    {"device":"servo","port":"pwm","pad":"TAIL","actuator":"tail","reversed":false},
    {"device":"icm42688p","detached":true}            (remove a detachable onboard device)
  The new form (entries with pad/port/detached) is complete and strict: exactly the wired devices
  and the onboard ones not detached exist, every problem is an error.
  The old form {"device","slot",...} (what the app writes today; slot = pad id, port = the device's
  primary port; nexus-xr benches only) keeps the behaviour from before the wiring model: the
  receiver (uart5, or the
  modelled UART the list puts it on), the ESC on motor1 and servo1..4 are assumed present, the list
  only adds and configures devices; the onboard gyro it omits is off (the old `imu=off` rule); an
  entry the machine cannot place (unknown device or pad, wrong kind, pad taken) is a warning and
  left out. Props and actuators are checked as strictly as in the new form. A list mixing both
  forms is rejected.

What the QEMU machine of each board can model behind which pad is its `modelled` in MACHINES below
(the machine's hardware, one line per device port); everything else is validated and then left out
with a warning.
"""
import glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# The emulated boards (the C port's BOARDS table in devices/src/session.c), in manifest order:
#   mcu             the MCU when board.json does not name it
#   dir             the board's data directory (board.json, its config)
#   default_config  the bench's board_config when it has none (a unified config: the blob's base);
#                   None = a CLI board, whose base script is board.json's `defaults_cli`
#   modelled        (device, port) -> the pads its QEMU machine models that device port on
#   servo_pads      the servo outputs in -M servos=<mask> bit order (bit i = servo_pads[i])
#   fdm             the aircraft FDM types the machine runs on this board (aircraft.json `fdm`)
#   legacy          the machine existed before the wiring model: the old slot form is accepted and a
#                   bench without wiring keeps the machine's own defaults (no device options)
MACHINES = {
    # core/qemu/hw/arm/nexus_xr.c: IMU on SPI1 (CS A04, INT1 B08; -M imu=on|off), CRSF receiver on any
    # USART with TX+RX except USART1, which is the MSP/CLI host socket (-M crsf=uartN|off), DShot ESC
    # with bidirectional eRPM on MOTOR 1 = A09 TIM1_CH2, DMA2 stream 6 (-M esc=on|off), servo outputs
    # SERVO 1..4 = TIM3_CH1..3, TIM2_CH1 (-M servos=<mask>), the DPS310/SPL06 baro on I2C3 at 0x76
    # (-M baro=on|off), the MSP DisplayPort OSD device (HD VTX) on any USART with TX+RX except USART1
    # (-M osd=uartN|off).
    "nexus-xr": {
        "mcu": "STM32F722", "dir": "boards/nexus-xr", "default_config": "boards/nexus-xr/NEXUS_XR.config",
        "modelled": {
            ("icm42688p", "spi"): ("gyro1",),
            ("dps310", "i2c"): ("i2c3",),
            ("crsf", "uart"): ("uart3", "uart4", "uart5", "uart6"),      # (uart2 is RX-only on the Nexus-XR)
            ("esc", "signal"): ("motor1",),
            ("servo", "pwm"): ("servo1", "servo2", "servo3", "servo4"),
            ("msp_osd", "uart"): ("uart3", "uart4", "uart5", "uart6"),
        },
        "servo_pads": ("servo1", "servo2", "servo3", "servo4"),
        "fdm": ("heli",), "legacy": True,
    },
    # the STM32F405 machine for iNav's SPEEDYBEEF405WING (boards/speedybee-f405-wing): IMU on SPI1
    # (CS A04, polled, no INT; -M imu=on|off), CRSF receiver on USART1..UART5 (USART6 is the MSP/CLI
    # host socket; -M crsf=uartN|off), the ESC on S1 (-M esc=on|off), servos on S1..S11
    # (-M servos=<mask>), the MAX7456 OSD on SPI2 (CS B12; registers and display memory, no video:
    # iNav's driver polls it at boot), the SPL06 baro on I2C1 at 0x76 (-M baro=on|off, baro-props=),
    # a QMC5883L compass on I2C1 at 0x0D (-M mag=on|off, mag-props=), a u-blox GPS on USART1..UART5
    # (-M gps=uartN|off, gps-props=), a battery on the VBAT/CURRENT pads (ADC1; -M battery=on|off,
    # battery-props=), the MSP DisplayPort OSD device (HD VTX) on USART1..UART5 (-M osd=uartN|off).
    # The SD card is not modelled.
    "speedybee-f405-wing": {
        "mcu": "STM32F405", "dir": "boards/speedybee-f405-wing", "default_config": None,
        "modelled": {
            ("icm42688p", "spi"): ("gyro1",),
            ("spl06", "i2c"): ("i2c1",),
            ("qmc5883l", "i2c"): ("i2c1",),
            ("crsf", "uart"): ("uart1", "uart2", "uart3", "uart4", "uart5"),
            ("ublox", "uart"): ("uart1", "uart2", "uart3", "uart4", "uart5"),
            ("esc", "signal"): ("output1",),
            ("servo", "pwm"): tuple(f"output{i}" for i in range(1, 12)),
            ("max7456", "spi"): ("osd1",),
            ("battery", "power"): ("batt1",),
            ("msp_osd", "uart"): ("uart1", "uart2", "uart3", "uart4", "uart5"),
        },
        "servo_pads": tuple(f"output{i}" for i in range(1, 12)),
        "fdm": ("plane",), "legacy": False,
    },
}
# the nexus-xr's (old imports: session.SLOTS, fault_evidence.py)
MODELLED = MACHINES["nexus-xr"]["modelled"]
SERVO_PADS = MACHINES["nexus-xr"]["servo_pads"]
DEFAULT_ACTUATORS = ["swash_a", "swash_b", "swash_c", "tail", "motor"]   # servo1..4, motor1
ACTUATOR_RE = re.compile(r"[A-Za-z0-9_]+")
ENTRY_KEYS = {"device", "port", "pad", "slot", "detached"}
NEW_KEYS = {"pad", "port", "detached"}
# the old form's defaults (the machine before the wiring model): receiver, ESC, servos
OLD_RX_PAD, OLD_ESC_PAD = "uart5", "motor1"

# the old `slots` (the app's wizard rule, app/scripts/WiringGraph.cs BoardInfo.Slots): these pad
# groups in this order
SLOT_GROUPS = ("gyro", "i2c", "uart", "motor", "servo")
# board.json pad data keys (a CLI board's pads come from board.json alone)
PAD_KEYS = ("group", "pins", "bus", "timer", "dma", "serial")
MAX_PINS = 3
BEEPER_RE = re.compile(r"[A-Z][0-9]{2}(:inv)?")


def spec_of(board):
    """The MACHINES entry of a load_board() result (nexus-xr for a board dict without one)."""
    return (board or {}).get("spec") or MACHINES["nexus-xr"]


def kind_fits(pad_kind, port_kind):
    """A port plugs into a pad of its own kind; an `output` pad takes a motor or a servo port."""
    return pad_kind == port_kind or (pad_kind == "output" and port_kind in ("motor", "servo"))


class WiringError(Exception):
    """The session's wiring is rejected; str() lists every problem, one per line."""


# ---- board ----------------------------------------------------------------------------------

def _parse_config(cfg):
    res, timers, dmas = {}, {}, {}
    lines = cfg.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^resource (\w+) (\d+) (\w+)\s*$", line)
        if m and m.group(3).upper() != "NONE":
            res[(m.group(1).upper(), int(m.group(2)))] = m.group(3).upper()
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        m = re.match(r"^timer (\w+) AF\d+", line)
        c = re.match(r"^# pin \w+: (TIM\d+) CH(\d+)", nxt)
        if m and c:
            timers[m.group(1).upper()] = f"{c.group(1)}_CH{c.group(2)}"
        m = re.match(r"^dma pin (\w+) \d+", line)
        c = re.match(r"^# pin \w+: (DMA\d) Stream (\d+) Channel (\d+)", nxt)
        if m and c:
            dmas[m.group(1).upper()] = f"{c.group(1)}_S{c.group(2)}_CH{c.group(3)}"
    sets = {k.lower(): v for k, v in re.findall(r"^set (\w+) = (\S+)", cfg, re.M)}
    return res, timers, dmas, sets


def config_pads(cfg):
    """Pads from a unified config, in the old slot order (gyro, i2c, uart, motor, servo), then the
    ones the old rule did not know (freqN = RPM capture inputs, flashN = the blackbox flash)."""
    res, timers, dmas, sets = _parse_config(cfg)

    def nums(*names):
        return sorted({n for (k, n) in res if k in names})

    def timed(p, pin):
        if pin in timers:
            p["timer"] = timers[pin]
        if pin in dmas:
            p["dma"] = dmas[pin]
        return p

    pads = []
    for n in nums("GYRO_CS"):
        p = {"id": f"gyro{n}", "kind": "spi", "group": "gyro", "pins": {"cs": res[("GYRO_CS", n)]}}
        if ("GYRO_EXTI", n) in res:
            p["pins"]["int"] = res[("GYRO_EXTI", n)]
        if f"gyro_{n}_spibus" in sets:
            p["bus"] = f"spi{sets[f'gyro_{n}_spibus']}"
        pads.append(p)
    for n in nums("I2C_SCL"):
        pins = {"scl": res[("I2C_SCL", n)]}
        if ("I2C_SDA", n) in res:
            pins["sda"] = res[("I2C_SDA", n)]
        pads.append({"id": f"i2c{n}", "kind": "i2c", "group": "i2c", "pins": pins, "bus": f"i2c{n}"})
    for n in nums("SERIAL_TX", "SERIAL_RX"):
        pins, flags = {}, []
        if ("SERIAL_TX", n) in res:
            pins["tx"] = res[("SERIAL_TX", n)]
        if ("SERIAL_RX", n) in res:
            pins["rx"] = res[("SERIAL_RX", n)]
        if "tx" not in pins:
            flags.append("rx_only")
        if "rx" not in pins:
            flags.append("tx_only")
        pads.append({"id": f"uart{n}", "kind": "uart", "group": "uart", "pins": pins, "flags": flags,
                     "serial": n - 1})      # the CLI's `serial <n>` index
    for n in nums("MOTOR"):
        pin = res[("MOTOR", n)]
        pads.append(timed({"id": f"motor{n}", "kind": "motor", "group": "motor", "pins": {"signal": pin}}, pin))
    for n in nums("SERVO"):
        pin = res[("SERVO", n)]
        pads.append(timed({"id": f"servo{n}", "kind": "servo", "group": "servo", "pins": {"signal": pin}}, pin))
    for n in nums("FREQ"):
        pin = res[("FREQ", n)]
        pads.append(timed({"id": f"freq{n}", "kind": "capture", "pins": {"in": pin}}, pin))
    for n in nums("FLASH_CS"):
        p = {"id": f"flash{n}", "kind": "spi", "pins": {"cs": res[("FLASH_CS", n)]}}
        if "flash_spi_bus" in sets:
            p["bus"] = f"spi{sets['flash_spi_bus']}"
        pads.append(p)
    for p in pads:
        p.setdefault("flags", [])
    return pads


def slots_of(pads):
    """The old manifest `slots` (what app/scripts/WiringGraph.cs BoardInfo.Slots derives)."""
    out = []
    for g in SLOT_GROUPS:
        for p in pads:
            if p.get("group") == g:
                s = {"id": p["id"], "kind": p["kind"]}
                if p.get("devices"):
                    s["devices"] = p["devices"]
                out.append(s)
    return out


def board_error(board_id):
    return WiringError(f"board {board_id!r}: not emulated (boards: {', '.join(MACHINES)})")


def _pad_data(p, o):
    """board.json pad data keys onto the pad; WiringError on a bad value"""
    for k in PAD_KEYS:
        if k not in o:
            continue
        v = o[k]
        if k == "pins":
            ok = isinstance(v, dict) and len(v) <= MAX_PINS and all(isinstance(x, str) for x in v.values())
        elif k == "serial":
            ok = isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 255
        else:
            ok = isinstance(v, str)
        if not ok:
            raise WiringError(f"board.json pad {p['id']}: bad {k} {v!r}")
        p[k] = dict(v) if k == "pins" else v


def load_board(board_id="nexus-xr", config_path=None, board_json=None, warn=None):
    """Pads (config + board.json, or board.json alone on a CLI board) with labels, flags, onboard
    devices and what the machine models; the board's machine, config kind, MSP UART, beeper."""
    if not isinstance(board_id, str) or board_id not in MACHINES:
        raise board_error(board_id)
    spec = MACHINES[board_id]
    d = os.path.join(ROOT, spec["dir"])
    bj_path = board_json or os.path.join(d, "board.json")
    with open(bj_path, encoding="utf-8") as f:
        bj = json.load(f)
    cli = bj.get("config") == "cli"
    cfg_path = "" if cli else config_path or os.path.join(d, bj.get("config", ""))
    cfg = ""
    if cfg_path and os.path.exists(cfg_path):
        with open(cfg_path, encoding="utf-8", errors="replace") as f:
            cfg = f.read()
    pads = config_pads(cfg)
    by_id = {p["id"]: p for p in pads}
    note = warn or (lambda m: None)
    if re.search(r"^(timer|dma pin) \w+", cfg, re.M) and not re.search(r"^# pin \w+:", cfg, re.M):
        note(f"board config {cfg_path}: no '# pin' comments (a dump without them): pad timers and DMA are unknown")
    for o in bj.get("pads", []):
        p = by_id.get(o["id"])
        if p is None:
            if "kind" not in o:
                note(f"board.json pad {o['id']}: not in the board config, left out")
                continue
            p = {"id": o["id"], "kind": o["kind"], "flags": []}
            pads.append(p)
            by_id[p["id"]] = p
        _pad_data(p, o)
        p["label"] = o.get("label")
        p["flags"] = p["flags"] + [x for x in o.get("flags", []) if x not in p["flags"]]
        for k in ("hint", "note"):
            if k in o:
                p[k] = o[k]
    for ob in bj.get("onboard", []):
        p = by_id.get(ob["pad"])
        if p is None:
            note(f"board.json onboard {ob['device']} on {ob['pad']}: no such pad in the board config, left out")
            continue
        p.update(onboard=True, device=ob["device"], detachable=bool(ob.get("detachable", False)))
        if ob.get("part"):
            p["part"] = ob["part"]
    seen = {}
    for p in pads:
        p.setdefault("label", None)
        p.setdefault("onboard", False)
        mod = sorted({dev for (dev, _), ps in spec["modelled"].items() if p["id"] in ps})
        if mod:
            p["devices"] = mod
        if p["label"]:
            k = p["label"].casefold()
            if k in seen or (k in {q["id"].casefold() for q in pads} and k != p["id"].casefold()):
                raise WiringError(f"board.json: label {p['label']!r} is not unique")
            seen[k] = p["id"]
    machine, msp, beeper, dcli = (bj.get("machine", board_id), bj.get("msp_uart", "uart1"), bj.get("beeper"),
                                  bj.get("defaults_cli"))
    if not isinstance(machine, str) or not machine:
        raise WiringError(f"board.json: machine {machine!r}: need the QEMU machine name")
    # the MSP UART: a uart pad of a CLI board (a unified config may leave any pad out)
    if not isinstance(msp, str) or not msp or (cli and (msp not in by_id or by_id[msp]["kind"] != "uart")):
        raise WiringError(f"board.json: msp_uart {msp!r} is not a uart pad")
    if beeper is not None and not (isinstance(beeper, str) and BEEPER_RE.fullmatch(beeper)):
        raise WiringError(f"board.json: beeper {beeper!r}: need a pin like 'C15' or 'C15:inv'")
    fam = bj.get("firmware_family", "rotorflight")
    if not isinstance(fam, str) or not fam:
        raise WiringError(f"board.json: firmware_family {fam!r}: need a name like 'rotorflight' or 'inav'")
    if dcli is not None and not isinstance(dcli, str):
        raise WiringError(f"board.json: defaults_cli {dcli!r}: need a file name")
    return {"id": board_id, "name": bj.get("name", board_id), "mcu": bj.get("mcu", spec["mcu"]),
            "pads": pads, "unmapped": bj.get("unmapped", []), "spec": spec, "machine": machine,
            "config": "cli" if cli else "unified", "msp_uart": msp, "beeper": beeper,
            "defaults_cli": os.path.join(d, dcli) if dcli else None, "firmware_family": fam}


def pad_public(p):
    """A pad as the manifest lists it."""
    out = {k: p[k] for k in ("id", "label", "kind", "flags", "pins", "bus", "timer", "dma", "serial") if k in p}
    out["onboard"] = p["onboard"]
    if p["onboard"]:
        out["device"] = p["device"]
        out["detachable"] = p["detachable"]
        if p.get("part"):
            out["part"] = p["part"]
    for k in ("hint", "note", "devices"):
        if p.get(k):
            out[k] = p[k]
    return out


# ---- devices, aircraft ----------------------------------------------------------------------

def device_catalog():
    """The C descriptors as JSON (devices/build/fb_manifest, rebuilt when a source is newer)."""
    tool = os.path.join(ROOT, "devices", "build", "fb_manifest")
    src = [os.path.join(ROOT, "devices", p) for p in ("tools/fb_manifest.c", "Makefile")] + \
        glob.glob(os.path.join(ROOT, "devices", "src", "*.c")) + \
        glob.glob(os.path.join(ROOT, "devices", "include", "fb", "*.h"))
    if not os.path.exists(tool) or os.path.getmtime(tool) < max(os.path.getmtime(p) for p in src):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "devices"), "tools"], check=True,
                       stdout=sys.stderr)
    return json.loads(subprocess.run([tool], check=True, capture_output=True, text=True).stdout)


def aircraft_path(ref):
    """Session `aircraft` ("models/rs7/aircraft.json", "rs7") -> absolute aircraft.json path."""
    ref = (ref or "models/rs7/aircraft.json").replace("\\", "/")
    if "/" not in ref and not ref.endswith(".json"):
        ref = f"models/{ref}/aircraft.json"
    ref = os.path.expanduser(ref)
    p = ref if os.path.isabs(ref) else os.path.join(ROOT, ref)
    if not os.path.exists(p):
        # the app writes its own repo path (another machine: "D:/.../models/rs7/aircraft.json"):
        # the model is known by its directory name, as before the aircraft file was read
        cand = os.path.join(ROOT, "models", os.path.basename(os.path.dirname(ref)) or ref, "aircraft.json")
        if os.path.exists(cand):
            return cand
    return p


# aircraft.json numbers: key -> (lowest, highest, lowest excluded); the machine's aircraft_load
# (core/qemu/hw/arm/nexus_xr.c) checks the same
INF = float("inf")
ESC_KEYS = {"kv": (0, INF, True), "poles": (2, 64, False), "tau_s": (0, INF, True),
            "max_current_a": (0, INF, True), "idle_current_a": (0, INF, False), "voltage_v": (0, INF, True)}
CENTRE_RANGE = (1000, 2000)
TRAVEL_RANGE = (1, 1000)


# aircraft.json `functions` (docs/protocol.md `function`): name -> its params (lo, hi, open_lo)
FUNCTION_PARAMS = {
    "calibrate_acc": {},
    "calibrate_gyro": {"hold_s": (0, 60, False)},
    "launch": {"speed_kmh": (0, 360, True), "duration_s": (0, 10, True), "pitch_deg": (-60, 60, False)},
    "calibrate_mag": {},
}
FUNCTION_KEYS = ("name", "label", "auto_first_start", "scenario", "params")


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and v == v and abs(v) != INF


def _range_txt(lo, hi, open_lo):
    if hi == INF:
        return f"a number {'>' if open_lo else '>='} {lo:g}" if lo != -INF else "a number"
    return f"a number {lo:g}..{hi:g}"


def load_aircraft(path):
    """models/<id>/aircraft.json, checked: WiringError naming the key on a wrong type or value."""
    where = os.path.relpath(path, ROOT) if os.path.isabs(path) and path.startswith(ROOT) else path
    try:
        with open(path, encoding="utf-8") as f:
            a = json.load(f)
    except (OSError, ValueError) as e:
        raise WiringError(f"aircraft {where}: {e}")
    if not isinstance(a, dict):
        raise WiringError(f"aircraft {where}: not a JSON object")
    a.setdefault("id", os.path.basename(os.path.dirname(path)))
    errs = []

    def err(m):
        errs.append(f"aircraft {where}: {m}")

    acts = a.get("actuators", [])
    if not isinstance(acts, list) or not all(isinstance(x, str) and ACTUATOR_RE.fullmatch(x) for x in acts) \
            or len(set(acts)) != len(acts):
        err("actuators: need a list of unique names (letters, digits, _)")
        acts = []
    ins = a.get("fdm_inputs")
    if ins is not None and (not isinstance(ins, list) or not all(isinstance(x, str) for x in ins)):
        err("fdm_inputs: need a list of actuator names")
        ins = None
    thr = a.get("throttle")
    if thr is not None and not isinstance(thr, str):
        err("throttle: need an actuator name")
        thr = None
    if a.get("fdm") == "plane":
        if ins is None or len(ins) != 4:
            err("fdm_inputs: the plane FDM needs 4 (aileron_l, aileron_r, elevator, rudder, in that order)")
        if thr is None:
            err("throttle: the plane FDM needs the ESC's actuator name")
    elif has_fdm(a):
        if ins is None or len(ins) != 4:
            err("fdm_inputs: the heli FDM needs 4 (swash servos in RotorFlight order, tail)")
        if thr is None:
            err("throttle: the heli FDM needs the ESC's actuator name")
        t = a.get("transmission")
        if not isinstance(t, dict) or "main_rotor_gear_ratio" not in t:
            err("transmission.main_rotor_gear_ratio: missing (the heli FDM needs the gear, [pinion, main])")
    for x in (ins or []) + ([thr] if thr else []):
        if acts and x not in acts:
            err(f"{x!r} (fdm_inputs/throttle) is not in actuators")
    if thr and ins and thr in ins:
        err(f"throttle {thr!r} is also an fdm_input")
    t = a.get("transmission")
    if t is not None:
        g = t.get("main_rotor_gear_ratio") if isinstance(t, dict) else None
        if not isinstance(t, dict):
            err("transmission: need an object")
        elif "main_rotor_gear_ratio" in t and not (isinstance(g, list) and len(g) == 2 and all(_num(x) and x > 0 for x in g)):
            err(f"transmission.main_rotor_gear_ratio {g!r}: need [pinion, main], 2 numbers > 0")
    # fdm_params: the model's own parameters (docs/fdm.md); the core builds the FDM from them and names a
    # wrong one (a session-check does it too), the resolver only wants an object
    if "fdm_params" in a and not isinstance(a["fdm_params"], dict):
        err("fdm_params: need an object (docs/fdm.md)")
    for sect, keys in (("esc", ESC_KEYS),):
        d = a.get(sect, {})
        if not isinstance(d, dict):
            err(f"{sect}: need an object")
            continue
        for k, v in d.items():
            if k not in keys:
                err(f"{sect}.{k}: unknown key (known: {', '.join(keys)})")
                continue
            lo, hi, open_lo = keys[k]
            if not _num(v) or v < lo or v > hi or (open_lo and v == lo):
                err(f"{sect}.{k} {v!r}: need {_range_txt(lo, hi, open_lo)}")
            elif k == "poles" and (v != int(v) or int(v) % 2):
                err(f"esc.poles {v!r}: need an even integer 2..64")
    c = a.get("centre_us", {})
    if not isinstance(c, dict):
        err("centre_us: need an object {fdm input: us}")
    else:
        for k, v in c.items():
            if ins is not None and k not in ins:
                err(f"centre_us.{k}: not one of the fdm_inputs ({', '.join(ins)})")
            elif not _num(v) or v != int(v) or not CENTRE_RANGE[0] <= v <= CENTRE_RANGE[1]:
                err(f"centre_us.{k} {v!r}: need an integer {CENTRE_RANGE[0]}..{CENTRE_RANGE[1]}")
    for kn in ("env_default", "acc_calibration"):
        ed = a.get(kn)
        if ed is None:
            continue
        if not isinstance(ed, str):
            err(f"{kn} {ed!r}: need a path (a scenario file, relative to the aircraft's directory)")
        elif not os.path.exists(os.path.join(os.path.dirname(path), ed)):
            err(f"{kn} {ed!r}: no such file next to the aircraft")
    fl = a.get("functions")
    if fl is not None:
        if not isinstance(fl, list):
            err("functions: need a list of {name, label, auto_first_start, scenario, params}")
        else:
            seen = set()
            for i, f in enumerate(fl):
                w = f"functions[{i}]"
                if not isinstance(f, dict):
                    err(f"{w}: need an object {{name, label, auto_first_start, scenario, params}}")
                    continue
                n = f.get("name")
                if not isinstance(n, str) or n not in FUNCTION_PARAMS:
                    err(f"{w}.name {n!r}: need one of {', '.join(FUNCTION_PARAMS)}")
                    continue
                if n in seen:
                    err(f"{w}.name {n!r}: listed twice")
                seen.add(n)
                for k in f:
                    if k not in FUNCTION_KEYS:
                        err(f"{w}.{k}: unknown key (known: {', '.join(FUNCTION_KEYS)})")
                if "label" in f and not isinstance(f["label"], str):
                    err(f"{w}.label {f['label']!r}: need a string")
                au = f.get("auto_first_start", False)
                if not isinstance(au, bool):
                    err(f"{w}.auto_first_start {au!r}: need true|false")
                elif au and n == "launch":
                    err(f"{w}.auto_first_start: launch is never part of the first start")
                sc = f.get("scenario")
                if sc is not None:
                    if n != "calibrate_acc":
                        err(f"{w}.scenario: only calibrate_acc takes a scenario (its poses)")
                    elif not isinstance(sc, str):
                        err(f"{w}.scenario {sc!r}: need a path (a scenario file, relative to the aircraft's directory)")
                    elif not os.path.exists(os.path.join(os.path.dirname(path), sc)):
                        err(f"{w}.scenario {sc!r}: no such file next to the aircraft")
                pr = f.get("params", {})
                if not isinstance(pr, dict):
                    err(f"{w}.params: need an object")
                    continue
                known = FUNCTION_PARAMS[n]
                for k, v in pr.items():
                    if k not in known:
                        err(f"{w}.params.{k}: unknown key (known: {', '.join(known) or 'none'})")
                        continue
                    lo, hi, open_lo = known[k]
                    if not _num(v) or v < lo or v > hi or (open_lo and v == lo):
                        err(f"{w}.params.{k} {v!r}: need {_range_txt(lo, hi, open_lo)}")
    el = a.get("env")
    if el is not None and not (isinstance(el, list) and all(isinstance(x, str) for x in el)):
        err("env: need a list of scenario paths")
    fc = a.get("firmware_cli")
    if fc is not None and not isinstance(fc, str):
        err(f"firmware_cli {fc!r}: need a path (a CLI script, relative to the data root)")
    if errs:
        raise WiringError("\n".join(errs))
    return a


def all_aircraft(warn=None):
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, "models", "*", "aircraft.json"))):
        try:
            out.append(load_aircraft(p))
        except WiringError as e:
            (warn or (lambda m: print(m, file=sys.stderr)))(f"{e} (left out of the manifest)")
    return out


def roles(aircraft):
    """(servo actuators = the FDM's inputs, the ESC's actuator = throttle). `aircraft` is the
    aircraft dict, or (old callers) its actuator list: the last one is the ESC's."""
    if isinstance(aircraft, dict):
        acts = aircraft.get("actuators") or []
        thr = aircraft.get("throttle") or ("motor" if not acts or "motor" in acts else acts[-1])
        ins = aircraft.get("fdm_inputs")
        if ins is None:
            ins = [x for x in acts if x != thr] if acts else DEFAULT_ACTUATORS[:4]
        return list(ins), thr
    if aircraft:
        return list(aircraft[:-1]), aircraft[-1]
    return DEFAULT_ACTUATORS[:4], DEFAULT_ACTUATORS[4]


def has_fdm(a):
    """The core has an FDM for this aircraft (fdm/: heli, plane)."""
    return a.get("fdm") in ("heli", "plane")


def fdm_on(a, board):
    """The aircraft's FDM runs on this board's machine."""
    return has_fdm(a) and a.get("fdm") in spec_of(board)["fdm"]


# ---- assembly -------------------------------------------------------------------------------

def wiring_form(wiring):
    """'old' (entries with slot), 'new' (pad/port/detached) or 'mixed' (both, in one entry or
    across entries). A list with neither is 'new' (strict)."""
    ents = [w for w in wiring if isinstance(w, dict)]
    old = any("slot" in w for w in ents)
    new = any(NEW_KEYS & w.keys() for w in ents)
    return "mixed" if old and new else "old" if old else "new"


def is_old_form(wiring):
    return bool(wiring) and wiring_form(wiring) == "old"


def _check_prop(dev, desc, k, v):
    """One entry prop against the device descriptor's `props`; None or the problem."""
    t = desc.get("type")
    lo, hi = desc.get("min"), desc.get("max")
    rng = f" {lo:g}..{hi:g}" if lo is not None and hi is not None else ""
    if t == "float":
        if not _num(v) or (lo is not None and v < lo) or (hi is not None and v > hi):
            return f"{dev} {k} {v!r}: need a number{rng}"
    elif t == "int":
        if not isinstance(v, int) or isinstance(v, bool) or (lo is not None and v < lo) or (hi is not None and v > hi):
            return f"{dev} {k} {v!r}: need an integer{rng}"
    elif t == "bool":
        if not isinstance(v, bool):
            return f"{dev} {k} {v!r}: need true|false"
    elif t == "enum":
        if v not in desc.get("options", []):
            return f"{dev} {k} {v!r}: need one of {', '.join(desc.get('options', []))}"
    elif t == "actuator":
        if not isinstance(v, str) or not ACTUATOR_RE.fullmatch(v):
            return f"{dev} {k} {v!r}: actuator name, letters, digits and _ only"
    return None


def resolve(wiring, board, catalog, aircraft=None, warn=None):
    """Session wiring -> the assembly: a list of entries
       {device, port, pad (pad id), props, onboard, modelled}
    with the onboard devices that stay on the board (and, old form, the assumed receiver, ESC and
    servos) included. `aircraft` = the aircraft dict (load_aircraft; an actuator list is accepted
    too). Raises WiringError with every problem; notes (left-out entries, old-form defaults) go to
    warn()."""
    note = warn or (lambda m: None)
    errs = []
    devs = {d["id"]: d for d in catalog}
    pads = {p["id"]: p for p in board["pads"]}
    by_label = {p["label"].casefold(): p["id"] for p in board["pads"] if p.get("label")}
    if not isinstance(wiring, list):
        raise WiringError("wiring: need a list")
    form = wiring_form(wiring)
    if form == "mixed":
        bad = [f"wiring[{i}]" for i, w in enumerate(wiring) if isinstance(w, dict) and "slot" in w]
        raise WiringError("wiring: mixes the old form ({device, slot}) and the new one ({device, port, pad} / "
                          f"detached); entries with slot: {', '.join(bad)}. Use one form for the whole list")
    old = form == "old" and bool(wiring)
    spec = spec_of(board)
    modelled = spec["modelled"]
    if old and not spec["legacy"]:
        raise WiringError(f"wiring: the old form ({{device, slot}}) is only for nexus-xr benches; on {board['id']} "
                          "use {device, port, pad}")
    fdm_ins, throttle = roles(aircraft)

    def name(pid):
        p = pads[pid]
        return f"{p['label']} ({pid})" if p.get("label") else pid

    def find_pad(ref):
        if not isinstance(ref, str):
            return None
        if ref in pads:
            return ref
        return by_label.get(ref.casefold())

    def place_err(where, msg):
        """A problem with where an entry goes: an error (new form), a warning (old form)."""
        if old:
            note(f"{where}: {msg}; left out (old-form session)")
        else:
            errs.append(f"{where}: {msg}")

    entries, detached = [], set()
    for i, w in enumerate(wiring):
        where = f"wiring[{i}]"
        if not isinstance(w, dict):
            errs.append(f"{where}: need an object")
            continue
        dev = w.get("device")
        d = devs.get(dev)
        if "detached" in w and not isinstance(w["detached"], bool):
            errs.append(f"{where}: detached must be true|false")
            continue
        if w.get("detached") is True:
            pid = find_pad(w["pad"]) if "pad" in w else next(
                (p["id"] for p in board["pads"] if p["onboard"] and p["device"] == dev), None)
            p = pads.get(pid) if pid else None
            if not p or not p["onboard"] or (dev and p["device"] != dev):
                errs.append(f"{where}: {dev or w.get('pad')} is not an onboard device of {board['id']}, nothing to detach")
            elif not p["detachable"]:
                errs.append(f"{where}: onboard {p['device']} on {name(pid)} is soldered (detachable: false), it cannot be removed")
            else:
                detached.add(pid)
            continue
        if d is None:
            place_err(where, f"unknown device {dev!r} (known: {', '.join(devs)})")
            continue
        ports = d.get("ports") or [{"id": d["id"], "kind": d["kind"], "dir": "both"}]
        port_id = w.get("port", ports[0]["id"])
        port = next((p for p in ports if p["id"] == port_id), None)
        if port is None:
            errs.append(f"{where}: device {dev} has no port {port_id!r} (ports: {', '.join(p['id'] for p in ports)})")
            continue
        if port.get("bundled"):
            errs.append(f"{where}: {dev}.{port_id} comes with the {ports[0]['id']} pad, wire {dev}.{ports[0]['id']}")
            continue
        ref = w.get("pad", w.get("slot"))
        pid = find_pad(ref)
        if pid is None:
            known = ", ".join(f"{p['label']}={p['id']}" if p.get("label") else p["id"] for p in board["pads"])
            place_err(where, f"unknown pad {ref!r} on {board['id']} (pads: {known})")
            continue
        p = pads[pid]
        if not kind_fits(p["kind"], port["kind"]):
            place_err(where, f"{dev}.{port_id} is a {port['kind']} port, pad {name(pid)} is {p['kind']}")
            continue
        if p["kind"] == "uart":
            need = {"in": ("tx",), "out": ("rx",), "both": ("tx", "rx")}[port.get("dir", "both")]
            miss = [x for x in need if x not in p.get("pins", {}) and "usb" not in p["flags"]]
            if miss:
                place_err(where, f"{dev}.{port_id} ({port.get('dir')}) needs {'+'.join(need).upper()} on the board side, "
                          f"pad {name(pid)} has no {'/'.join(miss).upper()}")
                continue
        # props: the descriptor's; the app writes actuator "", reversed false and option "" on
        # every entry (option = the ESC's telemetry)
        props, bad = {}, False
        pdesc = {x["id"]: x for x in d.get("props", [])}
        for k, v in w.items():
            if k in ENTRY_KEYS:
                continue
            if (k == "actuator" and v == "") or (k == "reversed" and v is False) or (k == "option" and v == ""):
                continue
            if k == "option" and "telemetry" in pdesc:
                if "telemetry" in w and w["telemetry"] != v:
                    errs.append(f"{where}: option {v!r} and telemetry {w['telemetry']!r} disagree")
                    bad = True
                    continue
                k = "telemetry"
            if k not in pdesc:
                errs.append(f"{where}: {dev} has no prop {k!r} "
                            f"({'props: ' + ', '.join(pdesc) if pdesc else 'it takes none'})")
                bad = True
                continue
            m = _check_prop(dev, pdesc[k], k, v)
            if m:
                errs.append(f"{where}: {m}")
                bad = True
                continue
            props[k] = v
        if "actuator" in props:
            a = props["actuator"]
            if dev == "servo" and a not in fdm_ins:
                errs.append(f"{where}: servo actuator {a!r} is not one of the aircraft's FDM inputs ({', '.join(fdm_ins)})")
                bad = True
            elif dev == "esc" and (port_id != ports[0]["id"] or a != throttle):
                errs.append(f"{where}: esc actuator {a!r}: the ESC's signal drives the aircraft's throttle {throttle!r}"
                            + ("" if port_id == ports[0]["id"] else f", not on its {port_id} port"))
                bad = True
        if bad:
            continue
        entries.append({"device": dev, "port": port_id, "pad": pid, "props": props,
                        "onboard": bool(p["onboard"] and p["device"] == dev), "src": where})

    # old form: a pad named twice keeps the first entry (before: the machine had one device there)
    if old:
        seen, keep = {}, []
        for e in entries:
            if e["pad"] in seen and pads[e["pad"]]["kind"] != "i2c":
                note(f"{e['src']}: pad {name(e['pad'])} is already used by {seen[e['pad']]}; left out (old-form session)")
                continue
            seen.setdefault(e["pad"], f"{e['device']}.{e['port']} ({e['src']})")
            keep.append(e)
        entries = keep

    # onboard devices: present unless detached (new form) / unless the old-form list omits them
    old_rx = [e for e in entries if e["device"] == "crsf" and e["pad"] in modelled.get(("crsf", "uart"), ())]
    rx_assumed = False
    for p in board["pads"]:
        if not p["onboard"]:
            continue
        pid = p["id"]
        named = [e for e in entries if e["pad"] == pid and e["device"] == p["device"]]
        if pid in detached:
            if named:
                errs.append(f"{named[0]['src']}: onboard {p['device']} on {name(pid)} is both wired and detached")
            continue
        if named:
            continue
        if old:
            if p["device"] == "crsf" and not old_rx and not any(e["pad"] == pid for e in entries):
                rx_assumed = True       # the old form's receiver: assumed present
            elif p["detachable"]:
                detached.add(pid)
                note(f"onboard {p['device']} on {name(pid)} not in the (old-form) wiring: detached")
                continue
            elif p.get("group"):      # a pad the old form can name at all
                note(f"onboard {p['device']} on {name(pid)} not in the (old-form) wiring: it is soldered, kept")
        d = devs.get(p["device"])
        port0 = (d.get("ports") or [{"id": p["device"]}])[0]["id"] if d else p["device"]
        entries.append({"device": p["device"], "port": port0, "pad": pid, "props": {}, "onboard": True,
                        "src": f"onboard {pid}"})

    # old form: the machine before the wiring model always had the receiver, the ESC and servo1..4
    if old:
        assumed = []
        if not old_rx and not rx_assumed and OLD_RX_PAD in pads and                 not any(e["pad"] == OLD_RX_PAD for e in entries):
            entries.append({"device": "crsf", "port": "uart", "pad": OLD_RX_PAD, "props": {},
                            "onboard": False, "src": "old-form default"})
            rx_assumed = True
        if rx_assumed:
            assumed.append(f"receiver on {name(OLD_RX_PAD)}")
        used = {e["pad"] for e in entries}
        if not any(e["device"] == "esc" and e["port"] == "signal" for e in entries) and OLD_ESC_PAD in pads \
                and OLD_ESC_PAD not in used:
            entries.append({"device": "esc", "port": "signal", "pad": OLD_ESC_PAD, "props": {}, "onboard": False,
                            "src": "old-form default"})
            assumed.append(f"ESC on {OLD_ESC_PAD}")
        miss = [sp for sp in spec["servo_pads"] if sp in pads and sp not in used]
        for sp in miss:
            entries.append({"device": "servo", "port": "pwm", "pad": sp, "props": {}, "onboard": False,
                            "src": "old-form default"})
        if miss:
            assumed.append("servos on " + ", ".join(miss))
        if assumed:
            note(f"old-form session: receiver/ESC/servos assumed present (not in the list: {'; '.join(assumed)})")

    # one device per pad, except on a bus; on a bus one device of a kind (same address)
    use = {}
    for e in entries:
        use.setdefault(e["pad"], []).append(e)
    for pid, es in use.items():
        if len(es) > 1 and pads[pid]["kind"] != "i2c":
            errs.append(f"pad {name(pid)} used by " + ", ".join(
                f"{e['device']}.{e['port']} ({e['src']})" for e in es))
        elif len(es) > 1:
            ids = [e["device"] for e in es]
            for dev in sorted({x for x in ids if ids.count(x) > 1}):
                errs.append(f"bus {name(pid)}: two {dev} (same I2C address): " + ", ".join(
                    e["src"] for e in es if e["device"] == dev))

    # one OSD per session: the machine models one MSP DisplayPort device and the app one canvas
    osds = [e for e in entries if e["device"] == "msp_osd"]
    if len(osds) > 1:
        errs.append("one OSD per session: msp_osd on " + ", ".join(f"{name(e['pad'])} ({e['src']})" for e in osds))
    # one GPS, one battery: the machine models one of each
    for dev in ("ublox", "battery"):
        es = [e for e in entries if e["device"] == dev]
        if len(es) > 1:
            errs.append(f"one {dev} per session: " + ", ".join(f"{name(e['pad'])} ({e['src']})" for e in es))

    # actuators: each on one output
    acts = {}
    for e in entries:
        a = e["props"].get("actuator")
        if not a:
            continue
        if a in acts:
            errs.append(f"{e['src']}: actuator {a} is already driven by {acts[a]}")
        else:
            acts[a] = f"{e['device']} on {name(e['pad'])} ({e['src']})"
    if errs:
        raise WiringError("\n".join(errs))

    # what the machine builds; one receiver (the explicitly wired one first)
    have_rx = False
    for e in sorted(entries, key=lambda e: e["onboard"]):
        cap = e["pad"] in modelled.get((e["device"], e["port"]), ())
        e["modelled"] = cap
        if cap and e["device"] == "crsf":
            if have_rx:
                e["modelled"] = False
                note(f"crsf on {name(e['pad'])}: the machine models one receiver (the wired one first), left out")
            have_rx = True
        elif not cap:
            what = "onboard, listed" if e["onboard"] else "wired"
            note(f"{e['device']}.{e['port']} on {name(e['pad'])} ({what}): not modelled on this machine, left out")
        if cap and e["device"] == "esc":
            opt = e["props"].get("telemetry", "bidir")
            if opt != "bidir":
                note(f"esc telemetry {opt!r}: only bidirectional DShot is modelled")
    return entries


def props_value(entry, dev, catalog):
    """A wiring entry's props for -M <dev>-props=: "<id>=<value>:..." in the descriptor's order,
    only the ones the entry gives (the machine has the defaults); '' = none"""
    desc = next((d for d in catalog or [] if d["id"] == dev), {})
    parts = []
    for p in desc.get("props", []):
        if p["id"] not in entry["props"]:
            continue
        v, t = entry["props"][p["id"]], p.get("type")
        txt = ("true" if v else "false") if t == "bool" else v if t == "enum" else \
            str(int(v)) if t == "int" else str(float(v))
        parts.append(f"{p['id']}={txt}")
    return ":".join(parts)


def machine_options(entries, aircraft=None, catalog=None, board=None):
    """The assembly -> -M device options of the board's machine (values unquoted; `baro`, `mag`,
    `gps`, `battery` only on a machine that models one). Props were checked by resolve()."""
    spec = spec_of(board)
    servo_pads = spec["servo_pads"]
    fdm_ins, throttle = roles(aircraft)
    on = [e for e in entries if e["modelled"]]
    opts = {}
    imu = [e for e in on if e["device"] == "icm42688p"]
    opts["imu"] = "on" if imu else "off"
    defaults = {"gyro_noise_dps": 0.05, "accel_noise_g": 0.002}
    for d in catalog or []:
        if d["id"] == "icm42688p":
            defaults.update({p["id"]: p["default"] for p in d.get("props", []) if "default" in p})
    for e in imu:
        if "gyro_noise_dps" in e["props"] or "accel_noise_g" in e["props"]:
            g = e["props"].get("gyro_noise_dps", defaults["gyro_noise_dps"])
            a = e["props"].get("accel_noise_g", defaults["accel_noise_g"])
            opts["imu-noise"] = f"{float(g)}:{float(a)}"
        if "accel_offset_mg" in e["props"] or "accel_scale_pct" in e["props"]:
            opts["imu-accel-error"] = (f"{float(e['props'].get('accel_offset_mg', 0))}:"
                                       f"{float(e['props'].get('accel_scale_pct', 0))}")
    rx = [e for e in on if e["device"] == "crsf"]
    opts["crsf"] = rx[0]["pad"] if rx else "off"
    esc = [e for e in on if e["device"] == "esc"]
    opts["esc"] = "on" if esc else "off"
    modelled_devs = {dev for dev, _ in spec["modelled"]}
    if modelled_devs & {"dps310", "spl06"}:
        baro = [e for e in on if e["device"] in ("dps310", "spl06")]
        opts["baro"] = "on" if baro else "off"
        if baro and props_value(baro[0], baro[0]["device"], catalog):
            opts["baro-props"] = props_value(baro[0], baro[0]["device"], catalog)
    osd = [e for e in on if e["device"] == "msp_osd"]
    if osd:                     # absent = off: the -M string of a session without it is unchanged
        opts["osd"] = osd[0]["pad"]
    for dev, key, val in (("qmc5883l", "mag", lambda e: "on"), ("ublox", "gps", lambda e: e["pad"]),
                          ("battery", "battery", lambda e: "on")):
        if dev not in modelled_devs:
            continue
        es = [e for e in on if e["device"] == dev]
        opts[key] = val(es[0]) if es else "off"
        if es and props_value(es[0], dev, catalog):
            opts[key + "-props"] = props_value(es[0], dev, catalog)
    names = [None] * len(servo_pads)
    rev = mask = 0
    for e in on:
        if e["device"] == "servo":
            i = servo_pads.index(e["pad"])
            mask |= 1 << i
            if e["props"].get("actuator"):
                names[i] = e["props"]["actuator"]
            if e["props"].get("reversed"):
                rev |= 1 << i
    for e in esc:
        ms = e["props"].get("signal_timeout_ms")
        if ms is not None:
            opts["esc-timeout-ms"] = str(ms)
    # a servo output the wiring does not name keeps the FDM input of its position unless another
    # output has it; the ESC's output is always the aircraft's throttle
    for i, n in enumerate(names):
        if n is None:
            dflt = fdm_ins[i] if i < len(fdm_ins) else None
            names[i] = dflt if dflt and dflt not in names else f"servo{i + 1}"
    opts["servos"] = str(mask)
    opts["actuators"] = ":".join(names + [throttle])
    if rev:
        opts["servo-rev"] = str(rev)
    return opts


def device_config(entries, board, catalog):
    """CLI lines the wired (modelled) devices need in the firmware (descriptor `config`, then its
    `config_firmware` lines for the board's `firmware_family`; "{serial}" = the serial index of the
    UART pad the device is on), in wiring order; appended to the custom-defaults blob after the
    board config and the session profiles."""
    devs = {d["id"]: d for d in catalog}
    pads = {p["id"]: p for p in board["pads"]}
    lines = []
    for e in entries:
        d = devs.get(e["device"], {}) if e.get("modelled") else {}
        fw = d.get("config_firmware")
        cfg = (d.get("config") or []) + ((fw.get(board.get("firmware_family", "rotorflight")) or [])
                                          if isinstance(fw, dict) else [])
        if not cfg:
            continue
        serial = pads[e["pad"]].get("serial")
        lines.append(f"# {e['device']} on {e['pad']} (device config)")
        for line in cfg:
            if "{serial}" in line and serial is None:
                raise WiringError(f"{e['device']} on {e['pad']}: its config needs a serial index, the pad has none")
            lines.append(line.replace("{serial}", str(serial)))
    return lines


def slots_compat():
    """session.SLOTS of old: {(device, slot): what the machine has there}."""
    return {(dev, pad): f"{dev}.{port}" for (dev, port), pads in MODELLED.items() for pad in pads}
