#!/usr/bin/env python3
"""Stand-in for the FlightBench core (docs/protocol.md) so the app can be developed and demoed
before the QEMU core speaks the protocol: sends `hello`, then `state` at 100 Hz of a helicopter
that spools up, lifts off and flies a lazy circle; echoes `rc` and `time` commands into `log`.
Publishes a small `signals` list on connect and streams `samples` for the ids a client `subscribe`s to.
OSD: a `clear` frame on connect, then every 100 ms only the cells that changed of a RotorFlight-like
18x50 layout (mode top-left, battery top-right, head speed bottom-left, crosshair, timer bottom-right,
a blinking "ARM" while disarmed; attr 0x40 = blink); `subscribe` with `"osd": false` stops it. Every
message carries `"font":"rotorflight"` and `frame` = a DRAW count that skips the DRAWs that changed nothing
(12 per second, as the core since `inav-osd`), so the numbers jump.

--plane --osd: the trainer with an `msp_osd` on uart4 (the core's trainer-osd bench): `hello.osd` MSP DisplayPort
50x18, and iNav-like `osd` messages (the layout of core/qemu/evidence/inav-osd: mode, sats, heading, RSSI, speed,
altitude, battery, fly time, and on page 1 (attr 1) the horizon 0x14C..0x154 following the pitch, its sidebars 0x131
and the crosshairs 0x13A 0x166 0x13B): no `font` until the firmware "answers" (t = 3 s), then `"font":"inav"`; a full
frame every 1 s (iNav's fullframe interval), deltas of the changed cells in between; `frame` = DRAWs at ~47/s, so it
jumps by several per message.

--plane: the trainer on the speedybee-f405-wing instead (docs/protocol.md "Plane FDM"): a first start
compressed to ~13 virtual s (fc.msp false while "booting", the `config` events applying -> calibrating with the
six `pose` events of the ACC calibration -> applied), then the model held 10 deg nose up on the pad (`pose`
"launch attitude") until a `launch` command: `launch` start/release events, a climb at 16 deg to 30 m, level
flight; `state.plane` all along (airspeed, alpha/beta, thrust, surfaces, launch phase, hold, scenario). fc.armed
follows ch5, fc.mode ch6 (ANGLE) + ch7 (NAV LAUNCH). `reset` puts it back on the pad (config not repeated).

Firmware views (docs/protocol.md `modes` / `diff` / `function` / `osd`, the shapes the real core sends —
core/qemu/evidence/modes):
- `modes` only for a client that subscribed (`subscribe {"modes":true}`, answered at once + a log line "modes:
  subscribed") or asked ({"type":"modes"}); the plane: again to subscribers after `config: applied`. Boxes (permanent
  id, index, name), `variant` (RTFL / INAV), ranges with slot / channel / rc_channel / rx_channel / logic. Plain:
  RS7-like ARM on AUX1 1700..2100 (as models/rs7/qemu-profile.txt) plus FAKE ranges to show a 3-position switch (AUX2:
  ANGLE 1300..1700, HORIZON 1700..2100) and RESCUE on AUX3; plane: the trainer's models/trainer/inav.cli (ARM / ANGLE
  / NAV LAUNCH on AUX1..3, iNav permanent ids 0 / 1 / 36). `state.fc.boxes` = the active boxes as permanent ids (from
  the channels and these ranges; ARM follows fc.armed), `fc.mode` = their names except ARM.
- `diff`: plain: replayed after `hello` (`replay`, `source` first_start); plane: after the first start's calibration
  (`source` first_start). {"type":"diff"} → a fresh one (`source` request) or, while armed, the last text with
  `stale`, `error` "armed"; {"type":"diff","refresh":false} → the last one (`error` "none" without one). --stale-diff
  marks them stale.
- `function` progress on {"type":"function","name":...}: calibrate_acc (6 steps), calibrate_gyro (hold still),
  launch (the plane: starts the launch as `launch` does); an unknown name → phase failed. The plane's first start
  runs calibrate_acc as the real core does: `config calibrating {"function":"calibrate_acc"}`, `function` steps with
  the poses, `applied {"functions":["calibrate_acc"],"acc_calibrated":true}`.
- `hello.osd` (and every state's `osd`): plain {"kind":"msp_displayport","cols":50,"rows":18,"video":true}, plane
  {"kind":"max7456","cols":30,"rows":16,"video":false}. `hello.firmware` is the file name until the firmware
  "answers"; then a log line "firmware says: …" (the plane at t = 3 s, plain at once).
- --manifest FILE: send FILE as the `manifest` after `hello`, with a `functions` catalog added to its aircraft
  (rs7: calibrate_acc / calibrate_gyro; trainer: calibrate_acc auto_first_start, calibrate_gyro, launch). The app
  caches a live manifest into user://manifest.json (not in a self-test run): run the app with an empty APPDATA.

Bench state (the core branch `bench-state`, docs/protocol.md; the app is built against it, the real core is not there yet):
- the firmware's CLI mode: a `cli` command with text (not `exit`) opens it: event `cli {"open":true,"by":"client"}` once,
  `state.fc.cli` true, `fc.msp` false; `{"type":"cli","exit":true}` (or the text `exit`) leaves it: event
  `cli {"open":false,"by":"client"}`, a `reboot` event, `fc.cli` false. The CLI lines are echoed as `log` as before.
- `{"type":"config","reset":"first_start"}`: `config applying {"lines":13}` at once, `applied` 1 s later (plane: the same
  two events, not the whole first start again).
- --bench-state: after hello, `config restored {"source":"bench","replay":true}`; --delta N: after hello, `config applying
  {"delta":true,"lines":N}`, `config {"removed":["set fake_removed = 1"]}`, `applied` 1 s later (a restart after Edit bench).

Config layers (docs/protocol.md, authoritative; real core shapes — board|aircraft|profile|device|bench):
- `{"type":"config","apply":true[,"full":true]}`: refused while armed (`{"type":"config","error":"armed"}`), else event
  `cli {"open":true,"by":"core"}`, `config applying {"lines":N[,"full":true]}`, 1 s later `config applied`, `cli
  {"open":false}` and a `reboot` (the firmware does not answer MSP for ~2 s after every reboot: `fc.msp` false).
- `{"type":"config","snapshot":"bench","path":P}`: refused while armed, else the fake `diff all` as CLI lines (comments,
  `batch`, blank lines dropped) written to P through a temp file + rename, answer `{"type":"config","snapshot":"written",
  "path":P,"lines":N}`, then a `reboot` (the real core's diff all leaves the CLI with exit).
- `{"type":"config","export":true}`: `{"type":"config","export":{"bench","root","assembly":<config_json-shaped>,"script",
  "applied","diff":{"text","stale","source","at"}}}` — the fake script by layer (plain: RS7_SCRIPT; plane: models/trainer/inav.cli + the GPS lines).
- `{"type":"diff","against":"script"}`: `{"type":"diff","against":"script","source","at","stale","sent":[{setting,value,
  text,layer,source}…],"current":[{setting,value,text}…],"changed":[{setting,script,firmware,layer,source}]
  (script/firmware are FULL LINE TEXTS),"only_firmware":[{setting,firmware}…],"only_script":[{setting,script,layer,
  source}…]}` computed from that script and the fake diff (refused while armed: `error` "armed").
- `{"type":"cli","exit":true}` also reboots with the ~2 s MSP gap (the boot hint); outside the CLI it is an `error`.
- --disarmed: the plain core never arms (it arms at t > 8 s otherwise), so apply / snapshot / against-script are not refused.
- `--session-check SESSION ROOT` (no server): prints a session-check report the way the real core is specified
  (docs/protocol.md "Launching the core" / "Validate first") — `config:` device lines (unchanged, disabled ones
  included), `launch:`, `manifest: {}`, `config_cli: N lines` + `config_cli| <line>` (enabled lines, unannotated), then
  a CLI board's layers: `config_src: [<layer>:<source>] <text>` per line sent, `config_disabled: [<layer>:<source>]
  <text> (disabled_lines '<entry>')`, `config_conflict: <setting>: [<layer>:<source>] <text> -> [<layer>:<source>]
  <text> (the last wins)`, then one line `config_json: {kind,layers:[{layer,file,path}],lines:[{text,layer,source,ref}],
  disabled:[{text,layer,source,ref,by}],conflicts:[{setting,lines,wins}]}` (every board; a blob board's `lines` are its
  device lines only, no conflicts), `defaults: none` on a CLI board — from the session's board script (board.json
  `defaults_cli` / `board_config`), the aircraft's `firmware_cli`, `profile` (layer `profile`), `profile_text`, the
  wired devices (a small table: `ublox`, `msp_osd`), `bench_cli` and `disabled_lines` (exact text or `layer:n`,
  1-based). A missing bench_cli file or an unknown board: `session: …`, exit 2.

  fake_core.py [--port 31250] [--plane [--osd]] [--manifest FILE] [--stale-diff] [--bench-state] [--delta N] [--disarmed]
  fake_core.py --session-check SESSION ROOT
"""
import json, math, os, random, re, socket, sys, threading, time

