#!/usr/bin/env python3
"""Generated benches for the session equivalence test (test_session_equiv.sh): the wiring cases of
scripts/test_wiring.py (every WiringError and note path both forms have), plus the bench's other
blocks (aircraft files with each checked mistake, environment, time, ports, profiles, profile_text,
no wiring, boot bare), and the speedybee-f405-wing's (output pads, what its machine models, the old
form rejected, no wiring, the CLI script: base script, aircraft firmware_cli, profiles, save/exit
dropped, reset/wipe commands rejected). Both resolvers must give the same report, warnings and
errors on each.

  gen_benches.py <outdir>      writes <outdir>/NNN-<name>.json (and the aircraft and CLI files they
                               use); new cases go at the end (the numbers of the others stay)
"""
import copy, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SESS = os.path.join(HERE, "..", "sessions")
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def load(name):
    with open(os.path.join(SESS, name), encoding="utf-8") as f:
        return json.load(f)


NEW, OLD = load("rs7-pads.json"), load("rs7.json")

# (name, form base, wiring) - the lists of test_wiring.py Sessions/Osd
WIRING = [
    ("unknown-pad", NEW, [{"device": "servo", "port": "pwm", "pad": "S9", "actuator": "tail"}]),
    ("servo-on-uart", NEW, [{"device": "servo", "port": "pwm", "pad": "PORT-A"}]),
    ("detach-gyro", NEW, NEW["wiring"] + [{"device": "icm42688p", "detached": True}]),
    ("detach-baro", NEW, NEW["wiring"] + [{"device": "dps310", "detached": True}]),
    ("compass-next-to-baro", NEW, NEW["wiring"] + [{"device": "compass", "pad": "i2c3"}]),
    ("duplicate-pad", NEW, [{"device": "servo", "pad": "servo1", "actuator": "swash_a"},
                            {"device": "servo", "pad": "servo1", "actuator": "swash_b"}]),
    ("gps-on-onboard-rx", NEW, [{"device": "gps", "pad": "uart5"}]),
    ("unknown-device", NEW, [{"device": "nope", "pad": "uart3"}]),
    ("crsf-on-rx-only", NEW, [{"device": "crsf", "pad": "TELEM"}]),
    ("bundled-port", NEW, [{"device": "icm42688p", "port": "int", "pad": "gyro1"}]),
    ("no-such-port", NEW, [{"device": "esc", "port": "nope", "pad": "motor1"}]),
    ("servo-not-fdm-input", NEW, [{"device": "servo", "pad": "servo1", "actuator": "flap"}]),
    ("old-without-crsf-esc", OLD, [w for w in OLD["wiring"] if w["device"] not in ("crsf", "esc")]),
    ("new-without-esc-rx-detached", NEW, [w for w in NEW["wiring"] if w["device"] != "esc"] +
     [{"device": "crsf", "detached": True}]),
    ("wired-and-detached", NEW, [{"device": "icm42688p", "detached": True}, {"device": "icm42688p", "pad": "gyro1"}]),
    ("detached-not-bool", NEW, [{"device": "icm42688p", "detached": "yes"}]),
    ("detached-int", NEW, [{"device": "icm42688p", "detached": 1}]),
    ("detach-not-onboard", NEW, [{"device": "gps", "detached": True}]),
    ("detach-flash-soldered", NEW, [{"device": "spiflash", "detached": True}]),
    ("detach-by-pad", NEW, NEW["wiring"] + [{"pad": "Int.Rx", "detached": True}]),
    ("reversed-string", NEW, [{"device": "servo", "pad": "servo1", "actuator": "swash_a", "reversed": "true"}]),
    ("reversed-int", NEW, [{"device": "servo", "pad": "servo1", "actuator": "swash_a", "reversed": 1}]),
    ("actuator-twice", NEW, [{"device": "servo", "pad": "servo1", "actuator": "tail"},
                             {"device": "servo", "pad": "servo4", "actuator": "tail"}]),
    ("actuator-bad-name", NEW, [{"device": "servo", "pad": "servo1", "actuator": "swash a"}]),
    ("actuator-number", NEW, [{"device": "servo", "pad": "servo1", "actuator": 3}]),
    ("servo-motor", NEW, [{"device": "servo", "pad": "servo1", "actuator": "motor"}]),
    ("esc-tail", NEW, [{"device": "esc", "pad": "motor1", "actuator": "tail"}]),
    ("esc-tlm-actuator", NEW, [{"device": "esc", "port": "tlm", "pad": "TELEM", "actuator": "motor"}]),
    ("gps-actuator", NEW, [{"device": "gps", "pad": "PORT-A", "actuator": "tail"}]),
    ("old-servo-motor", OLD, [{"device": "servo", "slot": "servo2", "actuator": "motor"}]),
    ("esc-throttle-name", NEW, [dict(NEW["wiring"][0], actuator="motor")] + NEW["wiring"][1:]),
    ("unknown-prop", NEW, [{"device": "servo", "pad": "servo1", "actuator": "swash_a", "trim": 5}]),
    ("icm-unknown-prop", NEW, [{"device": "icm42688p", "pad": "gyro1", "gyro_noise": 0.1}]),
    ("old-esc-unknown-prop", OLD, [{"device": "esc", "slot": "motor1", "timeout": 5}]),
    ("esc-telemetry-enum", NEW, [{"device": "esc", "pad": "motor1", "telemetry": "pwm"}]),
    ("esc-telemetry-serial", NEW, [dict(NEW["wiring"][0], telemetry="serial")] + NEW["wiring"][1:]),
    ("option-telemetry-disagree", NEW, [{"device": "esc", "pad": "motor1", "telemetry": "bidir", "option": "serial"}]),
    ("app-empty-props", NEW, [dict(w, actuator=w.get("actuator", ""), reversed=w.get("reversed", False),
                                   option=w.get("option", "")) for w in NEW["wiring"]]),
    ("timeout-range", NEW, [{"device": "esc", "pad": "motor1", "signal_timeout_ms": 2001}]),
    ("timeout-float", NEW, [{"device": "esc", "pad": "motor1", "signal_timeout_ms": 50.5}]),
    ("old-timeout-negative", OLD, [{"device": "esc", "slot": "motor1", "signal_timeout_ms": -1}]),
    ("gyro-noise-range", NEW, [{"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 51}]),
    ("accel-noise-string", NEW, [{"device": "icm42688p", "pad": "gyro1", "accel_noise_g": "0.1"}]),
    ("noise-and-timeout", NEW, [dict(NEW["wiring"][0], signal_timeout_ms=0)] + NEW["wiring"][1:] +
     [{"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 0.5}]),
    ("noise-both", NEW, NEW["wiring"] + [{"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 1,
                                          "accel_noise_g": 0.0125}]),
    ("mixed-across", NEW, [{"device": "servo", "slot": "servo1"}, {"device": "servo", "pad": "servo2"}]),
    ("mixed-in-one", NEW, [{"device": "servo", "slot": "servo1", "pad": "servo1"}]),
    ("mixed-detached", NEW, [{"device": "icm42688p", "slot": "gyro1"}, {"device": "crsf", "detached": True}]),
    ("i2c-compass-twice", NEW, [{"device": "compass", "pad": "i2c3"}, {"device": "compass", "pad": "i2c3"}]),
    ("i2c-baro-twice", NEW, [{"device": "dps310", "pad": "i2c3"}, {"device": "dps310", "pad": "i2c3"}]),
    ("old-crsf-uart3", OLD, [{"device": "icm42688p", "slot": "gyro1"}, {"device": "crsf", "slot": "uart3"}]),
    ("old-omits-baro", OLD, [w for w in OLD["wiring"] if w["device"] != "dps310"]),
    ("old-unplaceable", OLD, OLD["wiring"] + [{"device": "nope", "slot": "uart3"}, {"device": "servo", "slot": "servo9"},
                                              {"device": "servo", "slot": "PORT-A"},
                                              {"device": "servo", "slot": "servo1", "actuator": "swash_b"}]),
    ("app-smoke-gyro-only", OLD, [{"device": "icm42688p", "slot": "gyro1", "actuator": "", "reversed": False,
                                   "option": ""}]),
    ("external-rx-reversed", NEW, NEW["wiring"][:4] + [dict(NEW["wiring"][4], reversed=True)] + NEW["wiring"][5:] +
     [{"device": "crsf", "detached": True}, {"device": "crsf", "pad": "PORT-C"}]),
    ("two-receivers", NEW, NEW["wiring"] + [{"device": "crsf", "pad": "PORT-C"}]),
    ("osd-port-a", NEW, NEW["wiring"] + [{"device": "msp_osd", "port": "uart", "pad": "PORT-A"}]),
    ("osd-old-uart6", OLD, OLD["wiring"] + [{"device": "msp_osd", "slot": "uart6"}]),
    ("osd-telem", NEW, NEW["wiring"] + [{"device": "msp_osd", "port": "uart", "pad": "TELEM"}]),
    ("osd-int-rx", NEW, NEW["wiring"] + [{"device": "msp_osd", "port": "uart", "pad": "Int.Rx"}]),
    ("osd-servo-pad", NEW, NEW["wiring"] + [{"device": "msp_osd", "port": "uart", "pad": "S1"}]),
    ("osd-twice", NEW, NEW["wiring"] + [{"device": "msp_osd", "port": "uart", "pad": "PORT-A"},
                                        {"device": "msp_osd", "port": "uart", "pad": "PORT-B"}]),
    ("osd-twice-old", OLD, OLD["wiring"] + [{"device": "msp_osd", "slot": "uart4"}, {"device": "msp_osd", "slot": "uart6"}]),
    ("osd-uart5-rx-elsewhere", NEW, NEW["wiring"] + [{"device": "crsf", "detached": True},
                                                     {"device": "crsf", "port": "uart", "pad": "PORT-C"},
                                                     {"device": "msp_osd", "port": "uart", "pad": "uart5"}]),
    ("usb-pad", NEW, NEW["wiring"] + [{"device": "gps", "pad": "USB VCP"}]),
    ("entry-not-object", NEW, NEW["wiring"] + ["servo"]),
    ("wiring-not-list", NEW, {"device": "servo"}),
    ("pad-null", NEW, [{"device": "servo", "pad": None}]),
    ("port-null", NEW, [{"device": "servo", "port": None, "pad": "servo1"}]),
    ("unicode-props", NEW, [{"device": "servo", "pad": "servo1", "actuator": "swash_a", "trim": "é\u00e9\U0001F600\n'"}]),
]


def aircraft_variants(d):
    """(name, aircraft path) with one checked mistake each, written under d"""
    with open(os.path.join(ROOT, "models", "rs7", "aircraft.json"), encoding="utf-8") as f:
        base = json.load(f)
    out = []
    cases = [
        ("no-transmission", lambda a: a.pop("transmission")),
        ("gear-zero", lambda a: a["transmission"].update(main_rotor_gear_ratio=[0, 116])),
        ("gear-one", lambda a: a["transmission"].update(main_rotor_gear_ratio=[13])),
        ("kv-zero", lambda a: a["esc"].update(kv=0)),
        ("kv-string", lambda a: a["esc"].update(kv="520")),
        ("poles-odd", lambda a: a["esc"].update(poles=7)),
        ("poles-big", lambda a: a["esc"].update(poles=66)),
        ("esc-unknown", lambda a: a["esc"].update(kvv=1)),
        ("params-not-object", lambda a: a.update(fdm_params=5)),
        ("centre-range", lambda a: a.update(centre_us={"tail": 2500})),
        ("centre-not-input", lambda a: a.update(centre_us={"motor": 1500})),
        ("throttle-is-input", lambda a: a.update(throttle="tail")),
        ("actuators-dup", lambda a: a.update(actuators=["swash_a", "swash_a", "tail", "motor"])),
        ("fdm-inputs-3", lambda a: a.update(fdm_inputs=["swash_a", "swash_b", "swash_c"])),
        ("input-not-actuator", lambda a: a.update(fdm_inputs=["swash_a", "swash_b", "swash_c", "rudder"])),
        ("no-fdm", lambda a: (a.pop("fdm"), a.pop("id"))),
        ("many", lambda a: (a["esc"].update(kv=-1, tau_s=0), a.update(centre_us=[]), a.update(throttle=5))),
    ]
    for name, change in cases:
        a = copy.deepcopy(base)
        change(a)
        sub = os.path.join(d, "aircraft", name)
        os.makedirs(sub, exist_ok=True)
        p = os.path.join(sub, "aircraft.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(a, f)
        out.append((f"aircraft-{name}", p))
    return out


def benches(d):
    out = []
    for name, base, w in WIRING:
        s = copy.deepcopy(base)
        s["wiring"] = w
        out.append((name, s))
    for name, p in aircraft_variants(d):
        out.append((name, dict(copy.deepcopy(NEW), aircraft=p)))
    other = [
        ("aircraft-by-id", {"aircraft": "rs7"}),
        ("aircraft-app-path", {"aircraft": "C:/Users/you/flightbench/models/rs7/aircraft.json"}),
        ("aircraft-missing", {"aircraft": "models/nope/aircraft.json"}),
        ("aircraft-null", {"aircraft": None}),
        ("board-other", {"board": "matek-f405"}),
        ("no-firmware", {"firmware": ""}),
        ("firmware-relative", {"firmware": "fw/rotorflight.elf"}),
        ("firmware-backslashes", {"firmware": "C:\\fw\\a,b.elf"}),
        ("no-wiring", {"wiring": []}),
        ("boot-bare", {"board_config": ""}),
        ("profiles-list", {"profile": ["models/rs7/qemu-profile.txt", "models/rs7/qemu-profile.txt"]}),
        ("no-profile", {"profile": None}),
        ("profile-missing", {"profile": "models/rs7/nope.txt"}),
        ("profile-text", {"profile_text": "# comment\r\nset beeper_inversion = ON\r\n\r\n  resource BEEPER 1 C13\n\tset x = 1"}),
        ("profile-text-blank", {"profile_text": "  \n\t"}),
        ("beeper-none", {"profile_text": "resource BEEPER 1 B02\nresource beeper 1 none\n"}),
        ("env", {"environment": {"sea_level_pa": 100800, "field_elevation_m": 990}}),
        ("env-full", {"environment": {"sea_level_pa": 101325.123456789, "field_elevation_m": 989.987654321,
                                      "temp_c": -12.3456789}}),
        ("env-bad-value", {"environment": {"sea_level_pa": 5}}),
        ("env-bad-key", {"environment": {"qnh": 1}}),
        ("env-string", {"environment": {"temp_c": "20"}}),
        ("env-list", {"environment": []}),
        ("time-realtime", {"time": {"icount_shift": 3, "realtime": True, "idle_skip": False}}),
        ("time-no-icount", {"time": {"icount_shift": None}}),
        ("time-shift-bad", {"time": {"icount_shift": 11}}),
        ("time-shift-bool", {"time": {"icount_shift": True}}),
        ("time-skip-no-icount", {"time": {"icount_shift": None, "idle_skip": True}}),
        ("time-skip-string", {"time": {"idle_skip": "yes"}}),
        ("time-skip-params", {"time": {"idle_skip": True, "idle_skip_n": 32, "idle_skip_gran_us": 200}}),
        ("time-skip-n-bad", {"time": {"idle_skip": True, "idle_skip_n": 1}}),
        ("time-empty", {"time": {}}),
        ("ports", {"ports": {"control": 31280, "msp": 31281}}),
        ("ports-bad", {"ports": {"control": 70000}}),
        ("ports-string", {"ports": {"msp": "31235"}}),
    ]
    for name, change in other:
        s = copy.deepcopy(NEW)
        s.update(change)
        if change.get("firmware") == "":
            s.pop("firmware")
            s["firmware"] = ""
        out.append((name, s))
    return out


F405 = load("f405-crsf.json")          # speedybee-f405-wing, crsf on uart1 (new form)
RX = F405["wiring"]
SERVOS = [{"device": "esc", "port": "signal", "pad": "S1", "actuator": "motor", "signal_timeout_ms": 50},
          {"device": "servo", "port": "pwm", "pad": "output3", "actuator": "swash_a"},
          {"device": "servo", "port": "pwm", "pad": "S4", "actuator": "swash_b", "reversed": True},
          {"device": "servo", "port": "pwm", "pad": "output5", "actuator": "swash_c"},
          {"device": "servo", "port": "pwm", "pad": "S11", "actuator": "tail"}]

# (name, wiring) on the speedybee-f405-wing
F405_WIRING = [
    ("f405-servos", RX + SERVOS),
    ("f405-servo-on-s1", RX + [{"device": "servo", "port": "pwm", "pad": "S1"}]),
    ("f405-esc-on-s2", RX + [{"device": "esc", "port": "signal", "pad": "output2"}]),
    ("f405-esc-and-servo-s1", [{"device": "esc", "pad": "S1"}, {"device": "servo", "pad": "output1"}]),
    ("f405-crsf-on-output", [{"device": "crsf", "port": "uart", "pad": "S3"}]),
    ("f405-servo-on-uart", [{"device": "servo", "port": "pwm", "pad": "uart2"}]),
    ("f405-esc-tlm-on-output", [{"device": "esc", "port": "tlm", "pad": "output4"}]),
    ("f405-gps-uart3", RX + [{"device": "gps", "port": "uart", "pad": "uart3"}]),
    ("f405-crsf-uart6", [{"device": "crsf", "port": "uart", "pad": "uart6"}]),
    ("f405-two-receivers", RX + [{"device": "crsf", "pad": "uart2"}]),
    ("f405-old-form", [{"device": "crsf", "slot": "uart1"}, {"device": "servo", "slot": "output3"}]),
    ("f405-mixed", [{"device": "crsf", "slot": "uart1"}, {"device": "servo", "pad": "output3"}]),
    ("f405-detach-gyro", RX + [{"device": "icm42688p", "detached": True}]),
    ("f405-detach-baro", RX + [{"device": "spl06", "detached": True}]),
    ("f405-detach-osd", RX + [{"device": "max7456", "detached": True}]),
    ("f405-detach-sdcard-by-pad", RX + [{"pad": "sdcard1", "detached": True}]),
    ("f405-dps310-next-to-spl06", RX + [{"device": "dps310", "pad": "i2c1"}]),
    ("f405-msp-osd", RX + [{"device": "msp_osd", "port": "uart", "pad": "uart4"}]),
    ("f405-usb-gps", RX + [{"device": "gps", "pad": "USB VCP"}]),
    ("f405-imu-noise", RX + [{"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 0.25}]),
    ("f405-servo-not-fdm-input", [{"device": "servo", "pad": "S3", "actuator": "aileron"}]),
    ("f405-unknown-pad", [{"device": "servo", "pad": "S12"}]),
    ("f405-slot-less-old-app", [{"device": "icm42688p", "slot": "gyro1", "actuator": "", "reversed": False}]),
]


GPS3 = [{"device": "ublox", "port": "uart", "pad": "uart3"}]


def cli_files(d):
    """CLI scripts the speedybee benches use, written under d"""
    files = {
        "base.cli": b"# a base script\r\nfeature -GPS\r\n\r\n   set gyro_main_lpf_hz = 90   \r\nSAVE\n",
        "plane.cli": b"set platform_type = AIRPLANE\nmmix 0 1.0 0 0 0\n\tsmix 0 3 0 100 0 -1\nexit\n# end\nsave",
        "bad.cli": b"set a = 1\ndefaults nosave\n  Reboot\nsave\ndfu\nbl\nbootloader\nmsc\nbl_x = 1\nsavex\n",
        "nul.cli": b"save\x00x\nset b = 2\n",
        # the bench's own lines (layer 4): a conflict with the aircraft (nav_fw_bank_angle 35), the same
        # value again (no conflict), a numeric form (1.0 = 1.000), the GPS port absolute (+1 on the
        # device line, compatible: bit 1 set in 2), a changed smix after the aircraft's reset, a
        # feature turned off, profile contexts
        "bench.cli": b"# the bench's lines\nset nav_fw_bank_angle = 40\nset looptime = 1000\n"
                     b"set d_boost_min =  1.0\nserial 2 2 115200 38400 0 115200\nsmix 1 2 0 -100 0 -1\n"
                     b"feature -PWM_OUTPUT_ENABLE\ncontrol_profile 2\nset roll_rate = 20\ncontrol_profile 1\n"
                     b"set Platform_Type=airplane\nsave\n",
        # a snapshot-shaped file: the relative serial the other way (bit 1 clear), a resource, a map
        "bench2.cli": b"serial 2 0 115200 38400 0 115200\nresource MOTOR 1 B06\nmap TAER\nbeeper -ON_USB\n"
                      b"mmix reset\nmmix 0 1.000 0.000 0.000 0.000\n",
        "bench-bad.cli": b"set a = 1\nreboot\n",
    }
    out = {}
    for name, data in files.items():
        p = os.path.join(d, "cli", name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        out[name] = p
    return out


def f405_aircraft(d, cli):
    """(name, aircraft path): rs7 with a firmware_cli, a plane without an FDM, bad firmware_cli values"""
    with open(os.path.join(ROOT, "models", "rs7", "aircraft.json"), encoding="utf-8") as f:
        base = json.load(f)
    cases = [
        ("rs7-cli", dict(base, firmware_cli=cli["plane.cli"])),
        ("plane", {"id": "toyplane", "fdm": "plane", "actuators": ["ail_l", "ail_r", "elev", "rud", "motor"],
                   "fdm_inputs": ["ail_l", "ail_r", "elev", "rud"], "throttle": "motor",
                   "firmware_cli": cli["plane.cli"]}),
        ("plane-no-inputs", {"id": "toyplane2", "fdm": "plane", "actuators": ["ail_l", "ail_r", "elev", "rud", "motor"],
                             "throttle": "motor"}),
        ("cli-number", dict(base, firmware_cli=5)),
        ("cli-missing", dict(base, firmware_cli=os.path.join(d, "cli", "nope.cli"))),
        ("cli-null", dict(base, firmware_cli=None)),
    ]
    out = {}
    for name, a in cases:
        sub = os.path.join(d, "aircraft", "f405-" + name)
        os.makedirs(sub, exist_ok=True)
        p = os.path.join(sub, "aircraft.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(a, f)
        out[name] = p
    return out


TRAINER_WIRING = [{"device": "crsf", "port": "uart", "pad": "uart1"},
                  {"device": "esc", "pad": "S1", "actuator": "motor"},
                  {"device": "servo", "pad": "S3", "actuator": "aileron_l"},
                  {"device": "servo", "pad": "S4", "actuator": "aileron_r"},
                  {"device": "servo", "pad": "S5", "actuator": "elevator"},
                  {"device": "servo", "pad": "S6", "actuator": "rudder"}]


def plane_variants(d):
    """(name, aircraft path): the trainer with one checked mistake each (its env_default made
    absolute, the copy lives elsewhere)"""
    with open(os.path.join(ROOT, "models", "trainer", "aircraft.json"), encoding="utf-8") as f:
        base = json.load(f)
    base["env_default"] = os.path.join(ROOT, "models", "trainer", "env", "launch.json")
    base["acc_calibration"] = os.path.join(ROOT, "models", "trainer", "env", "acc-calibration.json")
    for f in base.get("functions", []):
        if "scenario" in f:
            f["scenario"] = os.path.join(ROOT, "models", "trainer", f["scenario"])
    fn = lambda a, name: next(f for f in a["functions"] if f["name"] == name)  # noqa: E731
    cases = [
        ("ok", lambda a: None),
        ("inputs-3", lambda a: a.update(fdm_inputs=["aileron_l", "aileron_r", "elevator"])),
        ("no-throttle", lambda a: a.pop("throttle")),
        ("env-default-missing", lambda a: a.update(env_default="env/nope.json")),
        ("env-default-number", lambda a: a.update(env_default=5)),
        ("env-not-list", lambda a: a.update(env="env/launch.json")),
        ("acc-cal-missing", lambda a: a.update(acc_calibration="env/nope.json")),
        ("acc-cal-list", lambda a: a.update(acc_calibration=["env/acc-calibration.json"])),
        ("heli-transmission-unneeded", lambda a: a.update(transmission={"main_rotor_gear_ratio": [1, 1]})),
        ("fn-not-list", lambda a: a.update(functions={"name": "launch"})),
        ("fn-not-object", lambda a: a["functions"].append("launch")),
        ("fn-unknown-name", lambda a: fn(a, "calibrate_acc").update(name="calibrate_baro")),
        ("fn-no-name", lambda a: fn(a, "calibrate_gyro").pop("name")),
        ("fn-twice", lambda a: a["functions"].append({"name": "launch"})),
        ("fn-unknown-key", lambda a: fn(a, "launch").update(hotkey="L")),
        ("fn-label-number", lambda a: fn(a, "launch").update(label=5)),
        ("fn-auto-string", lambda a: fn(a, "calibrate_gyro").update(auto_first_start="yes")),
        ("fn-auto-launch", lambda a: fn(a, "launch").update(auto_first_start=True)),
        ("fn-scenario-missing", lambda a: fn(a, "calibrate_acc").update(scenario="env/nope.json")),
        ("fn-scenario-number", lambda a: fn(a, "calibrate_acc").update(scenario=3)),
        ("fn-scenario-gyro", lambda a: fn(a, "calibrate_gyro").update(scenario="env/launch.json")),
        ("fn-params-list", lambda a: fn(a, "launch").update(params=[50])),
        ("fn-params-unknown", lambda a: fn(a, "calibrate_acc").update(params={"positions": 6})),
        ("fn-launch-speed-zero", lambda a: fn(a, "launch")["params"].update(speed_kmh=0)),
        ("fn-launch-pitch-far", lambda a: fn(a, "launch")["params"].update(pitch_deg=75)),
        ("fn-gyro-hold-string", lambda a: fn(a, "calibrate_gyro")["params"].update(hold_s="2")),
        ("fn-null", lambda a: a.update(functions=None)),
        ("fn-mag-params", lambda a: fn(a, "calibrate_mag").update(params={"time_s": 30})),
        ("fn-mag-scenario", lambda a: fn(a, "calibrate_mag").update(scenario="env/launch.json")),
    ]
    out = []
    for name, change in cases:
        a = copy.deepcopy(base)
        change(a)
        sub = os.path.join(d, "aircraft", "plane-" + name)
        os.makedirs(sub, exist_ok=True)
        p = os.path.join(sub, "aircraft.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(a, f)
        out.append((f"plane-{name}", p))
    return out


def f405_benches(d):
    cli = cli_files(d)
    ac = f405_aircraft(d, cli)
    out = []
    for name, w in F405_WIRING:
        out.append((name, dict(copy.deepcopy(F405), wiring=w)))
    other = [
        ("f405-no-wiring", {"wiring": []}),
        ("f405-no-base-script", {"board_config": ""}),
        ("f405-no-script-at-all", {"board_config": "", "wiring": []}),
        ("f405-base-custom", {"board_config": cli["base.cli"]}),
        ("f405-base-missing", {"board_config": os.path.join(d, "cli", "nope.cli")}),
        ("f405-profiles", {"profile": [cli["plane.cli"], cli["base.cli"]],
                           "profile_text": "set nav_fw_launch_accel = 1863\r\nsave\r\n# c\n  exit now"}),
        ("f405-profile-missing", {"profile": os.path.join(d, "cli", "nope.cli")}),
        ("f405-forbidden", {"profile": cli["bad.cli"]}),
        ("f405-forbidden-text", {"profile_text": "save\ndefaults\nset x = 1\nmsc"}),
        ("f405-nul-word", {"profile": cli["nul.cli"]}),
        ("f405-save-only", {"board_config": "", "profile_text": "save\n"}),
        ("f405-aircraft-cli", {"aircraft": ac["rs7-cli"], "profile": cli["base.cli"]}),
        ("f405-plane-no-inputs", {"aircraft": ac["plane-no-inputs"]}),
        ("f405-trainer", {"aircraft": "trainer", "wiring": TRAINER_WIRING}),
        ("f405-trainer-reversed", {"aircraft": "trainer", "wiring": TRAINER_WIRING[:5] + [
            {"device": "servo", "pad": "S6", "actuator": "rudder", "reversed": True}]}),
        ("f405-trainer-no-servos", {"aircraft": "trainer", "wiring": TRAINER_WIRING[:2]}),
        ("f405-trainer-accel-error", {"aircraft": "trainer", "wiring": TRAINER_WIRING + [
            {"device": "icm42688p", "port": "spi", "pad": "gyro1", "accel_offset_mg": 20, "accel_scale_pct": 0.5}]}),
        ("f405-accel-offset-only", {"wiring": [{"device": "icm42688p", "pad": "gyro1", "accel_offset_mg": -35.5}]}),
        ("f405-accel-scale-range", {"wiring": [{"device": "icm42688p", "pad": "gyro1", "accel_scale_pct": 11}]}),
        ("f405-plane-toy", {"aircraft": ac["plane"], "wiring": RX + [
            {"device": "esc", "pad": "S1", "actuator": "motor"},
            {"device": "servo", "pad": "S3", "actuator": "ail_l"}, {"device": "servo", "pad": "S4", "actuator": "ail_r"},
            {"device": "servo", "pad": "S5", "actuator": "elev"}, {"device": "servo", "pad": "S6", "actuator": "rud"}]}),
        ("f405-aircraft-cli-number", {"aircraft": ac["cli-number"]}),
        ("f405-aircraft-cli-missing", {"aircraft": ac["cli-missing"]}),
        ("f405-aircraft-cli-null", {"aircraft": ac["cli-null"]}),
        ("f405-environment", {"environment": {"sea_level_pa": 100800}}),
        ("f405-environment-bad", {"environment": {"qnh": 1}}),
        ("f405-time", {"time": {"icount_shift": 3, "realtime": True, "idle_skip": True, "idle_skip_n": 8}}),
        ("f405-ports", {"ports": {"control": 31260, "msp": 31261}}),
        ("f405-no-firmware", {"firmware": ""}),
        ("board-null", {"board": None}),
        ("board-list", {"board": ["speedybee-f405-wing"]}),
        ("f405-bench-cli", {"aircraft": "trainer", "wiring": TRAINER_WIRING + GPS3, "bench_cli": cli["bench.cli"]}),
        ("f405-bench-cli-disabled", {"aircraft": "trainer", "wiring": TRAINER_WIRING + GPS3, "bench_cli": cli["bench.cli"],
                                     "disabled_lines": ["device:2", "  set gps_ublox_nav_hz = 10 ", "aircraft:999",
                                                        "feature PWM_OUTPUT_ENABLE", "bench:1", "nope", "Device:1",
                                                        "board:x", "device:2"]}),
        ("f405-bench-cli-2", {"aircraft": "trainer", "wiring": TRAINER_WIRING + GPS3, "bench_cli": cli["bench2.cli"],
                              "profile_text": "resource MOTOR 1 A01\nmap AETR\nbeeper ON_USB"}),
        ("f405-bench-cli-profiles", {"profile": [cli["plane.cli"], cli["base.cli"]], "bench_cli": cli["base.cli"],
                                     "profile_text": "feature GPS\nset gyro_main_lpf_hz = 90"}),
        ("f405-bench-cli-missing", {"bench_cli": os.path.join(d, "cli", "nope.cli")}),
        ("f405-bench-cli-forbidden", {"bench_cli": cli["bench-bad.cli"]}),
        ("f405-bench-cli-number", {"bench_cli": 5}),
        ("f405-bench-cli-empty", {"bench_cli": "", "disabled_lines": []}),
        ("f405-bench-cli-nul", {"bench_cli": cli["nul.cli"], "disabled_lines": ["save", "bench:1"]}),
        ("f405-disabled-not-list", {"disabled_lines": "device:1"}),
        ("f405-disabled-number", {"disabled_lines": ["device:1", 3]}),
        ("f405-disabled-null", {"disabled_lines": None}),
        ("f405-device-config-twice", {"wiring": RX + [{"device": "msp_osd", "pad": "uart4"}],
                                      "profile_text": "serial 3 1 115200 57600 0 115200"}),
    ]
    for name, change in other:
        s = copy.deepcopy(F405)
        s.update(change)
        out.append((name, s))
    for name, p in plane_variants(d):
        out.append((f"f405-{name}", dict(copy.deepcopy(F405), aircraft=p, wiring=TRAINER_WIRING)))
    # the plane on the nexus-xr: no plane FDM on that machine
    names = iter(["aileron_l", "aileron_r", "elevator", "rudder"])
    w = [dict(e, actuator=next(names)) if e.get("device") == "servo" else e for e in NEW["wiring"]]
    out.append(("nexus-trainer", dict(copy.deepcopy(NEW), aircraft="trainer", wiring=w)))
    # bench lines are a CLI board's: a blob board rejects them
    out.append(("nexus-bench-cli", dict(copy.deepcopy(NEW), bench_cli=cli["bench.cli"])))
    out.append(("nexus-disabled-lines", dict(copy.deepcopy(NEW), disabled_lines=["device:1"])))
    out.append(("nexus-disabled-empty", dict(copy.deepcopy(NEW), disabled_lines=[], bench_cli=None)))
    # a nexus-xr bench with an aircraft that has a firmware_cli: the blob board ignores it
    out.append(("nexus-aircraft-cli", dict(copy.deepcopy(NEW), aircraft=ac["rs7-cli"])))
    out.append(("nexus-aircraft-cli-number", dict(copy.deepcopy(NEW), aircraft=ac["cli-number"])))
    return out


# phase 4a: the F405's sensors (spl06 props, the compass, the GPS, the battery)
GPS = {"device": "ublox", "port": "uart", "pad": "uart3"}
MAG = {"device": "qmc5883l", "port": "i2c", "pad": "i2c1"}
BAT = {"device": "battery", "port": "power", "pad": "batt1"}
SENSORS_WIRING = [
    ("f405-sensors", RX + [GPS, MAG, BAT]),
    ("f405-sensors-props", RX + [
        dict(GPS, generation="m8", baud=115200, fix_delay_s=5, noise_m=0, seed=7),
        dict(MAG, mount="CW90", declination_deg=-3, inclination_deg=45.5, noise_mg=0),
        dict(BAT, cells=3, capacity_mah=2200, start_soc=0.75, resistance_mohm=12.5, current_scale=1.1, fc_current_a=0.5),
        {"device": "spl06", "port": "i2c", "pad": "i2c1", "noise_scale": 0}]),
    ("f405-gps-uart5", RX + [dict(GPS, pad="uart5")]),
    ("f405-gps-uart6", [dict(GPS, pad="uart6")]),
    ("f405-gps-on-receiver", RX + [dict(GPS, pad="uart1")]),
    ("f405-two-gps", RX + [GPS, dict(GPS, pad="uart4")]),
    ("f405-two-batteries", [BAT, BAT]),
    ("f405-two-compasses", [MAG, MAG]),
    ("f405-battery-on-uart", [dict(BAT, pad="uart2")]),
    ("f405-compass-on-uart", [dict(MAG, pad="uart2")]),
    ("f405-mag-bad-mount", [dict(MAG, mount="CW45")]),
    ("f405-battery-cells-float", [dict(BAT, cells=4.5)]),
    ("f405-battery-soc-range", [dict(BAT, start_soc=1.5)]),
    ("f405-gps-bad-prop", [dict(GPS, rate=5)]),
    ("f405-compass-detached-baro", RX + [MAG, {"device": "spl06", "detached": True}]),
    ("f405-generic-compass", RX + [{"device": "compass", "pad": "i2c1"}]),
]


def sensors_benches(d):
    out = [(name, dict(copy.deepcopy(F405), wiring=w)) for name, w in SENSORS_WIRING]
    out.append(("f405-sensors-environment", dict(copy.deepcopy(F405), wiring=RX + [GPS, MAG],
                                                 environment={"field_elevation_m": 990, "temp_c": 25})))
    out.append(("f405-trainer-sensors", dict(copy.deepcopy(F405), aircraft="trainer",
                                             wiring=TRAINER_WIRING + [GPS, MAG, BAT])))
    return out


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for i, (name, s) in enumerate(benches(d) + f405_benches(d) + sensors_benches(d)):
        with open(os.path.join(d, f"{i:03d}-{name}.json"), "w", encoding="utf-8") as f:
            json.dump(s, f, ensure_ascii=False)


if __name__ == "__main__":
    main()
