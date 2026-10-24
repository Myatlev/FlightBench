#!/usr/bin/env python3
"""Unit tests of the wiring model (scripts/wiring.py, session.py, manifest.py).

  python3 core/qemu/scripts/test_wiring.py -v
"""
import copy, json, os, re, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest, session, wiring  # noqa: E402

ROOT = wiring.ROOT
CFG = os.path.join(ROOT, "boards", "nexus-xr", "NEXUS_XR.config")
SESS = os.path.join(ROOT, "core", "qemu", "sessions")
CATALOG = wiring.device_catalog()
LEGACY = {"imu": "on", "crsf": "uart5", "esc": "on", "baro": "on", "servos": "15",
          "actuators": "swash_a:swash_b:swash_c:tail:motor"}


def board_json_with(device, **changes):
    """a temp copy of the real board.json with one onboard entry changed (caller unlinks)"""
    with open(os.path.join(ROOT, "boards", "nexus-xr", "board.json"), encoding="utf-8") as f:
        bj = json.load(f)
    for ob in bj["onboard"]:
        if ob["device"] == device:
            for k, v in changes.items():
                if v is None:
                    ob.pop(k, None)
                else:
                    ob[k] = v
    return tmp_json(bj)


def app_slots(cfg):
    """app/scripts/WiringGraph.cs BoardInfo.Slots, re-implemented independently of wiring.py"""
    out = []
    for prefix, kind, res in (("gyro", "spi", "GYRO_CS"), ("i2c", "i2c", "I2C_SCL"),
                              ("uart", "uart", "SERIAL_(?:TX|RX)"), ("motor", "motor", "MOTOR"),
                              ("servo", "servo", "SERVO")):
        nums = sorted({int(n) for n in re.findall(r"^resource " + res + r" (\d+) ", cfg, re.M)})
        out += [(f"{prefix}{n}", kind) for n in nums]
    return out


def load(name):
    with open(os.path.join(SESS, name), encoding="utf-8") as f:
        return json.load(f)


def resolve(s, board_json=None, aircraft=None):
    notes = []
    board = wiring.load_board("nexus-xr", CFG, board_json)
    ac = aircraft or wiring.load_aircraft(wiring.aircraft_path(s.get("aircraft")))
    entries = wiring.resolve(s["wiring"], board, CATALOG, ac, notes.append)
    return entries, wiring.machine_options(entries, ac, CATALOG), notes


def tmp_json(obj):
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(obj, f)
    f.close()
    return f.name


def rs7_aircraft():
    with open(os.path.join(ROOT, "models", "rs7", "aircraft.json"), encoding="utf-8") as f:
        return json.load(f)


class Pads(unittest.TestCase):
    def test_pads_match_app_slots(self):
        with open(CFG, encoding="utf-8") as f:
            cfg = f.read()
        app = app_slots(cfg)
        self.assertEqual(len(app), 13)
        b = wiring.load_board("nexus-xr", CFG)
        pads = {p["id"]: p for p in b["pads"]}
        for sid, kind in app:
            self.assertIn(sid, pads)
            self.assertEqual(pads[sid]["kind"], kind)
        self.assertEqual([(s["id"], s["kind"]) for s in wiring.slots_of(b["pads"])], app)
        m = manifest.manifest()
        self.assertEqual([(s["id"], s["kind"]) for s in m["boards"][0]["slots"]], app)

    def test_labels_onboard_and_config_facts(self):
        b = wiring.load_board("nexus-xr", CFG)
        pads = {p["id"]: p for p in b["pads"]}
        self.assertEqual(pads["uart4"]["label"], "PORT-A")
        self.assertEqual(pads["servo4"]["label"], "TAIL")
        self.assertEqual(pads["uart2"]["flags"], ["rx_only"])
        self.assertEqual(pads["motor1"]["timer"], "TIM1_CH2")
        self.assertEqual(pads["motor1"]["dma"], "DMA2_S6_CH0")
        self.assertEqual(pads["freq1"]["kind"], "capture")
        self.assertTrue(pads["gyro1"]["onboard"] and pads["gyro1"]["detachable"])
        self.assertTrue(pads["i2c3"]["onboard"] and pads["i2c3"]["detachable"])
        self.assertEqual(pads["i2c3"]["devices"], ["dps310"])
        self.assertEqual(pads["uart5"]["device"], "crsf")

    def test_manifest_old_and_new_fields(self):
        m = manifest.manifest()
        json.loads(json.dumps(m))
        devs = {d["id"]: d for d in m["devices"]}
        for d in devs.values():
            self.assertIn("kind", d)
            self.assertEqual(d["ports"][0]["kind"], d["kind"])
        self.assertEqual([p["id"] for p in devs["esc"]["ports"]], ["signal", "tlm", "rpm"])
        self.assertTrue(all("label" in p and "onboard" in p for p in m["boards"][0]["pads"]))
        self.assertEqual({b["id"]: b["firmware_family"] for b in m["boards"]},
                         {"nexus-xr": "rotorflight", "speedybee-f405-wing": "inav"})