# extra signals beyond `state`, the way the real core will publish raw inputs/outputs
SIGNALS = [
    {"id": "fdm.alt_m", "unit": "m"}, {"id": "fdm.climb_mps", "unit": "m/s"},
    {"id": "in.gyro_raw_dps.x", "unit": "deg/s"}, {"id": "in.gyro_raw_dps.y", "unit": "deg/s"}, {"id": "in.gyro_raw_dps.z", "unit": "deg/s"},
    {"id": "out.servo1_us", "unit": "us"}, {"id": "out.servo2_us", "unit": "us"}, {"id": "out.servo3_us", "unit": "us"}, {"id": "out.servo4_us", "unit": "us"},
    {"id": "out.motor1_dshot", "unit": ""}, {"id": "fc.gov.target_rpm", "unit": "rpm"},
]

PORT = int(sys.argv[sys.argv.index("--port") + 1]) if "--port" in sys.argv else 31250
PLANE = "--plane" in sys.argv
PLANE_OSD = PLANE and "--osd" in sys.argv
MANIFEST = sys.argv[sys.argv.index("--manifest") + 1] if "--manifest" in sys.argv else None
STALE_DIFF = "--stale-diff" in sys.argv
BENCH_STATE = "--bench-state" in sys.argv
DELTA = int(sys.argv[sys.argv.index("--delta") + 1]) if "--delta" in sys.argv else 0
DISARMED = "--disarmed" in sys.argv    # the plain core never arms (it arms at t > 8 s otherwise): the config operations are not refused

# ---- firmware views (modes / diff / functions / osd) ----
RS7_BOXES = [{"id": 0, "index": 0, "name": "ARM"}, {"id": 1, "index": 1, "name": "ANGLE"}, {"id": 2, "index": 2, "name": "HORIZON"},
             {"id": 13, "index": 3, "name": "BEEPER"}, {"id": 27, "index": 4, "name": "FAILSAFE"}, {"id": 53, "index": 5, "name": "RESCUE"},
             {"id": 55, "index": 6, "name": "GOVERNOR"}]
RS7_RANGES = [{"slot": 0, "channel": 0, "rc_channel": 5, "rx_channel": 4, "box": 0, "start_us": 1700, "end_us": 2100, "logic": "or"},   # models/rs7/qemu-profile.txt: aux 0 0 0 1700 2100
              {"slot": 1, "channel": 1, "rc_channel": 6, "rx_channel": 5, "box": 1, "start_us": 1300, "end_us": 1700, "logic": "or"},   # FAKE: a 3-position switch for the demo
              {"slot": 2, "channel": 1, "rc_channel": 6, "rx_channel": 5, "box": 2, "start_us": 1700, "end_us": 2100, "logic": "or"},
              {"slot": 3, "channel": 2, "rc_channel": 7, "rx_channel": 6, "box": 53, "start_us": 1700, "end_us": 2100, "logic": "or"}]  # FAKE
INAV_BOXES = [{"id": 0, "index": 0, "name": "ARM"}, {"id": 1, "index": 1, "name": "ANGLE"}, {"id": 2, "index": 2, "name": "HORIZON"},
              {"id": 12, "index": 3, "name": "MANUAL"}, {"id": 10, "index": 4, "name": "NAV RTH"}, {"id": 27, "index": 5, "name": "FAILSAFE"},
              {"id": 36, "index": 6, "name": "NAV LAUNCH"}]
INAV_RANGES = [{"slot": 0, "channel": 0, "rc_channel": 4, "rx_channel": 4, "box": 0, "start_us": 1700, "end_us": 2100},     # models/trainer/inav.cli: aux 0 0 0 1700 2100
               {"slot": 1, "channel": 1, "rc_channel": 5, "rx_channel": 5, "box": 1, "start_us": 1700, "end_us": 2100},     # aux 1 1 1 1700 2100
               {"slot": 2, "channel": 2, "rc_channel": 6, "rx_channel": 6, "box": 36, "start_us": 1700, "end_us": 2100}]    # aux 2 36 2 1700 2100
def modes_msg(t, plane):
    return {"type": "modes", "t": round(t, 3), "variant": "INAV" if plane else "RTFL", "boxes": INAV_BOXES if plane else RS7_BOXES,
            "ranges": INAV_RANGES if plane else RS7_RANGES, "rx_map": [0, 1, 3, 2] if plane else [0, 1, 3, 2, 5, 4, 6, 7]}

def diff_answer(m, t, text, armed, source):
    """The core's answer to {"type":"diff"}: refresh:false = the cache; armed = refused with the last text."""
    if m.get("refresh") is False:
        if text is None:
            return {"type": "diff", "t": round(t, 3), "text": None, "stale": True, "source": None, "at": None, "error": "none"}
        return {"type": "diff", "t": round(t, 3), "text": text, "stale": STALE_DIFF, "source": source, "at": 0.0}
    if armed:
        return {"type": "diff", "t": round(t, 3), "text": text, "stale": True, "source": source if text else None, "at": 0.0 if text else None, "error": "armed"}
    return {"type": "diff", "t": round(t, 3), "text": text or "", "stale": STALE_DIFF, "source": "request", "at": round(t, 3)}
RS7_DIFF = """# diff all
# version
# Rotorflight / STM32F7X2 (S7X2) 4.6.0 (fake core)

# start the command batch
batch start

board_name NEXUS_XR
manufacturer_id RADM

# feature
feature -RX_PARALLEL_PWM
feature RX_SERIAL

# aux
aux 0 0 0 1700 2100 0 0

# master
set gov_mode = ELECTRIC
set tail_center_trim = 137
set acc_calibration = 0,0,0,-30671

batch end
"""
# the plain core's script (RS7-like: the profile's lines as the bench would send them), by layer
RS7_SCRIPT = [("board", "boards/nexus-xr/NEXUS_XR.config:3", "board_name NEXUS_XR"),
              ("board", "boards/nexus-xr/NEXUS_XR.config:4", "manufacturer_id RADM"),
              ("aircraft", "models/rs7/qemu-profile.txt:12", "feature RX_SERIAL"),
              ("aircraft", "models/rs7/qemu-profile.txt:20", "aux 0 0 0 1700 2100 0 0"),
              ("aircraft", "models/rs7/qemu-profile.txt:31", "set gov_mode = ELECTRIC"),
              ("aircraft", "models/rs7/qemu-profile.txt:32", "set tail_center_trim = 0"),
              ("aircraft", "models/rs7/qemu-profile.txt:40", "set rescue_mode = ON"),
              ("bench", "bench profile_text:1", "set tail_center_trim = 60")]
DEVICE_LINES = {"ublox": ["feature GPS", "serial {serial} 2 115200 115200 0 115200", "set gps_provider = UBLOX", "set gps_ublox_nav_hz = 10"],
                "msp_osd": ["serial {serial} 1 115200 57600 0 115200", "set displayport_msp_serial = {serial}"]}

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

def plane_script():
    """The trainer's script by layer: models/trainer/inav.cli (aircraft) + the GPS module on uart3 (device)."""
    out = []
    try:
        with open(os.path.join(REPO, "models", "trainer", "inav.cli"), encoding="utf-8") as fh:
            for i, raw in enumerate(fh.read().splitlines(), 1):
                t = raw.strip()
                if t and not t.startswith("#") and t.split()[0].lower() not in ("save", "exit"):
                    out.append(("aircraft", f"models/trainer/inav.cli:{i}", t))
    except OSError:
        out.append(("aircraft", "models/trainer/inav.cli", "set platform_type = AIRPLANE"))
    for t in DEVICE_LINES["ublox"]:
        out.append(("device", "device ublox on uart3", t.format(serial=2)))
    return out

# the trainer's `diff all` as the fake core reports it: today's models/trainer/inav.cli (2026-09-30: the configurator's
# "Airplane with a Tail", PWM_OUTPUT_ENABLE, NAV LAUNCH on ch7 — not the old `feature FW_LAUNCH` config) + the GPS's lines,
# with what a user then did in the configurator: nav_fw_bank_angle 35 -> 40, nav_fw_launch_climb_angle added, nav_wp_radius
# back at its default (gone from the diff)
def build_inav_diff():
    body = []
    for _, _, t in plane_script():
        if t == "set nav_fw_bank_angle = 35":
            t = "set nav_fw_bank_angle = 40"
        if t == "set nav_wp_radius = 800":
            continue
        body.append(t)
    body.append("set nav_fw_launch_climb_angle = 20")
    head = "# diff all\n# INAV/SPEEDYBEEF405WING 9.1.0 (fake core)\n\n# start the command batch\nbatch start\n\n"
    return head + "\n".join(body) + "\n\nbatch end\n"
INAV_DIFF = build_inav_diff()

def setting_key(line):
    """(key, value) of a CLI line for comparing a diff with a script; None for comments / batch / resets / profile switches."""
    t = line.strip()
    if not t or t.startswith("#"):
        return None
    w = t.split()
    cmd = w[0].lower()
    if cmd in ("batch", "save", "exit", "control_profile", "battery_profile", "mixer_profile", "profile", "rateprofile", "defaults") or (len(w) > 1 and w[1] == "reset"):
        return None
    if cmd == "set" and "=" in t:
        name, val = t[4:].split("=", 1)
        return ("set " + name.strip().lower(), val.strip())
    if cmd == "feature" and len(w) > 1:
        return ("feature " + w[1].lstrip("-").upper(), "OFF" if w[1].startswith("-") else "ON")
    if len(w) > 2 and cmd in ("aux", "serial", "servo", "smix", "mmix", "resource"):
        return (" ".join(w[:2]), " ".join(" ".join(w[2:]).split()))
    return (cmd, " ".join(w[1:]))

def diff_lines(text):
    """The CLI lines of a `diff all` (comments, batch, blank dropped)."""
    return [l.strip() for l in (text or "").splitlines() if l.strip() and not l.strip().startswith("#") and l.split()[0].lower() not in ("batch", "save")]

def changes_against(script, diff):
    """{"type":"diff","against":"script"}'s answer (docs/protocol.md): `script` / `firmware` are FULL LINE TEXTS."""
    s = {}
    for layer, src, t in script:
        k = setting_key(t)
        if k:
            s[k[0]] = (t, k[1], layer, src)
    d = {}
    for t in (diff or "").splitlines():
        k = setting_key(t)
        if k:
            d[k[0]] = (t.strip(), k[1])
    changed = [{"setting": k, "script": s[k][0], "firmware": d[k][0], "layer": s[k][2], "source": s[k][3]} for k in s if k in d and s[k][1] != d[k][1]]
    only_fw = [{"setting": k, "firmware": d[k][0]} for k in d if k not in s]
    only_script = [{"setting": k, "script": s[k][0], "layer": s[k][2], "source": s[k][3]} for k in s if k not in d]
    return changed, only_fw, only_script

def assembly_of(script):
    """A `config_json`-shaped `{kind,layers,lines,disabled,conflicts}` from a `(layer,source,text)` script list (the
    fake plain / plane scripts have no `disabled_lines` of their own: `disabled` stays empty)."""
    counts = {}
    lines = []
    for layer, src, text in script:
        counts[layer] = counts.get(layer, 0) + 1
        lines.append({"text": text, "layer": layer, "source": src, "ref": f"{layer}:{counts[layer]}"})
    by = {}
    for l in lines:
        k = setting_key(l["text"])
        if k:
            by.setdefault(k[0], []).append(l)
    conflicts = [{"setting": k, "lines": v, "wins": v[-1]["ref"]} for k, v in by.items() if len({x["layer"] for x in v}) > 1]
    return {"kind": "cli", "layers": [], "lines": lines, "disabled": [], "conflicts": conflicts}

def export_answer(t, script, diff, applied=True):
    """{"type":"config","export":{bench,root,assembly,script,applied,diff:{text,stale,source,at}}} (docs/protocol.md)."""
    return {"type": "config", "t": round(t, 3), "export": {
        "bench": "fake-bench.json", "root": REPO, "assembly": assembly_of(script),
        "script": "\n".join(x for _, _, x in script) + "\n", "applied": applied,
        "diff": {"text": diff, "stale": STALE_DIFF, "source": "first_start" if diff else None, "at": round(t, 3) if diff else None},
    }}

def write_snapshot(path, diff):
    """The core's snapshot: the diff's CLI lines written atomically to `path`; returns the line count."""
    lines = [l for l in diff_lines(diff)]
    body = "# the firmware's diff all as bench lines (fake core snapshot)\n" + "\n".join(lines) + "\n"
    tmp = path + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    os.replace(tmp, path)
    return len(lines)

def config_layers_command(send, m, t_now, armed, script, diff, reboot):
    """apply / snapshot / export (the `config-layers` contract). Returns True when handled."""
    t = t_now()
    if m.get("export") is True:
        send(export_answer(t, script, diff))
        return True
    if m.get("apply") is True or m.get("snapshot") is not None:
        if armed():
            send({"type": "config", "t": round(t, 3), "error": "armed", **({"snapshot": "refused"} if m.get("snapshot") is not None else {})})
            return True
        if m.get("snapshot") is not None:
            path = str(m.get("path") or "")
            if m.get("snapshot") != "bench" or not path:
                send({"type": "config", "t": round(t, 3), "snapshot": "refused", "error": "snapshot needs \"bench\" and a path"})
                return True
            try:
                n = write_snapshot(path, diff)
            except OSError as e:
                send({"type": "config", "t": round(t, 3), "snapshot": "refused", "error": str(e)})
                return True
            send({"type": "log", "src": "core", "t": round(t, 3), "text": f"config: snapshot: diff all ({n} lines) written to {path}; exit"})
            send({"type": "config", "t": round(t, 3), "snapshot": "written", "path": path, "lines": n})
            reboot()
            return True
        full = m.get("full") is True
        n = len(script) if full else max(1, len(changes_against(script, diff)[2]) + len(changes_against(script, diff)[0]))
        def body():
            send({"type": "event", "t": round(t_now(), 3), "name": "cli", "data": {"open": True, "by": "core", "via": "msp"}})
            send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": dict({"state": "applying", "lines": n}, **({"full": True} if full else {}))})
            time.sleep(1.0)
            send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": dict({"state": "applied", "lines": n}, **({"full": True} if full else {}))})
            send({"type": "event", "t": round(t_now(), 3), "name": "cli", "data": {"open": False, "by": "core"}})
            reboot()
        threading.Thread(target=body, daemon=True).start()
        return True
    return False