class Sessions(unittest.TestCase):
    def test_rs7_old_form_unchanged(self):
        e, o, _ = resolve(load("rs7.json"))
        self.assertEqual(o, LEGACY)

    def test_rs7_pads_by_labels(self):
        e, o, notes = resolve(load("rs7-pads.json"))
        self.assertEqual(o, LEGACY)
        self.assertTrue(any("esc.tlm on TELEM (uart2)" in n for n in notes))

    def reject(self, wiring_list, msg, board_json=None):
        s = load("rs7-pads.json")
        s["wiring"] = wiring_list
        with self.assertRaises(wiring.WiringError) as cm:
            resolve(s, board_json)
        self.assertIn(msg, str(cm.exception))
        return str(cm.exception)

    def test_reject_unknown_pad(self):
        self.reject([{"device": "servo", "port": "pwm", "pad": "S9", "actuator": "tail"}], "unknown pad 'S9'")

    def test_reject_servo_on_uart(self):
        self.reject([{"device": "servo", "port": "pwm", "pad": "PORT-A"}],
                    "servo.pwm is a servo port, pad PORT-A (uart4) is uart")

    def test_reject_soldered_removal(self):
        name = board_json_with("dps310", detachable=False)
        try:
            self.reject([{"device": "dps310", "detached": True}], "is soldered (detachable: false)", name)
        finally:
            os.unlink(name)
        with open(os.path.join(ROOT, "boards", "nexus-xr", "board.json"), encoding="utf-8") as f:
            bj = json.load(f)
        for ob in bj["onboard"]:
            if ob["device"] == "icm42688p":
                del ob["detachable"]
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(bj, f)
        try:
            self.reject([{"device": "icm42688p", "detached": True}],
                        "onboard icm42688p on gyro1 is soldered (detachable: false)", f.name)
        finally:
            os.unlink(f.name)

    def test_gyro_detach_allowed(self):
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "icm42688p", "detached": True})
        self.assertEqual(resolve(s)[1]["imu"], "off")

    def test_baro_modelled_and_detachable(self):
        e, o, notes = resolve(load("rs7-pads.json"))
        baro = [x for x in e if x["device"] == "dps310"]
        self.assertTrue(len(baro) == 1 and baro[0]["onboard"] and baro[0]["modelled"] and baro[0]["pad"] == "i2c3")
        self.assertEqual(o["baro"], "on")
        self.assertFalse(any("dps310" in n for n in notes))
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "dps310", "detached": True})
        self.assertEqual(resolve(s)[1], dict(LEGACY, baro="off"))
        # a compass next to the baro: accepted, left out with a note, the baro stays
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "compass", "pad": "i2c3"})
        e, o, notes = resolve(s)
        self.assertEqual(o["baro"], "on")
        self.assertTrue(any("compass.i2c on i2c3 (wired): not modelled on this machine, left out" in n for n in notes))

    def test_environment(self):
        self.assertEqual(session.environment(None), "")
        self.assertEqual(session.environment({}), "101325.0:0.0:22.0")
        self.assertEqual(session.environment({"sea_level_pa": 100800, "field_elevation_m": 990}), "100800.0:990.0:22.0")
        # full precision: the value the machine parses (%lf) is the session's double exactly
        e = session.environment({"sea_level_pa": 101325.123456789, "field_elevation_m": 989.987654321, "temp_c": -12.3456789})
        self.assertEqual([float(x) for x in e.split(":")], [101325.123456789, 989.987654321, -12.3456789])
        for bad in ({"sea_level_pa": 5}, {"qnh": 1}, {"temp_c": "20"}, []):
            with self.assertRaises(SystemExit):
                session.environment(bad)

    def test_no_wiring_keeps_the_baro(self):
        # a session without `wiring`: no machine options at all -> the machine defaults (baro=on)
        s = {"aircraft": "models/rs7/aircraft.json"}
        board, ac, ap, entries, opts = session.assembly(s)
        self.assertIsNone(entries)
        self.assertEqual(opts, {})
        with open(os.path.join(ROOT, "core", "qemu", "hw", "arm", "nexus_xr.c"), encoding="utf-8") as f:
            self.assertIn("NEXUS_XR_MACHINE(obj)->baro_present = s->board->baro;", f.read())
        with open(os.path.join(ROOT, "core", "qemu", "hw", "arm", "stm32_soc.c.inc"), encoding="utf-8") as f:
            soc = f.read()
        nexus = soc[soc.index("static const FbBoard board_nexus_xr"):]
        self.assertIn(".baro = true", nexus[:nexus.index("};")])

    def test_reject_duplicate_pad(self):
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "swash_a"},
                     {"device": "servo", "pad": "servo1", "actuator": "swash_b"}], "pad S1 (servo1) used by")
        self.reject([{"device": "gps", "pad": "uart5"}], "pad Int.Rx (uart5) used by gps.uart")

    def test_i2c_bus_shared(self):
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "compass", "pad": "i2c3"})
        resolve(s)

    def test_reject_other(self):
        self.reject([{"device": "nope", "pad": "uart3"}], "unknown device 'nope'")
        self.reject([{"device": "crsf", "pad": "TELEM"}], "pad TELEM (uart2) has no TX")
        self.reject([{"device": "icm42688p", "port": "int", "pad": "gyro1"}], "comes with the spi pad")
        self.reject([{"device": "esc", "port": "nope", "pad": "motor1"}], "has no port 'nope'")
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "flap"}], "not one of the aircraft's")

    def test_without_crsf_esc(self):
        # old form: the list only adds; receiver and ESC stay (the machine before the wiring model)
        s = load("rs7.json")
        s["wiring"] = [w for w in s["wiring"] if w["device"] not in ("crsf", "esc")]
        _, o, notes = resolve(s)
        self.assertEqual(o, LEGACY)
        self.assertTrue(any(n.startswith("old-form session: receiver/ESC/servos assumed present") for n in notes))
        # new form: complete, what is not wired is not there
        s = load("rs7-pads.json")
        s["wiring"] = [w for w in s["wiring"] if w["device"] != "esc"] + [{"device": "crsf", "detached": True}]
        o = resolve(s)[1]
        self.assertEqual((o["crsf"], o["esc"]), ("off", "off"))

    def reject_old(self, wiring_list, msg):
        s = load("rs7.json")
        s["wiring"] = wiring_list
        with self.assertRaises(wiring.WiringError) as cm:
            resolve(s)
        self.assertIn(msg, str(cm.exception))

    def test_reject_wired_and_detached(self):
        self.reject([{"device": "icm42688p", "detached": True}, {"device": "icm42688p", "pad": "gyro1"}],
                    "is both wired and detached")

    def test_reject_detached_not_bool(self):
        self.reject([{"device": "icm42688p", "detached": "yes"}], "detached must be true|false")
        self.reject([{"device": "icm42688p", "detached": 1}], "detached must be true|false")

    def test_reject_reversed_not_bool(self):
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "swash_a", "reversed": "true"}],
                    "servo reversed 'true': need true|false")
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "swash_a", "reversed": 1}],
                    "need true|false")

    def test_reject_actuator_twice(self):
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "tail"},
                     {"device": "servo", "pad": "servo4", "actuator": "tail"}], "actuator tail is already driven by")

    def test_reject_bad_actuator_name(self):
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "swash a"}], "letters, digits and _ only")
        self.reject([{"device": "servo", "pad": "servo1", "actuator": 3}], "letters, digits and _ only")

    def test_reject_actuator_roles(self):
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "motor"}],
                    "servo actuator 'motor' is not one of the aircraft's FDM inputs")
        self.reject([{"device": "esc", "pad": "motor1", "actuator": "tail"}],
                    "esc actuator 'tail': the ESC's signal drives the aircraft's throttle 'motor'")
        self.reject([{"device": "gps", "pad": "PORT-A", "actuator": "tail"}], "gps has no prop 'actuator'")
        self.reject_old([{"device": "servo", "slot": "servo2", "actuator": "motor"}], "not one of the aircraft's FDM inputs")
        # the ESC keeps the throttle's name
        s = load("rs7-pads.json")
        s["wiring"][0]["actuator"] = "motor"
        self.assertEqual(resolve(s)[1]["actuators"], LEGACY["actuators"])

    def test_reject_unknown_prop(self):
        self.reject([{"device": "servo", "pad": "servo1", "actuator": "swash_a", "trim": 5}],
                    "wiring[0]: servo has no prop 'trim'")
        self.reject([{"device": "icm42688p", "pad": "gyro1", "gyro_noise": 0.1}], "icm42688p has no prop 'gyro_noise'")
        self.reject_old([{"device": "esc", "slot": "motor1", "timeout": 5}], "esc has no prop 'timeout'")
        self.reject([{"device": "esc", "pad": "motor1", "telemetry": "pwm"}], "need one of bidir, serial")

    def test_app_empty_props_tolerated(self):
        """the app writes actuator "", reversed false, option "" on every entry"""
        s = load("rs7-pads.json")
        for w in s["wiring"]:
            w.setdefault("actuator", "")
            w.setdefault("reversed", False)
            w.setdefault("option", "")
        self.assertEqual(resolve(s)[1], LEGACY)

    def test_prop_ranges(self):
        self.reject([{"device": "esc", "pad": "motor1", "signal_timeout_ms": 2001}],
                    "esc signal_timeout_ms 2001: need an integer 0..2000")
        self.reject([{"device": "esc", "pad": "motor1", "signal_timeout_ms": 50.5}], "need an integer 0..2000")
        self.reject_old([{"device": "esc", "slot": "motor1", "signal_timeout_ms": -1}], "need an integer 0..2000")
        self.reject([{"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 51}],
                    "icm42688p gyro_noise_dps 51: need a number 0..50")
        self.reject([{"device": "icm42688p", "pad": "gyro1", "accel_noise_g": "0.1"}], "need a number 0..2")
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 0.5})
        s["wiring"][0]["signal_timeout_ms"] = 0
        o = resolve(s)[1]
        self.assertEqual((o["imu-noise"], o["esc-timeout-ms"]), ("0.5:0.002", "0"))

    def test_reject_mixed_forms(self):
        self.reject([{"device": "servo", "slot": "servo1"}, {"device": "servo", "pad": "servo2"}],
                    "mixes the old form")
        self.reject([{"device": "servo", "slot": "servo1", "pad": "servo1"}], "mixes the old form")
        self.reject([{"device": "icm42688p", "slot": "gyro1"}, {"device": "crsf", "detached": True}],
                    "mixes the old form")

    def test_i2c_same_device_twice(self):
        self.reject([{"device": "compass", "pad": "i2c3"}, {"device": "compass", "pad": "i2c3"}],
                    "bus i2c3: two compass (same I2C address)")
        self.reject([{"device": "dps310", "pad": "i2c3"}, {"device": "dps310", "pad": "i2c3"}], "two dps310")

    def test_reject_non_unique_label(self):
        with open(os.path.join(ROOT, "boards", "nexus-xr", "board.json"), encoding="utf-8") as f:
            bj = json.load(f)
        for p in bj["pads"]:
            if p["id"] == "uart6":
                p["label"] = "port-a"           # case-insensitive clash with uart4's PORT-A
        name = tmp_json(bj)
        try:
            with self.assertRaises(wiring.WiringError) as cm:
                wiring.load_board("nexus-xr", CFG, name)
            self.assertIn("label 'port-a' is not unique", str(cm.exception))
            for p in bj["pads"]:
                if p["id"] == "uart6":
                    p["label"] = "uart3"        # a label equal to another pad's id
            with open(name, "w") as f:
                json.dump(bj, f)
            with self.assertRaises(wiring.WiringError) as cm:
                wiring.load_board("nexus-xr", CFG, name)
            self.assertIn("is not unique", str(cm.exception))
        finally:
            os.unlink(name)

    def test_old_form_crsf_on_uart3(self):
        s = load("rs7.json")
        s["wiring"] = [{"device": "icm42688p", "slot": "gyro1"}, {"device": "crsf", "slot": "uart3"}]
        _, o, notes = resolve(s)
        self.assertEqual(o, dict(LEGACY, crsf="uart3", baro="off"))   # the list omits the baro
        assumed = [n for n in notes if n.startswith("old-form session: receiver/ESC/servos assumed present")]
        self.assertEqual(len(assumed), 1)
        self.assertIn("ESC on motor1", assumed[0])
        self.assertNotIn("receiver", assumed[0].split("(", 1)[1])

    def test_old_form_omits_soldered(self):
        s = load("rs7.json")
        s["wiring"] = [w for w in s["wiring"] if w["device"] != "dps310"]
        name = board_json_with("dps310", detachable=False)
        try:
            _, o, notes = resolve(s, name)
        finally:
            os.unlink(name)
        self.assertEqual(o, LEGACY)
        self.assertTrue(any("onboard dps310 on i2c3 not in the (old-form) wiring: it is soldered, kept" in n
                            for n in notes))
        # the real board.json: the baro is detachable, an old-form list without it has none
        _, o, notes = resolve(s)
        self.assertEqual(o, dict(LEGACY, baro="off"))
        self.assertTrue(any("onboard dps310 on i2c3 not in the (old-form) wiring: detached" in n for n in notes))

    def test_old_form_unplaceable_is_warning(self):
        s = load("rs7.json")
        s["wiring"] += [{"device": "nope", "slot": "uart3"}, {"device": "servo", "slot": "servo9"},
                        {"device": "servo", "slot": "PORT-A"}, {"device": "servo", "slot": "servo1", "actuator": "swash_b"}]
        _, o, notes = resolve(s)
        self.assertEqual(o, LEGACY)
        self.assertEqual(sum("left out (old-form session)" in n for n in notes), 4)

    def test_app_smoke_gyro_only(self):
        """the app's smoke bench: gyro only, old form, the app's empty props"""
        s = load("rs7.json")
        s["wiring"] = [{"device": "icm42688p", "slot": "gyro1", "actuator": "", "reversed": False, "option": ""}]
        _, o, notes = resolve(s)
        self.assertEqual(o, dict(LEGACY, baro="off"))       # the old-form list omits the (detachable) baro
        self.assertTrue(any("assumed present (not in the list: receiver on Int.Rx (uart5); ESC on motor1; "
                            "servos on servo1, servo2, servo3, servo4)" in n for n in notes))

    def test_aircraft_checks(self):
        def bad(change, msg):
            a = rs7_aircraft()
            change(a)
            d = tempfile.mkdtemp()
            p = os.path.join(d, "aircraft.json")
            with open(p, "w") as f:
                json.dump(a, f)
            with self.assertRaises(wiring.WiringError) as cm:
                wiring.load_aircraft(p)
            self.assertIn(msg, str(cm.exception))
            s = load("rs7-pads.json")
            s["aircraft"] = p
            r = subprocess.run([sys.executable, os.path.join(HERE, "session.py"), tmp_json(s), d],
                               capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn(msg, r.stderr)
        bad(lambda a: a.pop("transmission"), "transmission.main_rotor_gear_ratio: missing")
        bad(lambda a: a["transmission"].update(main_rotor_gear_ratio=[0, 116]), "need [pinion, main], 2 numbers > 0")
        bad(lambda a: a["transmission"].update(main_rotor_gear_ratio=[13]), "need [pinion, main]")
        bad(lambda a: a["esc"].update(kv=0), "esc.kv 0: need a number > 0")
        bad(lambda a: a["esc"].update(kv="520"), "esc.kv '520'")
        bad(lambda a: a["esc"].update(poles=7), "esc.poles 7: need an even integer 2..64")
        bad(lambda a: a["esc"].update(poles=66), "esc.poles 66: need a number 2..64")
        bad(lambda a: a["esc"].update(kvv=1), "esc.kvv: unknown key")
        bad(lambda a: a.update(fdm_params=5), "fdm_params: need an object")
        bad(lambda a: a.update(centre_us={"tail": 2500}), "centre_us.tail 2500: need an integer 1000..2000")
        bad(lambda a: a.update(centre_us={"motor": 1500}), "centre_us.motor: not one of the fdm_inputs")
        bad(lambda a: a.update(throttle="tail"), "throttle 'tail' is also an fdm_input")

    def test_plane_aircraft(self):
        a = wiring.load_aircraft(os.path.join(ROOT, "models", "trainer", "aircraft.json"))
        self.assertTrue(wiring.has_fdm(a))
        self.assertTrue(wiring.fdm_on(a, wiring.load_board("speedybee-f405-wing")))
        self.assertFalse(wiring.fdm_on(a, wiring.load_board("nexus-xr")))
        self.assertEqual(wiring.roles(a), (["aileron_l", "aileron_r", "elevator", "rudder"], "motor"))

        def bad(change, msg):
            with open(os.path.join(ROOT, "models", "trainer", "aircraft.json"), encoding="utf-8") as f:
                a = json.load(f)
            a["env_default"] = os.path.join(ROOT, "models", "trainer", "env", "launch.json")
            for f in a["functions"]:
                if "scenario" in f:
                    f["scenario"] = os.path.join(ROOT, "models", "trainer", f["scenario"])
            change(a)
            d = tempfile.mkdtemp()
            p = os.path.join(d, "aircraft.json")
            with open(p, "w") as f:
                json.dump(a, f)
            with self.assertRaises(wiring.WiringError) as cm:
                wiring.load_aircraft(p)
            self.assertIn(msg, str(cm.exception))
        bad(lambda a: a.update(fdm_inputs=["aileron_l"]), "fdm_inputs: the plane FDM needs 4")
        bad(lambda a: a.pop("throttle"), "throttle: the plane FDM needs the ESC's actuator name")
        bad(lambda a: a.update(env_default="env/nope.json"), "env_default 'env/nope.json': no such file")
        self.assertEqual([f["name"] for f in a["functions"]], ["calibrate_acc", "calibrate_mag", "calibrate_gyro", "launch"])
        bad(lambda a: a["functions"][0].update(name="calibrate_baro"),
            "functions[0].name 'calibrate_baro': need one of calibrate_acc, calibrate_gyro, launch, calibrate_mag")
        bad(lambda a: a["functions"].append({"name": "launch"}), "functions[4].name 'launch': listed twice")
        bad(lambda a: a["functions"][3].update(auto_first_start=True),
            "functions[3].auto_first_start: launch is never part of the first start")
        bad(lambda a: a["functions"][1].update(params={"time_s": 30}),
            "functions[1].params.time_s: unknown key (known: none)")
        bad(lambda a: a["functions"][0].update(scenario="env/nope.json"),
            "functions[0].scenario 'env/nope.json': no such file next to the aircraft")
        bad(lambda a: a["functions"][1].update(scenario="env/launch.json"),
            "functions[1].scenario: only calibrate_acc takes a scenario (its poses)")
        bad(lambda a: a["functions"][3]["params"].update(pitch_deg=75),
            "functions[3].params.pitch_deg 75: need a number -60..60")
        bad(lambda a: a["functions"][0].update(params={"positions": 6}),
            "functions[0].params.positions: unknown key (known: none)")

    def test_board_config_without_pin_comments(self):
        with open(CFG, encoding="utf-8") as f:
            cfg = "".join(line for line in f if not line.startswith("# pin"))
        name = tmp_json({})
        with open(name, "w") as f:
            f.write(cfg)
        notes = []
        try:
            b = wiring.load_board("nexus-xr", name, None, notes.append)
        finally:
            os.unlink(name)
        self.assertTrue(any("no '# pin' comments" in n and "timers and DMA are unknown" in n for n in notes))
        self.assertNotIn("timer", {p["id"]: p for p in b["pads"]}["motor1"])
        notes = []
        wiring.load_board("nexus-xr", CFG, None, notes.append)
        self.assertFalse(any("# pin" in n for n in notes))

    def test_external_receiver_and_reversed_servo(self):
        s = load("rs7-pads.json")
        s["wiring"] += [{"device": "crsf", "detached": True}, {"device": "crsf", "pad": "PORT-C"}]
        s["wiring"][4]["reversed"] = True
        o = resolve(s)[1]
        self.assertEqual(o["crsf"], "uart3")
        self.assertEqual(o["servo-rev"], "2")

    def test_app_style_session(self):
        """what app/scripts/Bench.cs ToSession writes: every entry with actuator/reversed/option,
        the aircraft as the app machine's own repo path"""
        s = load("rs7.json")
        for w in s["wiring"]:
            w.setdefault("actuator", "")
            w.setdefault("reversed", False)
            w["option"] = w.pop("telemetry", "")
        s["aircraft"] = "C:/Users/you/flightbench/models/rs7/aircraft.json"
        self.assertEqual(resolve(s)[1], LEGACY)
        self.assertEqual(wiring.aircraft_path(s["aircraft"]), os.path.join(ROOT, "models", "rs7", "aircraft.json"))

    def test_session_script(self):
        """session.py end to end: old form and new form give the same machine options"""
        outs = []
        for name in ("rs7.json", "rs7-pads.json"):
            r = subprocess.run([sys.executable, os.path.join(HERE, "session.py"), os.path.join(SESS, name),
                                tempfile.gettempdir()], capture_output=True, text=True, check=True)
            outs.append(re.search(r"^SESSION_OPTS=(.*)$", r.stdout, re.M).group(1))
        self.assertEqual(outs[0], outs[1])
        self.assertIn("aircraft-file=", outs[0])


class Osd(unittest.TestCase):
    """the MSP DisplayPort OSD device (msp_osd): on a free UART, its CLI lines with the pad's serial index"""

    def test_osd_on_port_a(self):
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "msp_osd", "port": "uart", "pad": "PORT-A"})
        entries, o, _ = resolve(s)
        self.assertEqual(o["osd"], "uart4")
        board = wiring.load_board("nexus-xr", CFG)
        cfg = wiring.device_config(entries, board, CATALOG)
        self.assertIn("serial 3 1 115200 57600 0 115200", cfg)
        self.assertIn("set displayport_msp_serial = 3", cfg)
        self.assertTrue(any(x.startswith("set osd_crosshairs_pos") for x in cfg))
        self.assertIn("set osd_displayport_device = AUTO", cfg)

    def test_osd_old_form_and_other_uarts(self):
        s = load("rs7.json")
        s["wiring"].append({"device": "msp_osd", "slot": "uart6"})
        entries, o, _ = resolve(s)
        self.assertEqual(o["osd"], "uart6")
        cfg = wiring.device_config(entries, wiring.load_board("nexus-xr", CFG), CATALOG)
        self.assertIn("set displayport_msp_serial = 5", cfg)

    def test_osd_absent_leaves_options(self):
        s = load("rs7.json")
        entries, o, _ = resolve(s)
        self.assertNotIn("osd", o)
        self.assertEqual(wiring.device_config(entries, wiring.load_board("nexus-xr", CFG), CATALOG), [])

    def test_osd_rejected_where_it_cannot_go(self):
        for pad in ("TELEM", "Int.Rx", "S1"):   # RX-only, the onboard receiver's pad, a servo pad
            s = load("rs7-pads.json")
            s["wiring"].append({"device": "msp_osd", "port": "uart", "pad": pad})
            with self.assertRaises(wiring.WiringError, msg=pad):
                resolve(s)

    def test_osd_twice_rejected(self):
        """one OSD per session, in either form"""
        s = load("rs7-pads.json")
        s["wiring"] += [{"device": "msp_osd", "port": "uart", "pad": "PORT-A"},
                        {"device": "msp_osd", "port": "uart", "pad": "PORT-B"}]
        with self.assertRaisesRegex(wiring.WiringError, r"one OSD per session: msp_osd on PORT-A \(uart4\).*PORT-B"):
            resolve(s)
        s = load("rs7.json")
        s["wiring"] += [{"device": "msp_osd", "slot": "uart4"}, {"device": "msp_osd", "slot": "uart6"}]
        with self.assertRaisesRegex(wiring.WiringError, "one OSD per session"):
            resolve(s)

    def test_osd_on_uart5_receiver_elsewhere(self):
        """the onboard receiver detached, crsf on PORT-C: the OSD can take uart5 (Int.Rx)"""
        s = load("rs7-pads.json")
        s["wiring"] += [{"device": "crsf", "detached": True}, {"device": "crsf", "port": "uart", "pad": "PORT-C"},
                        {"device": "msp_osd", "port": "uart", "pad": "uart5"}]
        entries, o, _ = resolve(s)
        self.assertEqual((o["crsf"], o["osd"]), ("uart3", "uart5"))
        cfg = wiring.device_config(entries, wiring.load_board("nexus-xr", CFG), CATALOG)
        self.assertIn("set displayport_msp_serial = 4", cfg)
        self.assertIn("serial 4 1 115200 57600 0 115200", cfg)
        self.assertIn("set osd_displayport_device = AUTO", cfg)
        # without detaching, uart5 is the onboard receiver's
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "msp_osd", "port": "uart", "pad": "uart5"})
        with self.assertRaises(wiring.WiringError):
            resolve(s)

    def test_osd_manifest(self):
        m = manifest.manifest(CFG)
        d = {x["id"]: x for x in m["devices"]}["msp_osd"]
        self.assertTrue(d["implemented"])
        self.assertEqual(d["category"], "osd")
        self.assertEqual(d["ports"], [{"id": "uart", "kind": "uart", "dir": "both", "label": "MSP DisplayPort serial"}])
        self.assertEqual(d["signals"], ["osd.frames"])
        self.assertEqual([f["id"] for f in d["faults"]], ["link_lost"])
        pads = {p["id"]: p for p in m["boards"][0]["pads"]}
        self.assertIn("msp_osd", pads["uart4"]["devices"])

    def test_osd_session_script(self):
        s = load("rs7-pads.json")
        s["wiring"].append({"device": "msp_osd", "port": "uart", "pad": "PORT-A"})
        f = tmp_json(s)
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, os.path.join(HERE, "session.py"), f, tmp],
                               capture_output=True, text=True, check=True)
            self.assertIn("osd=uart4", re.search(r"^SESSION_OPTS=(.*)$", r.stdout, re.M).group(1))
            prof = re.search(r"^PROFILE=(.*)$", r.stdout, re.M).group(1).strip("'").split()
            self.assertTrue(prof[-1].endswith("device_config.txt"))
            with open(prof[-1], encoding="utf-8") as fh:
                self.assertIn("set displayport_msp_serial = 3", fh.read())
        os.unlink(f)