def diff_against_script(t, script, diff, armed):
    if armed:
        return {"type": "diff", "t": round(t, 3), "against": "script", "error": "armed", "text": None, "stale": True}
    changed, only_fw, only_script = changes_against(script, diff)
    sent = []
    for layer, src, x in script:
        k = setting_key(x)
        sent.append({"setting": k[0] if k else None, "value": k[1] if k else None, "text": x, "layer": layer, "source": src})
    current = []
    for l in (diff or "").splitlines():
        s = l.strip()
        if not s or s.startswith("#") or s.split()[0].lower() in ("batch", "save"):
            continue
        k = setting_key(s)
        current.append({"setting": k[0] if k else None, "value": k[1] if k else None, "text": s})
    return {"type": "diff", "t": round(t, 3), "against": "script", "source": "request", "at": round(t, 3), "stale": False,
            "sent": sent, "current": current, "changed": changed, "only_firmware": only_fw, "only_script": only_script}

# ---- --session-check: a report of the `config-layers` core ----
def session_check(session_path, root):
    def fail(msg):
        sys.stderr.write(f"session: {msg}\n")
        sys.exit(2)
    try:
        with open(session_path, encoding="utf-8") as fh:
            s = json.load(fh)
    except (OSError, ValueError) as e:
        fail(f"{session_path}: {e}")
    def res(p):
        p = str(p).replace("\\", "/")
        if p.startswith("~"):
            p = os.path.expanduser(p)
        return p if os.path.isabs(p) or (len(p) > 1 and p[1] == ":") else os.path.join(root, p)
    board = s.get("board", "nexus-xr")
    bj_path = os.path.join(root, "boards", board, "board.json")
    if not os.path.exists(bj_path):
        fail(f"unknown board '{board}'")
    with open(bj_path, encoding="utf-8") as fh:
        bj = json.load(fh)
    cli_board = bj.get("config") == "cli"
    lines = []                                     # [layer, source, text]
    layer_files = []                               # [(layer, shown, abs)] — config_json's `layers`
    def add_file(layer, path, shown):
        abs_path = res(path)
        layer_files.append((layer, shown, abs_path))
        try:
            with open(abs_path, encoding="utf-8") as fh:
                for i, raw in enumerate(fh.read().splitlines(), 1):
                    t = raw.strip()
                    if t and not t.startswith("#") and t.split()[0].lower() not in ("save", "exit"):
                        lines.append([layer, f"{shown}:{i}", t])
        except OSError as e:
            fail(f"{layer} file {path}: {e.strerror}")
    bc = s.get("board_config")
    if bc is None:
        if cli_board and bj.get("defaults_cli"):
            bc = f"boards/{board}/{bj['defaults_cli']}"
        else:
            cfgs = sorted(f for f in os.listdir(os.path.join(root, "boards", board)) if f.endswith(".config"))
            bc = f"boards/{board}/{cfgs[0]}" if cfgs else ""
    if bc:
        add_file("board", bc, bc)
    air = s.get("aircraft", "")
    try:
        with open(res(air), encoding="utf-8") as fh:
            aj = json.load(fh)
    except (OSError, ValueError):
        aj = {}
    if cli_board and aj.get("firmware_cli"):
        add_file("aircraft", aj["firmware_cli"], aj["firmware_cli"])
    prof = s.get("profile")
    for p in ([prof] if isinstance(prof, str) else prof or []):
        add_file("profile", p, p)
    for i, raw in enumerate(str(s.get("profile_text", "")).splitlines(), 1):
        t = raw.strip()
        if t and not t.startswith("#") and t.split()[0].lower() not in ("save", "exit"):
            lines.append(["profile", f"profile_text:{i}", t])
    config_lines = []                              # the report's older `config:` lines — device lines only, unchanged
    serial_of = {p["id"]: p.get("serial") for p in bj.get("pads", []) if isinstance(p, dict)}
    for w in s.get("wiring", []):
        dev, pad = w.get("device"), w.get("pad", w.get("slot", ""))
        if dev in DEVICE_LINES and pad:
            src = f"device {dev} on {pad}"
            config_lines.append(f"# {dev} on {pad} (device config)")
            ser = serial_of.get(pad)
            ser = ser if ser is not None else (int(pad[4:]) - 1 if pad.startswith("uart") and pad[4:].isdigit() else 0)
            for t in DEVICE_LINES[dev]:
                t = t.format(serial=ser)
                config_lines.append(t)
                lines.append(["device", src, t])
    if s.get("bench_cli"):
        if not os.path.exists(res(s["bench_cli"])):
            fail(f"bench_cli {s['bench_cli']}: no such file")
        add_file("bench", s["bench_cli"], s["bench_cli"].replace("\\", "/"))
    # 1-based per-layer index (docs/protocol.md `ref`: "<layer>:<n>", counted across a layer's enabled AND disabled lines)
    counts = {}
    for l in lines:
        counts[l[0]] = counts.get(l[0], 0) + 1
        l.append(counts[l[0]])                     # l[3] = 1-based layer index
    dis = set(str(x).strip() for x in s.get("disabled_lines", []) or [])
    def disabled_entry(l):
        if l[2] in dis:
            return l[2]
        ref = f"{l[0]}:{l[3]}"
        return ref if ref in dis else None
    for l in lines:
        l.append(disabled_entry(l))                # l[4] = the disabled_lines entry matched, else None
    enabled = [l for l in lines if l[4] is None]
    disabled = [l for l in lines if l[4] is not None]
    def ref(l):
        return f"{l[0]}:{l[3]}"
    def line_obj(l):
        return {"text": l[2], "layer": l[0], "source": l[1], "ref": ref(l)}
    # conflicts: the same setting in >= 2 layers among the enabled lines; the later layer wins
    by_setting = {}
    for l in enabled:
        k = setting_key(l[2])
        if k:
            by_setting.setdefault(k[0], []).append(l)
    conflicts = [{"setting": k, "lines": [line_obj(x) for x in ls], "wins": ref(ls[-1])}
                 for k, ls in by_setting.items() if len({x[0] for x in ls}) > 1]
    out = sys.stdout
    # the older `config:` lines (unchanged by config-layers): the wired devices' own lines, disabled ones included
    for x in config_lines:
        out.write(f"config: {x}\n")
    out.write(f"root: {root}\nboard: {board}\nboard_machine: {bj.get('machine', board)}\n")
    t = s.get("time", {})
    ports = s.get("ports", {})
    out.write(f"launch: icount_shift={t.get('icount_shift', 2)} realtime={'on' if t.get('realtime') else 'off'} control={ports.get('control', 31250)} msp={ports.get('msp', 31235)} machine={bj.get('machine', board)}\n")
    out.write("manifest: {\"type\":\"manifest\"}\n")
    if cli_board:
        out.write(f"config_cli: {len(enabled)} lines\n")
        for l in enabled:
            out.write(f"config_cli| {l[2]}\n")
    else:
        out.write("config_cli: none\n")
    if cli_board:
        for l in enabled:
            out.write(f"config_src: [{l[0]}:{l[1]}] {l[2]}\n")
        for l in disabled:
            out.write(f"config_disabled: [{l[0]}:{l[1]}] {l[2]} (disabled_lines '{l[4]}')\n")
        for c in conflicts:
            chain = " -> ".join(f"[{x['layer']}:{x['source']}] {x['text']}" for x in c["lines"])
            out.write(f"config_conflict: {c['setting']}: {chain} (the last wins)\n")
    out.write("config_json: " + json.dumps({
        "kind": "cli" if cli_board else "blob",
        "layers": [{"layer": ly, "file": shown, "path": abspath} for ly, shown, abspath in layer_files],
        "lines": [line_obj(l) for l in enabled] if cli_board else [line_obj(l) for l in enabled if l[0] == "device"],
        "disabled": [dict(line_obj(l), by=l[4]) for l in disabled] if cli_board else [],
        "conflicts": conflicts if cli_board else [],
    }) + "\n")
    if cli_board:
        out.write("defaults: none\n")
    else:
        blob = "\n".join(l[2] for l in enabled) + "\n"
        out.write(f"defaults: {len(blob)} bytes (max 6136)\n{blob}")
    sys.exit(0)
FUNCTIONS = {
    "rs7": [{"name": "calibrate_acc", "label": "Calibrate accelerometer (level)", "auto_first_start": False, "params": {}},
            {"name": "calibrate_gyro", "label": "Calibrate gyro (hold still)", "auto_first_start": False, "params": {"hold_s": 3}}],
    "trainer": [{"name": "calibrate_acc", "label": "Calibrate accelerometer (6 positions)", "auto_first_start": True, "params": {}},
                {"name": "calibrate_gyro", "label": "Calibrate gyro (hold still)", "auto_first_start": False, "params": {"hold_s": 3}},
                {"name": "launch", "label": "Launch (hand throw)", "auto_first_start": False, "params": {"speed_kmh": 50, "duration_s": 0.5, "pitch_deg": 10}}],
}

def active_boxes(rc, ranges, armed):
    """Permanent ids of the boxes whose range holds its channel (start <= us < end); ARM follows `armed`."""
    ids = [0] if armed else []
    for r in ranges:
        us = rc[4 + r["channel"]] if 4 + r["channel"] < len(rc) else 1500
        if r["box"] != 0 and r["start_us"] <= us < r["end_us"] and r["box"] not in ids:
            ids.append(r["box"])
    return ids

def box_names(ids, boxes):
    names = {b["id"]: b["name"] for b in boxes}
    return [names.get(i, f"box {i}") for i in ids]

def manifest_message():
    """--manifest FILE with the functions catalog added (None without the flag)."""
    if not MANIFEST:
        return None
    with open(MANIFEST, encoding="utf-8") as fh:
        m = json.load(fh)
    m["type"] = "manifest"
    ids = [a.get("id") for a in m.get("aircraft", [])]
    if "trainer" not in ids:
        m.setdefault("aircraft", []).append({"id": "trainer", "name": "Trainer", "fdm": "plane",
                                             "actuators": ["aileron_l", "aileron_r", "elevator", "rudder", "motor"], "implemented": True})
    for a in m["aircraft"]:
        if a.get("id") in FUNCTIONS:
            a["functions"] = FUNCTIONS[a["id"]]
    return m

def run_function(send, name, t_now, on_launch=None):
    """The progress of an environment function, in host time (a thread)."""
    def ev(phase, **kw):
        send(dict({"type": "function", "name": name, "phase": phase, "t": round(t_now(), 3)}, **kw))
    def body():
        if name == "calibrate_acc":
            poses = ["level", "inverted", "left side down", "right side down", "nose up", "nose down"]
            ev("start", steps=6, detail="6 poses, each held still while the firmware samples it")
            for k, p in enumerate(poses, 1):
                time.sleep(0.4)
                ev("step", step=k, steps=6, detail=p)
            time.sleep(0.3)
            ev("done", step=6, steps=6, detail="saved; ACC calibrated")
        elif name == "calibrate_gyro":
            ev("start", steps=1, detail="hold level and still 3 s (the firmware's boot calibration)")
            time.sleep(1.0)
            ev("done", step=1, steps=1, detail="gyro calibrated flag set")
        elif name == "launch":
            if on_launch is None:
                ev("failed", detail="launch: not a plane")
                return
            ev("start", steps=1, detail="50 km/h in 0.5 s at 10 deg")
            on_launch()
            time.sleep(0.6)
            ev("done", step=1, steps=1, detail="released")
        else:
            ev("failed", detail=f"unknown function '{name}'")
    threading.Thread(target=body, daemon=True).start()

def bench_state_on_connect(send, t_now):
    """--bench-state / --delta: what the bench-state core says to a client after hello (a thread for the delta's applied)."""
    if BENCH_STATE:
        send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": {"state": "restored", "source": "bench", "lines": 13, "replay": True}})
    if DELTA:
        def body():
            send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": {"state": "applying", "delta": True, "lines": DELTA}})
            send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": {"removed": ["set fake_removed = 1"]}})
            time.sleep(1.0)
            send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": {"state": "applied", "lines": DELTA, "delta": True}})
        threading.Thread(target=body, daemon=True).start()

def config_command(send, m, t_now):
    """{"type":"config","reset":"first_start"}: the first start again (applying now, applied 1 s later)."""
    if m.get("reset") not in ("first_start", "defaults"):
        send({"type": "error", "what": f"config: unknown reset {m.get('reset')!r}"})
        return
    send({"type": "log", "src": "core", "t": round(t_now(), 3), "text": f"config: reset {m.get('reset')}: the bench state discarded, first start"})
    def body():
        send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": {"state": "applying", "lines": 13}})
        time.sleep(1.0)
        send({"type": "event", "t": round(t_now(), 3), "name": "config", "data": {"state": "applied", "lines": 13}})
    threading.Thread(target=body, daemon=True).start()

def quat_from_euler(roll, pitch, yaw):
    cr, sr = math.cos(roll / 2), math.sin(roll / 2)
    cp, sp = math.cos(pitch / 2), math.sin(pitch / 2)
    cy, sy = math.cos(yaw / 2), math.sin(yaw / 2)
    return [cr * cp * cy + sr * sp * sy, sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy]

OSD_ROWS, OSD_COLS, OSD_BLINK = 18, 50, 0x40

def osd_layout(t, rpm, armed, mode):
    """{(r, c): (text, attr)} of the fake OSD: fixed-width fields so an update overwrites the old one."""
    volts = 16.8 if not armed else max(14.0, 16.8 - 0.01 * (t - 8))
    secs = int(t)
    return {(0, 1): (f"{mode:<5}", 0), (0, 44): (f"{volts:4.1f}V", 0),
            (17, 1): (f"{rpm:4.0f} RPM", 0), (8, 24): ("+", 0),
            (17, 44): (f"{secs // 60:02d}:{secs % 60:02d}", 0),
            (11, 23): ("   ", 0) if armed else ("ARM", OSD_BLINK)}

# iNav 9.1.0 drivers/osd_symbols.h (the codes the trainer-osd bench's canvas showed; page 1 = attr 1)
SYM_RSSI, SYM_SAT_L, SYM_SAT_R, SYM_DEGREES, SYM_HEADING, SYM_VOLT = 0x01, 0x08, 0x09, 0x0B, 0x0C, 0x1F
SYM_BATT_FULL, SYM_ALT_M, SYM_KMH, SYM_ON_M = 0x63, 0x76, 0x90, 0x9E
SYM_AH_DECORATION, SYM_AH_CH_LEFT, SYM_AH_CH_CENTER, SYM_AH_CH_RIGHT, SYM_AH_H_START = 0x131, 0x13A, 0x166, 0x13B, 0x14C

def inav_osd_canvas(t, mode, pitch_deg, kmh, alt_m, volts):
    """{(r, c): code} of the plane's iNav-like 50x18 canvas, code = attr << 8 | byte (blanks left out)."""
    cv = {}
    def put(r, c, text_or_codes):
        for i, x in enumerate(text_or_codes):
            cv[(r, c + i)] = ord(x) if isinstance(x, str) else x
    put(1, 2, f"{mode[:4]:<4}")
    put(1, 23, [SYM_HEADING] + list("359") + [SYM_DEGREES])
    put(1, 44, [SYM_RSSI] + list("88"))
    put(2, 2, [SYM_SAT_L, SYM_SAT_R] + list("16"))
    put(9, 8, list(f"{kmh:>3.0f}") + [SYM_KMH])
    put(9, 37, list(f"{alt_m:>3.0f}") + [SYM_ALT_M])
    for r in range(6, 13):
        put(r, 18, [SYM_AH_DECORATION]); put(r, 32, [SYM_AH_DECORATION])
    put(9, 24, [SYM_AH_CH_LEFT, SYM_AH_CH_CENTER, SYM_AH_CH_RIGHT])
    # the horizon: nose up moves it down; 9 sub-row glyphs 0x14C..0x154 per row, 1 row ~ 5 deg of pitch
    pos = max(6.0, min(12.99, 11.5 + pitch_deg / 5.0))
    row, sub = int(pos), int((pos - int(pos)) * 9)
    put(row, 20, [SYM_AH_H_START + sub] * 11)
    put(16, 2, [SYM_BATT_FULL] + list(f"{volts:4.1f}") + [SYM_VOLT])
    secs = int(t)
    put(16, 41, [SYM_ON_M] + list(f"{secs // 60:02d}:{secs % 60:02d}"))
    return {k: v for k, v in cv.items() if v != 0x20}