F405 = "speedybee-f405-wing"
F405_RX = [{"device": "crsf", "port": "uart", "pad": "uart1"}]


def resolve_f405(w, aircraft=None):
    notes = []
    board = wiring.load_board(F405)
    ac = aircraft or wiring.load_aircraft(wiring.aircraft_path("rs7"))
    entries = wiring.resolve(w, board, CATALOG, ac, notes.append)
    return entries, wiring.machine_options(entries, ac, CATALOG, board), notes


def run_session(s):
    """session.py run mode on a bench dict: (returncode, {NAME: value}, stderr, CONFIG_CLI bytes)"""
    f = tmp_json(s)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, os.path.join(HERE, "session.py"), f, tmp], capture_output=True, text=True)
            env = {k: " ".join(__import__("shlex").split(v)) for k, v in
                   (line.split("=", 1) for line in r.stdout.splitlines())}
            cli = b""
            if env.get("CONFIG_CLI"):
                with open(env["CONFIG_CLI"], "rb") as fh:
                    cli = fh.read()
            return r.returncode, env, r.stderr, cli
    finally:
        os.unlink(f)


class SpeedyBee(unittest.TestCase):
    """the second board: iNav 9.1.0 on the SpeedyBee F405 WING, pads from board.json, a CLI script"""

    def test_board(self):
        b = wiring.load_board(F405)
        pads = {p["id"]: p for p in b["pads"]}
        self.assertEqual((b["machine"], b["config"], b["msp_uart"], b["beeper"], b["mcu"]),
                         (F405, "cli", "uart6", "C15:inv", "STM32F405"))
        self.assertTrue(b["defaults_cli"].endswith(os.path.join("boards", F405, "defaults.cli")))
        self.assertEqual([p for p in pads if p.startswith("output")], [f"output{i}" for i in range(1, 12)])
        self.assertEqual((pads["output1"]["kind"], pads["output1"]["label"], pads["output1"]["timer"],
                          pads["output1"]["pins"], pads["output1"]["devices"]),
                         ("output", "S1", "TIM4_CH2", {"signal": "B07"}, ["esc", "servo"]))
        self.assertEqual(pads["output7"]["timer"], "TIM8_CH2N")
        self.assertEqual([pads[f"uart{i}"]["serial"] for i in range(1, 7)], [0, 1, 2, 3, 4, 5])
        self.assertEqual(pads["gyro1"]["pins"], {"cs": "A04"})
        self.assertEqual((pads["i2c1"]["device"], pads["i2c1"]["detachable"]), ("spl06", True))
        self.assertEqual((pads["osd1"]["device"], pads["osd1"]["detachable"]), ("max7456", False))
        self.assertNotIn("devices", pads["uart6"])            # the MSP host socket
        self.assertEqual(pads["uart5"]["devices"], ["crsf", "msp_osd", "ublox"])
        self.assertEqual(pads["i2c1"]["devices"], ["qmc5883l", "spl06"])
        self.assertEqual((pads["batt1"]["kind"], pads["batt1"]["label"], pads["batt1"]["pins"], pads["batt1"]["devices"]),
                         ("power", "BAT", {"vbat": "C00", "curr": "C01"}, ["battery"]))
        self.assertIn("usb", pads["usb"]["flags"])
        # slots: gyro, i2c, uart only (outputs are not slots)
        self.assertEqual([s["id"] for s in wiring.slots_of(b["pads"])],
                         ["gyro1", "i2c1"] + [f"uart{i}" for i in range(1, 7)])

    def test_output_kind_rule(self):
        e, o, _ = resolve_f405(F405_RX + [{"device": "esc", "pad": "S1"}, {"device": "servo", "pad": "output2"}])
        self.assertEqual((o["esc"], o["servos"]), ("on", "2"))
        with self.assertRaises(wiring.WiringError) as cm:
            resolve_f405([{"device": "crsf", "port": "uart", "pad": "S3"}])
        self.assertIn("crsf.uart is a uart port, pad S3 (output3) is output", str(cm.exception))
        with self.assertRaises(wiring.WiringError) as cm:
            resolve_f405([{"device": "servo", "pad": "uart2"}])
        self.assertIn("servo.pwm is a servo port, pad uart2 is uart", str(cm.exception))
        with self.assertRaises(wiring.WiringError) as cm:
            resolve_f405([{"device": "esc", "pad": "S1"}, {"device": "servo", "pad": "output1"}])
        self.assertIn("pad S1 (output1) used by esc.signal (wiring[0]), servo.pwm (wiring[1])", str(cm.exception))

    def test_modelled(self):
        e, o, notes = resolve_f405([{"device": "esc", "pad": "S2"}, {"device": "crsf", "pad": "uart6"},
                                    {"device": "msp_osd", "pad": "uart4"}])
        self.assertEqual((o["esc"], o["crsf"], o["baro"], o["mag"], o["gps"], o["battery"]),
                         ("off", "off", "on", "off", "off", "off"))
        self.assertEqual(o["osd"], "uart4")
        for n in ("esc.signal on S2 (output2) (wired)", "crsf.uart on uart6 (wired)"):
            self.assertTrue(any(x.startswith(n + ": not modelled") for x in notes), n)
        self.assertFalse(any(x.startswith("spl06") or x.startswith("msp_osd") for x in notes))
        self.assertEqual({(x["device"], x["pad"]) for x in e if x["modelled"]},
                         {("icm42688p", "gyro1"), ("spl06", "i2c1"), ("max7456", "osd1"), ("msp_osd", "uart4")})
        # the OSD's config lines: the board's firmware family (inav), not RotorFlight's
        board = wiring.load_board(F405)
        self.assertEqual(board["firmware_family"], "inav")
        lines = wiring.device_config(e, board, wiring.device_catalog())
        self.assertEqual(lines[:4], ["# msp_osd on uart4 (device config)", "feature OSD",
                                     "serial 3 33554432 115200 115200 0 115200", "set osd_video_system = HDZERO"])
        self.assertIn("osd_layout 0 3 0 0 V", lines)             # the artificial horizon
        self.assertFalse(any("displayport_msp_serial" in x for x in lines))

    def test_options(self):
        w = F405_RX + [{"device": "esc", "pad": "S1", "actuator": "motor", "signal_timeout_ms": 50},
                       {"device": "servo", "pad": "output3", "actuator": "swash_a"},
                       {"device": "servo", "pad": "S4", "actuator": "swash_b", "reversed": True},
                       {"device": "servo", "pad": "S11", "actuator": "tail"},
                       {"device": "icm42688p", "pad": "gyro1", "gyro_noise_dps": 0.25}]
        _, o, _ = resolve_f405(w)
        self.assertEqual(list(o.items()), [
            ("imu", "on"), ("imu-noise", "0.25:0.002"), ("crsf", "uart1"), ("esc", "on"), ("baro", "on"),
            ("mag", "off"), ("gps", "off"), ("battery", "off"), ("esc-timeout-ms", "50"),
            ("servos", str(4 | 8 | 1024)),
            ("actuators", "servo1:servo2:swash_a:swash_b:servo5:servo6:servo7:servo8:servo9:servo10:tail:motor"),
            ("servo-rev", "8")])
        _, o, _ = resolve_f405(F405_RX + [{"device": "icm42688p", "detached": True}])
        self.assertEqual(o["imu"], "off")

    def test_sensors(self):
        gps = {"device": "ublox", "port": "uart", "pad": "uart3"}
        mag = {"device": "qmc5883l", "port": "i2c", "pad": "i2c1"}
        bat = {"device": "battery", "port": "power", "pad": "batt1"}
        e, o, notes = resolve_f405(F405_RX + [gps, mag, bat])
        self.assertEqual((o["baro"], o["mag"], o["gps"], o["battery"]), ("on", "on", "uart3", "on"))
        self.assertFalse(any(k.endswith("-props") for k in o))
        # props: the descriptor's order, only the given ones, ints as ints, floats as floats
        e, o, _ = resolve_f405(F405_RX + [
            dict(gps, seed=3, generation="m8", fix_delay_s=5), dict(mag, noise_mg=0, mount="CW90"),
            dict(bat, capacity_mah=2200, cells=3), {"device": "spl06", "pad": "i2c1", "noise_scale": 2}])
        self.assertEqual((o["gps-props"], o["mag-props"], o["battery-props"], o["baro-props"]),
                         ("generation=m8:fix_delay_s=5.0:seed=3", "mount=CW90:noise_mg=0.0", "cells=3:capacity_mah=2200.0",
                          "noise_scale=2.0"))
        # the GPS's config lines with the pad's serial index
        board = wiring.load_board(F405)
        lines = wiring.device_config(e, board, wiring.device_catalog())
        self.assertEqual(lines[:5], ["# ublox on uart3 (device config)", "feature GPS", "serial 2 +1",
                                     "set gps_provider = UBLOX", "set gps_ublox_nav_hz = 10"])
        _, o, _ = resolve_f405([dict(gps, pad="uart5")])
        self.assertEqual(o["gps"], "uart5")
        for w, msg in (([gps, dict(gps, pad="uart4")], "one ublox per session: uart3 (wiring[0]), uart4 (wiring[1])"),
                       ([bat, bat], "one battery per session"),
                       ([mag, mag], "bus i2c1: two qmc5883l (same I2C address)"),
                       ([dict(bat, pad="uart2")], "battery.power is a power port, pad uart2 is uart"),
                       ([dict(mag, mount="CW45")], "qmc5883l mount 'CW45': need one of CW0"),
                       ([dict(bat, start_soc=2)], "battery start_soc 2: need a number 0..1"),
                       (F405_RX + [dict(gps, pad="uart1")], "pad uart1 used by crsf.uart (wiring[0]), ublox.uart (wiring[1])")):
            with self.assertRaises(wiring.WiringError) as cm:
                resolve_f405(w)
            self.assertIn(msg, str(cm.exception))
        _, o, notes = resolve_f405([dict(gps, pad="uart6")])
        self.assertEqual(o["gps"], "off")
        self.assertTrue(any(x.startswith("ublox.uart on uart6 (wired): not modelled") for x in notes))

    def test_old_form_rejected(self):
        with self.assertRaises(wiring.WiringError) as cm:
            resolve_f405([{"device": "crsf", "slot": "uart1"}])
        self.assertIn("the old form ({device, slot}) is only for nexus-xr benches; on speedybee-f405-wing use "
                      "{device, port, pad}", str(cm.exception))
        with self.assertRaises(wiring.WiringError) as cm:
            resolve_f405([{"device": "crsf", "slot": "uart1"}, {"device": "servo", "pad": "S3"}])
        self.assertIn("mixes the old form", str(cm.exception))

    def test_no_wiring(self):
        board, ac, ap, entries, opts = session.assembly({"board": F405, "aircraft": "rs7"})
        self.assertEqual([(e["device"], e["modelled"]) for e in entries],
                         [("icm42688p", True), ("spl06", True), ("max7456", True), ("sdcard", False)])
        self.assertEqual((opts["imu"], opts["crsf"], opts["esc"], opts["servos"]), ("on", "off", "off", "0"))

    def test_session_run(self):
        rc, env, err, cli = run_session(load("f405-crsf.json"))
        self.assertEqual(rc, 0, err)
        self.assertEqual((env["MACHINE"], env["MSP_UART"], env["CONFIG"]), (F405, "uart6", "cli"))
        opts = env["SESSION_OPTS"].split(",")
        self.assertIn("crsf=uart1", opts)
        self.assertIn("beeper=C15:inv", opts)
        self.assertFalse(any(x.startswith(("fdm=", "aircraft-file=")) for x in opts))
        self.assertIn("baro=on", opts)
        self.assertIn("aircraft 'rs7': no FDM for it on speedybee-f405-wing, the IMU sees a board at rest", err)
        self.assertEqual(cli, b"")              # the base script has no commands (comments only)
        # environment: the SPL06's air
        rc, env, err, _ = run_session(dict(load("f405-boot.json"), environment={"sea_level_pa": 100800}))
        self.assertEqual(rc, 0, err)
        self.assertNotIn("has no modelled baro", err)
        self.assertIn("baro-env=100800.0:0.0:22.0", env["SESSION_OPTS"].split(","))
        # nexus-xr keeps its blob and its FDM
        rc, env, err, cli = run_session(load("rs7-pads.json"))
        self.assertEqual((rc, env["MACHINE"], env["CONFIG"], env["CONFIG_CLI"], cli), (0, "nexus-xr", "unified", "", b""))
        self.assertIn("fdm=on", env["SESSION_OPTS"].split(","))

    def test_config_cli(self):
        d = tempfile.mkdtemp()
        base, prof, acli = (os.path.join(d, n) for n in ("base.cli", "p.cli", "ac.cli"))
        for p, data in ((base, b"# base\r\nfeature -GPS\r\n\r\n  save  \n"), (prof, b"set a = 1\nEXIT\n"),
                        (acli, b"set platform_type = AIRPLANE\n")):
            with open(p, "wb") as f:
                f.write(data)
        text, notes, errs, prov = session.config_cli(base, acli, [prof], "set b = 2\n#c\n",
                                                     ["# osd (device config)", "set c = 3"])
        self.assertEqual(text, b"feature -GPS\nset platform_type = AIRPLANE\nset a = 1\nset b = 2\nset c = 3\n")
        # each line's layer, source and ref index (config_json)
        self.assertEqual([(x["layer"], x["source"], x["idx"]) for x in prov["lines"]],
                         [("board", f"{base}:2", 1), ("aircraft", f"{acli}:1", 1), ("profile", f"{prof}:1", 1),
                          ("profile", "profile_text:1", 2), ("device", "device osd", 1)])
        self.assertEqual(notes, [f"config_cli: {base} line 4: 'save' dropped (the core saves at the end)",
                                 f"config_cli: {prof} line 2: 'exit' dropped (the core saves at the end)"])
        self.assertEqual(errs, [])
        text, notes, errs, _ = session.config_cli("", "", [], "defaults\n  Reboot now\nset x = 1\nblx\nbl\nmsc\n", [])
        self.assertEqual([e.split(": ", 2)[1:] for e in errs],
                         [["profile_text line 1", "'defaults' would reset or wipe the board mid-script, not allowed"],
                          ["profile_text line 2", "'reboot' would reset or wipe the board mid-script, not allowed"],
                          ["profile_text line 5", "'bl' would reset or wipe the board mid-script, not allowed"],
                          ["profile_text line 6", "'msc' would reset or wipe the board mid-script, not allowed"]])
        text, notes, errs, _ = session.config_cli(os.path.join(d, "nope.cli"), "", [], "", [])
        self.assertEqual(errs, [f"board config {os.path.join(d, 'nope.cli')}: No such file or directory"])
        # end to end: a forbidden line rejects the bench, a base script "" = none
        rc, _, err, _ = run_session(dict(load("f405-boot.json"), profile_text="save\ndfu\n"))
        self.assertNotEqual(rc, 0)
        self.assertIn("config_cli: profile_text line 1: 'save' dropped", err)
        self.assertIn("config_cli: profile_text line 2: 'dfu' would reset or wipe the board", err)
        rc, env, err, cli = run_session(dict(load("f405-boot.json"), board_config=""))
        self.assertEqual((rc, env["BOARD_CONFIG"], env["CONFIG_CLI"], cli), (0, "", "", b""))

    def test_aircraft_firmware_cli(self):
        a = rs7_aircraft()
        a["firmware_cli"] = 5
        p = tmp_json(a)
        try:
            with self.assertRaises(wiring.WiringError) as cm:
                wiring.load_aircraft(p)
            self.assertIn("firmware_cli 5: need a path", str(cm.exception))
        finally:
            os.unlink(p)

    def test_manifest_both_boards(self):
        m = manifest.manifest()
        self.assertEqual([(b["id"], b["machine"], b["config"], b["msp_uart"]) for b in m["boards"]],
                         [("nexus-xr", "nexus-xr", "unified", "uart1"), (F405, F405, "cli", "uart6")])
        self.assertEqual(list(m["boards"][1])[:9], ["id", "name", "mcu", "machine", "config", "msp_uart", "firmware_family", "slots", "pads"])
        devs = {d["id"]: d for d in m["devices"]}
        self.assertEqual({k: (devs[k]["category"], devs[k]["kind"], devs[k]["implemented"])
                          for k in ("spl06", "max7456", "sdcard", "qmc5883l", "ublox", "battery", "compass")},
                         {"spl06": ("baro", "i2c", True), "max7456": ("osd", "spi", True),
                          "sdcard": ("storage", "spi", False), "qmc5883l": ("compass", "i2c", True),
                          "ublox": ("gps", "uart", True), "battery": ("battery", "power", True),
                          "compass": ("compass", "i2c", False)})
        self.assertTrue(devs["dps310"]["implemented"] and devs["servo"]["implemented"])
        # the nexus-xr's board config applies to it alone
        self.assertEqual(manifest.manifest(CFG, "nexus-xr")["boards"][1], m["boards"][1])


if __name__ == "__main__":
    unittest.main()