def osd_runs(cells):
    """[(r, c, code)] → the core's cells: runs of one row, consecutive columns, one attr."""
    out = []
    for r, c, code in sorted(cells):
        last = out[-1] if out else None
        if last and last["r"] == r and last["c"] + len(last["s"]) == c and last["attr"] == code >> 8:
            last["s"] += chr(code & 0xFF)
        else:
            out.append({"r": r, "c": c, "s": chr(code & 0xFF), "attr": code >> 8})
    return out

def serve(conn, addr):
    print("client", addr)
    f = conn.makefile("rwb", buffering=0)
    lock = threading.Lock()
    def send(obj):
        with lock:
            f.write((json.dumps(obj) + "\n").encode())
    OSD_OBJ = {"kind": "msp_displayport", "cols": OSD_COLS, "rows": OSD_ROWS, "video": True, "uart": 4}
    send({"type": "hello", "core": "fake-core", "version": "0.0", "board": "nexus-xr",
          "firmware": "rotorflight 4.6.0 STM32F7X2_OSD (NEXUS_XR) (fake core)", "aircraft": "rs7",
          "actuators": ["swash_a", "swash_b", "swash_c", "tail", "motor"], "state_hz": 100,
          "osd": OSD_OBJ})
    if manifest_message() is not None:
        send(manifest_message())
    send({"type": "signals", "items": SIGNALS})
    send({"type": "diff", "t": 0.0, "text": RS7_DIFF, "stale": STALE_DIFF, "source": "first_start", "at": 0.0, "replay": True})
    rc = [1500] * 16
    running = True
    t = 0.0
    cli = False
    boot_until = 0.0                 # after a reboot the firmware does not answer MSP for ~2 s (fc.msp false)
    def reboot():
        nonlocal boot_until
        boot_until = t + 2.0
        send({"type": "event", "t": round(t, 3), "name": "reboot", "data": {"cause": "reset"}})
    bench_state_on_connect(send, lambda: t)
    subscribed = []
    osd_on = True
    osd_sent = {}                    # (r, c) -> (text, attr) last sent: only changes go out
    osd_draws = 0                    # the device's DRAW count: 12/s, the unchanged ones not sent
    send({"type": "osd", "frame": osd_draws, "rows": OSD_ROWS, "cols": OSD_COLS, "cells": [], "clear": True, "link": True, "font": "rotorflight"})
    def reader():
        nonlocal running, rc, subscribed, osd_on, cli
        try:
            for line in f:
                try:
                    m = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if m.get("type") == "rc":
                    rc = m.get("ch_us", rc)
                elif m.get("type") == "time":
                    running = m.get("mode") != "pause"
                    send({"type": "log", "src": "core", "t": t, "text": f"time {m.get('mode')}"})
                elif m.get("type") == "cli":
                    leave = m.get("exit") is True or str(m.get("text", "")).strip() == "exit"
                    if leave:
                        send({"type": "log", "src": "cli", "t": t, "text": "# exit\nRebooting (fake core)"})
                        if cli:
                            cli = False
                            send({"type": "event", "t": round(t, 3), "name": "cli", "data": {"open": False, "by": "client"}})
                            reboot()
                        elif m.get("exit") is True:
                            send({"type": "error", "what": "cli exit: the firmware is not in its CLI"})
                    else:
                        if not cli:
                            cli = True
                            send({"type": "event", "t": round(t, 3), "name": "cli", "data": {"open": True, "by": "client"}})
                        send({"type": "log", "src": "cli", "t": t, "text": f"# {m.get('text')}\n(fake core has no firmware)"})
                elif m.get("type") == "config":
                    if not config_layers_command(send, m, lambda: t, lambda: t > 8 and not DISARMED, RS7_SCRIPT, RS7_DIFF, reboot):
                        config_command(send, m, lambda: t)
                elif m.get("type") == "reset":
                    send({"type": "event", "t": t, "name": "reset", "data": m})
                elif m.get("type") == "modes":
                    send(modes_msg(t, False))
                elif m.get("type") == "diff" and m.get("against") == "script":
                    send(diff_against_script(t, RS7_SCRIPT, RS7_DIFF, t > 8 and not DISARMED))
                elif m.get("type") == "diff":
                    send(diff_answer(m, t, RS7_DIFF, t > 8 and not DISARMED, "first_start"))
                elif m.get("type") == "function":
                    run_function(send, str(m.get("name")), lambda: t)
                elif m.get("type") == "subscribe":
                    if m.get("modes") is True:
                        send({"type": "log", "src": "core", "t": t, "text": "modes: subscribed (kept current after each boot)"})
                        send(modes_msg(t, False))
                    known = {x["id"] for x in SIGNALS}
                    if "signals" in m:
                        subscribed = [i for i in m.get("signals", []) if i in known]
                    if isinstance(m.get("osd"), bool):
                        if m["osd"] and not osd_on:
                            osd_sent.clear()          # turned back on: resend every cell
                        osd_on = m["osd"]
                    send({"type": "log", "src": "core", "t": t, "text": f"subscribed {len(subscribed)} signals"})
                else:
                    send({"type": "log", "src": "core", "t": t, "text": "got " + m.get("type", "?")})
        except (OSError, ValueError):
            pass
        running = False
    threading.Thread(target=reader, daemon=True).start()
    dt = 0.01
    tick = 0
    try:
        while True:
            if running:
                t += dt
                tick += 1
                rpm = min(1800.0, t * 150.0)
                alt = 0.0 if t < 12 else min(15.0, (t - 12) * 1.0)
                ang = 0.0 if t < 20 else (t - 20) * 0.25
                n, e = 20 * math.sin(ang), 20 * (1 - math.cos(ang))
                roll = 0.0 if t < 20 else 0.2
                pitch = 0.0 if t < 20 else -0.08
                yaw = ang
                q = quat_from_euler(roll, pitch, yaw)
                armed = t > 8 and not DISARMED
                boxes = active_boxes(rc, RS7_RANGES, armed)
                mode = "+".join(n for n in box_names(boxes, RS7_BOXES) if n != "ARM") or "ACRO"
                send({"type": "state", "t": round(t, 3), "pos_ned": [n, e, -alt], "vel_ned": [0, 0, 0],
                      "quat": q, "euler_deg": [math.degrees(roll), math.degrees(pitch), math.degrees(yaw)],
                      "gyro_dps": [0, 0, math.degrees(0.25 if t > 20 else 0)], "accel_g": [0, 0, -1],
                      "rpm": rpm, "motor_rpm": rpm * 8.92, "actuators": {"swash_a": 0.3, "swash_b": 0.3, "swash_c": 0.3, "tail": 0.0, "motor": rpm / 1800},
                      "heli": {"coll_deg": round(2.0 + 4.5 * rpm / 1800, 2), "thrust_n": round(30.0 * (rpm / 1800) ** 2, 1)},
                      "pwm_us": [1600, 1600, 1600, 1500, 1000, 1000, 1000, 2000 if armed else 1000],
                      "on_ground": alt <= 0.0, "osd": OSD_OBJ,
                      "fc": {"msp": not cli and t >= boot_until, "armed": armed, "mode": mode, "arming_flags": [] if armed else ["RXLOSS"], "cli": cli, "power": True, "baro_alt_m": round(alt, 2),
                             "cpu_pct": 6, "link": {"rssi_dbm": -60, "lq": 100}, "rc_seen": rc[:8],
                             "boxes": boxes},
                      "timing": {"realtime_factor": 1.0, "host_t": round(t, 3)}})
                if tick % 8 == 0:
                    osd_draws += 1
                if osd_on and tick % 10 == 0:
                    cells = [{"r": r, "c": c, "s": s, "attr": a} for (r, c), (s, a) in osd_layout(t, rpm, armed, mode).items()
                             if osd_sent.get((r, c)) != (s, a)]
                    if cells:
                        for x in cells:
                            osd_sent[(x["r"], x["c"])] = (x["s"], x["attr"])
                        send({"type": "osd", "frame": osd_draws, "rows": OSD_ROWS, "cols": OSD_COLS, "cells": cells, "clear": False, "link": True, "font": "rotorflight"})
                if subscribed:
                    climb = 0.0 if t < 12 or alt >= 15 else 1.0
                    servo = 1600 + 40 * math.sin(t * 2.0)
                    values = {"fdm.alt_m": alt, "fdm.climb_mps": climb,
                              "in.gyro_raw_dps.x": random.gauss(0, 0.8), "in.gyro_raw_dps.y": random.gauss(0, 0.8),
                              "in.gyro_raw_dps.z": math.degrees(0.25 if t > 20 else 0) + random.gauss(0, 0.8),
                              "out.servo1_us": servo, "out.servo2_us": servo, "out.servo3_us": servo, "out.servo4_us": 1500 + 20 * math.sin(t * 5.0),
                              "out.motor1_dshot": 48 + rpm / 1800 * 1500 if armed else 0, "fc.gov.target_rpm": 1800 if armed else 0}
                    send({"type": "samples", "t": round(t, 3), "v": {i: round(values[i], 4) for i in subscribed}})
            time.sleep(dt)
    except (OSError, ValueError):
        pass
    print("client gone", addr)

def serve_plane(conn, addr):
    """The trainer: first-start config + calibration events, then hand launch on `launch`."""
    print("client (plane)", addr)
    f = conn.makefile("rwb", buffering=0)
    lock = threading.Lock()
    def send(obj):
        with lock:
            f.write((json.dumps(obj) + "\n").encode())
    OSD_OBJ = ({"kind": "msp_displayport", "cols": OSD_COLS, "rows": OSD_ROWS, "video": True, "uart": 4} if PLANE_OSD
               else {"kind": "max7456", "cols": 30, "rows": 16, "video_system": "PAL", "video": False})
    send({"type": "hello", "core": "fake-core", "version": "0.0", "board": "speedybee-f405-wing",
          "firmware": "inav_9.1.0_SPEEDYBEEF405WING.hex", "aircraft": "trainer",
          "actuators": ["aileron_l", "aileron_r", "elevator", "rudder", "motor"], "state_hz": 100,
          "osd": OSD_OBJ})
    if manifest_message() is not None:
        send(manifest_message())
    st = {"rc": [1500] * 16, "running": True, "launch_at": None, "reset": False, "t": 0.0, "armed": False, "modes_sub": False, "diff": None, "boot_until": 0.0,
          "osd_on": PLANE_OSD, "osd_shown": {}, "osd_draws": 0, "osd_full_at": -1.0}
    def plane_reboot():
        st["boot_until"] = st["t"] + 2.0
        send({"type": "event", "t": round(st["t"], 3), "name": "reboot", "data": {"cause": "reset"}})
    bench_state_on_connect(send, lambda: st["t"])
    def start_launch():
        if st["launch_at"] is None:
            st["launch_at"] = "pending"
            send({"type": "log", "src": "core", "t": 0, "text": "launch: 50 km/h in 0.5 s at 10 deg (a 27.8 m/s^2, IMU X 29.5 m/s^2)"})
            return True
        return False
    def reader():
        try:
            for line in f:
                try:
                    m = json.loads(line)
                except json.JSONDecodeError:
                    continue
                typ = m.get("type")
                if typ == "rc":
                    st["rc"] = m.get("ch_us", st["rc"])
                elif typ == "time":
                    st["running"] = m.get("mode") != "pause"
                elif typ == "launch":
                    if not start_launch():
                        send({"type": "error", "what": "launch: a launch is running"})
                elif typ == "modes":
                    send(modes_msg(st["t"], True))
                elif typ == "subscribe":
                    if PLANE_OSD and isinstance(m.get("osd"), bool):
                        if m["osd"] and not st["osd_on"]:
                            st["osd_full_at"] = -1.0          # turned back on: a full frame next
                        st["osd_on"] = m["osd"]
                    if m.get("modes") is True:
                        st["modes_sub"] = True
                        send({"type": "log", "src": "core", "t": st["t"], "text": "modes: subscribed (kept current after each boot)"})
                        if st["t"] > 3.0:
                            send(modes_msg(st["t"], True))
                elif typ == "diff" and m.get("against") == "script":
                    send(diff_against_script(st["t"], plane_script(), st["diff"] or INAV_DIFF, st["armed"]))
                elif typ == "diff":
                    send(diff_answer(m, st["t"], st["diff"], st["armed"], "first_start"))
                elif typ == "function":
                    run_function(send, str(m.get("name")), lambda: st["t"], on_launch=start_launch)
                elif typ == "reset":
                    st["reset"] = True
                elif typ == "config":
                    if not config_layers_command(send, m, lambda: st["t"], lambda: st["armed"], plane_script(), st["diff"] or INAV_DIFF, plane_reboot):
                        config_command(send, m, lambda: st["t"])
                else:
                    send({"type": "log", "src": "core", "t": 0, "text": "got " + str(typ)})
        except (OSError, ValueError):
            pass
        st["running"] = False
    threading.Thread(target=reader, daemon=True).start()
    # first start, compressed: (t, event name, data)
    poses = [("level", 0), ("inverted", 1), ("left side down", 5), ("right side down", 4), ("nose up", 2), ("nose down", 3)]
    timeline = [(3.0, "config", {"state": "applying", "lines": 13}), (4.0, "config", {"state": "calibrating", "lines": 13, "function": "calibrate_acc"}),
                (4.0, "function", {"phase": "start", "step": 0, "steps": 6, "detail": "Calibrate accelerometer (6 positions) (first start)", "first_start": True})]
    timeline += [(5.0 + i, "pose", {"label": l, "index": ix, "step": i + 1, "scenario": "acc-calibration"}) for i, (l, ix) in enumerate(poses)]
    timeline += [(5.3 + i, "function", {"phase": "step", "step": i + 1, "steps": 6, "detail": f"{l} (iNav position {ix}) collected"}) for i, (l, ix) in enumerate(poses)]
    timeline += [(11.0, "pose", {"label": "back to level", "step": 7, "scenario": "acc-calibration"}),
                 (12.0, "scenario_end", {"scenario": "acc-calibration"}),
                 (12.1, "function", {"phase": "done", "step": 6, "steps": 6, "detail": "all 6 positions; the first start saves it", "acc_calibrated": True}),
                 (12.2, "diff", {}),
                 (12.5, "config", {"state": "applied", "lines": 13, "functions": ["calibrate_acc"], "acc_calibrated": True}),
                 (13.5, "pose", {"label": "launch attitude", "step": 1, "scenario": "launch"})]
    timeline.sort(key=lambda x: x[0])
    dt, t, tick = 0.01, 0.0, 0
    x = h = v = 0.0          # distance north, height, speed
    pitch = 0.0
    released = None          # t of release
    ramp_t0 = None
    armed_was = False
    fw_said = False
    try:
        while True:
            if not st["running"]:
                time.sleep(dt)
                continue
            t += dt
            tick += 1
            st["t"] = t
            if not fw_said and t > 3.0:
                fw_said = True
                send({"type": "log", "src": "core", "t": round(t, 3), "text": "firmware says: inav 9.1.0 SPEEDYBEEF405WING (fake core)"})
                if st["modes_sub"]:
                    send(modes_msg(t, True))
            while timeline and timeline[0][0] <= t:
                _, name, data = timeline.pop(0)
                if name == "function":
                    send(dict({"type": "function", "t": round(t, 3), "name": "calibrate_acc"}, **data))
                    continue
                if name == "diff":
                    st["diff"] = INAV_DIFF
                    send({"type": "diff", "t": round(t, 3), "text": INAV_DIFF, "stale": STALE_DIFF, "source": "first_start", "at": round(t, 3)})
                    continue
                send({"type": "event", "t": round(t, 3), "name": name, "data": data})
                if name == "config" and data.get("state") == "applied" and st["modes_sub"]:     # the core reads them again after the config's reboot
                    send(modes_msg(t, True))
            if st["reset"]:
                st["reset"] = False
                x = h = v = 0.0; released = ramp_t0 = None; st["launch_at"] = None
                send({"type": "event", "t": round(t, 3), "name": "reset", "data": {"what": "aircraft", "firmware": False}})
                send({"type": "event", "t": round(t, 3), "name": "pose", "data": {"label": "launch attitude", "step": 1, "scenario": "launch"}})
            rc = st["rc"]
            armed = t > 13 and rc[4] > 1700
            st["armed"] = armed
            if armed != armed_was:
                armed_was = armed
                send({"type": "event", "t": round(t, 3), "name": "armed" if armed else "disarmed", "data": {}})
            launch = "none"
            if st["launch_at"] == "pending" and t > 13:
                st["launch_at"] = ramp_t0 = t
                send({"type": "event", "t": round(t, 3), "name": "launch", "data": {"phase": "start", "a_mss": 27.78, "speed_mps": 13.89, "heading_deg": 0.0, "pitch_deg": 10.0}})
                send({"type": "event", "t": round(t, 3), "name": "launch_command", "data": {"label": "", "step": 2, "scenario": "launch"}})
                send({"type": "event", "t": round(t, 3), "name": "scenario_end", "data": {"scenario": "launch"}})
            if ramp_t0 is not None and released is None:
                launch = "ramp"
                v = min(13.89, (t - ramp_t0) / 0.5 * 13.89)
                x += v * math.cos(math.radians(10)) * dt; h += v * math.sin(math.radians(10)) * dt
                pitch = 10.0
                if t - ramp_t0 >= 0.5:
                    released = t
                    send({"type": "event", "t": round(t, 3), "name": "launch", "data": {"phase": "release", "speed_mps": 13.89, "height_m": round(h, 2)}})
            elif released is not None:
                motor = armed and t - released > 0.5
                target = (16.8 if h < 28 else 0.0) if motor else -3.0
                pitch += (target - pitch) * min(1.0, dt * 1.5)
                v += ((13.7 if motor else 12.5) - v) * dt * 0.5
                x += v * math.cos(math.radians(pitch)) * dt; h = max(0.0, h + v * math.sin(math.radians(pitch)) * dt)
            else:
                pitch = 10.0 if t > 13.5 else 0.0
            held = released is None
            rpm = 9990.0 if (released is not None and armed and t - released > 0.5) else 0.0
            elev = -3.0 + 2.0 * math.sin(t * 0.7) if released is not None else 0.0
            ail = 1.5 * math.sin(t * 1.3) if released is not None else 0.0
            q = quat_from_euler(0.0, math.radians(pitch), 0.0)
            boxes = active_boxes(rc, INAV_RANGES, armed)
            mode = "+".join(n for n in box_names(boxes, INAV_BOXES) if n != "ARM") or "MANUAL"
            msp = t > 3.0 and not (12.5 < t < 13.0) and t >= st["boot_until"]
            send({"type": "state", "t": round(t, 3), "pos_ned": [x, 0.0, -h], "vel_ned": [v, 0.0, 0.0], "quat": q,
                  "euler_deg": [0.0, pitch, 0.0], "gyro_dps": [0, 0, 0], "accel_g": [0, 0, -1], "rpm": rpm,
                  "actuators": {"aileron_l": ail / 20, "aileron_r": -ail / 20, "elevator": -elev / 20, "rudder": 0.0, "motor": 0.7 if rpm else 0.0},
                  "on_ground": h <= 0.0 and released is None, "osd": OSD_OBJ,
                  "fc": {"msp": msp, "armed": armed, "mode": mode, "arming_flags": [] if t > 20 else ["CAL"], "cpu_pct": 12, "cli": False, "power": True,
                         "boxes": boxes},
                  "plane": {"airspeed_mps": round(v, 2), "alpha_deg": round(2.1 if released else 0.0, 2), "beta_deg": 0.0, "thrust_n": 21.4 if rpm else 0.0,
                            "surfaces_deg": {"aileron_l": round(ail, 2), "aileron_r": round(-ail, 2), "elevator": round(elev, 2), "rudder": 0.0},
                            "launch": launch, "held": held, "posed": held and t > 14.0, "scenario": "launch" if ramp_t0 is None and t > 13 else ("acc-calibration" if 4 < t < 12 else None),
                            "wind_ned": [0, 0, 0]},
                  "timing": {"realtime_factor": 1.0, "host_t": round(t, 3)}})
            if PLANE_OSD and tick % 2 == 0:
                st["osd_draws"] += 1                    # iNav draws ~47/s with the horizon on
            if PLANE_OSD and st["osd_on"] and t > 1.0 and tick % 10 == 0:
                canvas = inav_osd_canvas(t, mode, pitch, v * 3.6, h, 16.8 if not armed else 16.4)
                font = {"font": "inav"} if t > 3.0 else {}      # the device's FC_VARIANT answered at t = 3 s
                full = t - st["osd_full_at"] >= 1.0
                if full:
                    st["osd_full_at"] = t
                    cells = [(r, c, x) for (r, c), x in canvas.items()]
                else:
                    shown = st["osd_shown"]
                    cells = [(r, c, x) for (r, c), x in canvas.items() if shown.get((r, c)) != x]
                    cells += [(r, c, 0x20) for (r, c) in shown if (r, c) not in canvas]
                if full or cells:
                    st["osd_shown"] = canvas
                    send(dict({"type": "osd", "t": round(t, 3), "frame": st["osd_draws"], "rows": OSD_ROWS, "cols": OSD_COLS,
                               "clear": full, "link": True, "cells": osd_runs(cells)}, **font))
            time.sleep(dt)
    except (OSError, ValueError):
        pass
    print("client gone", addr)

def main():
    if "--session-check" in sys.argv:
        i = sys.argv.index("--session-check")
        session_check(sys.argv[i + 1], sys.argv[i + 2] if len(sys.argv) > i + 2 else REPO)
    # the core's own form, so a wrapper (`fake-check.cmd`: python tools/fake_core.py %*) can stand in for the core
    # executable in a screenshot run: -M <machine>,session=<bench>,root=<dir>,session-check=on; without session= the
    # manifest (app/tests/manifest-fixture.json); `-M help` / `<machine>,help`: nothing
    if "-M" in sys.argv:
        opts = {}
        for kv in re.split(r"(?<!,),(?!,)", sys.argv[sys.argv.index("-M") + 1]):
            k, _, v = kv.partition("=")
            opts[k] = v.replace(",,", ",")
        if opts.get("session-check") == "on":
            if "session" in opts:
                session_check(opts["session"], opts.get("root") or REPO)
            with open(os.path.join(REPO, "app", "tests", "manifest-fixture.json"), encoding="utf-8") as fh:
                sys.stdout.write("manifest: " + json.dumps(json.load(fh)) + "\n")
        sys.exit(0)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT)); s.listen(2)
    print(f"fake core on 127.0.0.1:{PORT}{' (plane: the trainer)' if PLANE else ''}")
    while True:
        conn, addr = s.accept()
        threading.Thread(target=serve_plane if PLANE else serve, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    main()
