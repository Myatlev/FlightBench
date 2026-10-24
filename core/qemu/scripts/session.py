#!/usr/bin/env python3
"""REFERENCE IMPLEMENTATION: the core resolves benches itself (-M nexus-xr,session=<bench>; the C port
is devices/src/session.c) and run.sh no longer calls this on the launch path. Kept as the reference
the C resolver is checked against (core/qemu/tests/test_session_equiv.sh: same report, warnings,
errors, blob and manifest bytes on every bench); a change of rules goes into both.

Session description (docs/protocol.md) -> shell variables for core/qemu/run.sh.

  session.py <session.json> <tmpdir>
  session.py --check <session.json>     validate only; prints the resolved assembly

Prints `NAME=value` lines (shell-quoted) for run.sh to eval: ELF, BOARD_CONFIG, PROFILE,
ICOUNT, UART_PORT, CTL_PORT, SESSION_OPTS, MACHINE (the board's QEMU machine), MSP_UART (its MSP
host UART pad), CONFIG (`unified` = a custom-defaults blob, `cli` = a CLI script) and CONFIG_CLI
(the assembled CLI script's file, empty for none). Relative paths resolve against the repo root.
A CLI board (speedybee-f405-wing, iNav) gets no blob: its configuration is one CLI script the
machine sends over the MSP UART - the board's base script (`board_config`, default board.json's
`defaults_cli`), the aircraft's `firmware_cli`, the profiles and profile_text, the wired devices'
lines; blank and `#` lines dropped, `save`/`exit` dropped with a note (the core saves at the end),
`defaults`/`reboot`/`dfu`/`bl`/`bootloader`/`msc` rejected (they reset or wipe the board mid-script).
The wiring is resolved by scripts/wiring.py (board = unified config + boards/<id>/board.json,
devices = the C descriptors, assembly = the session's names). New form ({device, port, pad}):
complete and strict, an unknown device/port/pad/prop, a port on a pad of another kind, a pad used
twice (an I2C bus takes several devices, not two of one kind), removing a soldered onboard device
or a wrong actuator is an error. Old form ({device, slot}): the receiver, ESC and servos are
assumed present, an entry that cannot be placed is a warning. Mixed forms are rejected. A valid
entry the machine does not model is reported on stderr and left out. The machine then gets exactly
the devices the assembly has (-M imu/crsf/esc/baro/servos/osd), the CLI lines a wired device needs
(descriptor `config`, e.g. the MSP DisplayPort OSD's serial port and element layout: appended to the
custom-defaults blob after the profiles), the actuator names and reversals per
output, and the aircraft file (FDM, ESC, transmission; checked by wiring.load_aircraft). The optional
`environment` {sea_level_pa, field_elevation_m, temp_c} sets what the baro sees (-M baro-env):
sea-level pressure and sea-level temperature (ICAO standard atmosphere, the lapse rate from sea level)
and the field elevation. Without `wiring` the machine keeps its defaults, the onboard baro included.
"""
import json, os, re, shlex, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wiring  # noqa: E402
import cli_norm  # noqa: E402

ROOT = wiring.ROOT
SLOTS = wiring.slots_compat()           # old import (fault_evidence.py): {(device, slot): ...}


def warn(msg):
    print(f"session: {msg}", file=sys.stderr)


def mopt(key, value):
    """One -M option: QEMU splits the -M string at commas, a literal comma is written ",,"."""
    return f"{key}={str(value).replace(',', ',,')}"


def path(p):
    if not p:
        return ""
    p = os.path.expanduser(p.replace("\\", "/"))
    if sys.platform == "win32" and p.startswith("/"):
        # MSYS2 path (/d/..., /home/...) -> what a native QEMU can open; before the isabs test,
        # which is False for "/d/..." on Windows from Python 3.13 (it would join it to ROOT's drive)
        import shutil, subprocess
        if shutil.which("cygpath"):
            p = subprocess.run(["cygpath", "-m", p], capture_output=True, text=True).stdout.strip() or p
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


def beeper_from(text):
    """resource BEEPER 1 B02 / set beeper_inversion = ON (last one wins)."""
    pin, inv = None, False
    for line in text.splitlines():
        m = re.match(r"\s*resource\s+BEEPER\s+1\s+(\S+)", line, re.I)
        if m:
            pin = None if m.group(1).upper() == "NONE" else m.group(1).upper()
        m = re.match(r"\s*set\s+beeper_inversion\s*=\s*(\S+)", line, re.I)
        if m:
            inv = m.group(1).upper() == "ON"
    return pin, inv


# session "environment": what the baro sees (standard atmosphere at field elevation + FDM altitude);
# sea_level_pa and temp_c are the sea-level reference values (temperature at 0 m, not at the field)
ENV_KEYS = {"sea_level_pa": (30000, 120000, 101325), "field_elevation_m": (-500, 9000, 0),
            "temp_c": (-60, 60, 22)}


def environment(env):
    """session `environment` -> the machine's baro-env "p0:h0:t0", "" when absent; exits on a bad value."""
    if env is None:
        return ""
    if not isinstance(env, dict):
        sys.exit("session: environment: need an object {sea_level_pa, field_elevation_m, temp_c}")
    for k, v in env.items():
        if k not in ENV_KEYS:
            sys.exit(f"session: environment.{k}: unknown key (known: {', '.join(ENV_KEYS)})")
        lo, hi, _ = ENV_KEYS[k]
        if not isinstance(v, (int, float)) or isinstance(v, bool) or not lo <= v <= hi:
            sys.exit(f"session: environment.{k} {v!r}: need a number {lo}..{hi}")
    # repr: the shortest string that round-trips the double (:g kept only 6 digits)
    return ":".join(repr(float(env.get(k, d))) for k, (_, _, d) in ENV_KEYS.items())


def assembly(s, board_json=None):
    """(board, aircraft, entries or None for 'no wiring: machine defaults', machine options)."""
    bid = s.get("board", "nexus-xr")
    if not isinstance(bid, str) or bid not in wiring.MACHINES:
        raise wiring.board_error(bid)
    spec = wiring.MACHINES[bid]
    board = wiring.load_board(bid, path(s.get("board_config", spec["default_config"] or "")), board_json, warn)
    ap = wiring.aircraft_path(s.get("aircraft"))
    if not os.path.exists(ap):
        raise wiring.WiringError(f"aircraft {s.get('aircraft')!r}: no {ap}")
    ac = wiring.load_aircraft(ap)
    w = s.get("wiring")
    if not w:
        if spec["legacy"]:
            warn("no wiring: the machine's defaults (the RS7 layout: gyro, baro on i2c3, CRSF on uart5, ESC on "
                 "motor1, servo1..4)")
            return board, ac, ap, None, {}
        warn(f"no wiring: {bid} with its onboard devices only (no receiver, no ESC, no servos)")
        w = []
    catalog = wiring.device_catalog()
    entries = wiring.resolve(w, board, catalog, ac, warn)
    return board, ac, ap, entries, wiring.machine_options(entries, ac, catalog, board)


def device_config(s, board, entries):
    """The wired devices' CLI lines (wiring.device_config), [] without wiring."""
    return wiring.device_config(entries, board, wiring.device_catalog()) if entries else []


# the CLI script of a CLI board: lines by their first word (ASCII, case-insensitive, as iNav's CLI)
CLI_DROP = ("save", "exit")
CLI_FORBID = ("defaults", "reboot", "dfu", "bl", "bootloader", "msc")


LAYERS = ("board", "aircraft", "profile", "device", "bench")


def rel_path(p):
    """a path for the report: relative to the data root when under it"""
    r = ROOT
    if not r or not p.startswith(r):
        return p
    if r.endswith("/"):
        return p[len(r):]
    return p[len(r) + 1:] if p[len(r):len(r) + 1] == "/" else p


def _cstr(z):
    return z.split("\x00", 1)[0]


def _txt(b):
    return b.decode("utf-8", "surrogateescape")


def _cli_lines(src, data, lines, notes, errs, layer, file=None, lsrc=None):
    for n, raw in enumerate(data.split(b"\n"), 1):
        line = raw.strip()
        if not line or line.startswith(b"#"):
            continue
        word = line.split(None, 1)[0].lower().decode("utf-8", "replace")
        if word in CLI_DROP:
            notes.append(f"config_cli: {src} line {n}: '{word}' dropped (the core saves at the end)")
        elif word in CLI_FORBID:
            errs.append(f"config_cli: {src} line {n}: '{word}' would reset or wipe the board mid-script, not allowed")
        else:
            idx = 1 + sum(1 for x in lines if x["layer"] == layer)
            lines.append({"b": line, "text": _txt(line), "layer": layer, "idx": idx, "by": None,
                          "source": f"{rel_path(file)}:{n}" if file else lsrc[n - 1]})


def device_sources(devcfg):
    """each device line's source: "device <name> on <pad>" from its block's "# ... (device config)" line"""
    out, cur = [], "device"
    for x in devcfg:
        if x.startswith("# ") and x.endswith(" (device config)") and len(x) > 2 + len(" (device config)"):
            cur = "device " + x[2:-len(" (device config)")]
            out.append(None)
        else:
            out.append(cur)
    return out


def apply_disabled(lines, disabled, notes):
    for d in disabled or []:
        m = re.fullmatch(r"(board|aircraft|profile|device|bench):([0-9]+)", d)
        text = d.strip(cli_norm.WS) if not m else None
        hit = 0
        for x in lines:
            if m:
                if x["layer"] != m.group(1) or x["idx"] != int(m.group(2)):
                    continue
            elif "\x00" in x["text"] or x["text"] != text:
                continue
            hit += 1
            if x["by"] is not None:
                continue
            x["by"] = d
            # (the C resolver's notes are C strings: a line with a NUL byte shows up to it)
            notes.append(f"config: disabled_lines {d!r}: [{x['layer']}:{x['source']}] {_cstr(x['text'])} not sent")
        if not hit:
            notes.append(f"config: disabled_lines {d!r}: matches no line of the script")


def find_conflicts(lines, notes):
    sent = [x for x in lines if x["by"] is None]
    out = []
    for key, idx in cli_norm.conflicts(sent):
        ls = [sent[j] for j in idx]
        notes.append(f"config: conflict: {key}: " + ", then ".join(f"[{x['layer']}:{x['source']}] {_cstr(x['text'])}" for x in ls)
                     + ": the later line wins")
        out.append((key, ls))
    return out


def config_cli(base, aircraft_cli, profiles, profile_text, devcfg, bench_cli="", disabled=None):
    """The CLI board's script: (bytes, notes, errors, prov); the lines each end in "\n", b"" = none. Sources
    in this order: the base script, the aircraft's firmware_cli, the profile files, profile_text, the
    wired devices' lines, the bench's lines (bench_cli); then disabled_lines skips lines. A file that cannot
    be read ends it (an error). prov: {"lines": [...], "files": [(layer, path)], "conflicts": [...]}"""
    lines, notes, errs, files_read = [], [], [], []
    files = ([("board config", "board", base)] if base else []) + \
        ([("aircraft firmware_cli", "aircraft", aircraft_cli)] if aircraft_cli else []) + \
        [("profile", "profile", p) for p in profiles]
    prov = {"lines": lines, "files": files_read, "conflicts": []}
    for what, layer, p in files:
        try:
            with open(p, "rb") as f:
                data = f.read()
        except OSError as e:
            errs.append(f"{what} {p}: {e.strerror}")
            return b"", notes, errs, prov
        files_read.append((layer, p))
        _cli_lines(p, data, lines, notes, errs, layer, file=p)
    if profile_text:
        t = profile_text.encode("utf-8")
        _cli_lines("profile_text", t, lines, notes, errs, "profile",
                   lsrc=[f"profile_text:{k}" for k in range(1, t.count(b"\n") + 2)])
    if devcfg:
        _cli_lines("device config", ("\n".join(devcfg) + "\n").encode("utf-8"), lines, notes, errs, "device",
                   lsrc=device_sources(devcfg) + [None])
    if bench_cli:
        try:
            with open(bench_cli, "rb") as f:
                data = f.read()
        except OSError as e:
            errs.append(f"bench_cli {bench_cli}: {e.strerror}")
            return b"", notes, errs, prov
        files_read.append(("bench", bench_cli))
        _cli_lines(bench_cli, data, lines, notes, errs, "bench", file=bench_cli)
    apply_disabled(lines, disabled, notes)
    prov["conflicts"] = find_conflicts(lines, notes)
    return b"".join(x["b"] + b"\n" for x in lines if x["by"] is None), notes, errs, prov


def config_json(kind, prov):
    """the report's config_json (session.c config_json): one line"""
    def line(x, by=False):
        d = {"text": x["text"], "layer": x["layer"], "source": x["source"], "ref": f"{x['layer']}:{x['idx']}"}
        if by:
            d["by"] = x["by"]
        return d
    return cli_norm.dumps({
        "kind": kind,
        "layers": [{"layer": la, "file": rel_path(p), "path": p} for la, p in prov["files"]],
        "lines": [line(x) for x in prov["lines"] if x["by"] is None],
        "disabled": [line(x, True) for x in prov["lines"] if x["by"] is not None],
        "conflicts": [{"setting": k, "lines": [line(x) for x in ls], "wins": f"{ls[-1]['layer']}:{ls[-1]['idx']}"}
                      for k, ls in prov["conflicts"]]})


def blob_provenance(board_config, profiles, devcfg):
    """a blob board: its custom defaults' files and the device lines with their sources"""
    lines = []
    for x, src in zip(devcfg, device_sources(devcfg)):
        t = x.strip(cli_norm.WS)
        if t and not t.startswith("#"):
            lines.append({"text": t, "layer": "device", "source": src or "device", "by": None,
                          "idx": 1 + len(lines)})
    return {"lines": lines, "files": ([("board", board_config)] if board_config else []) +
            [("profile", p) for p in profiles], "conflicts": []}


def check(sess_path):
    with open(sess_path, encoding="utf-8") as f:
        s = json.load(f)
    try:
        board, ac, ap, entries, opts = assembly(s)
    except wiring.WiringError as e:
        for line in str(e).splitlines():
            print(f"session: {line}", file=sys.stderr)
        sys.exit(1)
    pads = {p["id"]: p for p in board["pads"]}
    for e in entries or []:
        p = pads[e["pad"]]
        print(f"{e['device'] + '.' + e['port']:16s} -> {p['id']:7s} {('(' + p['label'] + ')') if p.get('label') else '':10s}"
              f"{' onboard' if e['onboard'] else ''}{' modelled' if e['modelled'] else ' left out'}"
              f"{' ' + json.dumps(e['props']) if e['props'] else ''}")
    print("machine: " + ",".join(f"{k}={v}" for k, v in opts.items()))
    for line in device_config(s, board, entries):
        print(f"config: {line}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        return check(sys.argv[2])
    if len(sys.argv) != 3:
        sys.exit("usage: session.py <session.json> <tmpdir> | session.py --check <session.json>")
    sess_path, tmp = sys.argv[1], sys.argv[2]
    with open(sess_path, encoding="utf-8") as f:
        s = json.load(f)
    out = {}
    bid = s.get("board", "nexus-xr")
    if not isinstance(bid, str) or bid not in wiring.MACHINES:
        sys.exit(f"session: {wiring.board_error(bid)}")
    spec = wiring.MACHINES[bid]
    out["ELF"] = path(s.get("firmware", ""))
    if not out["ELF"]:
        sys.exit("session: no firmware")
    out["BOARD_CONFIG"] = path(s.get("board_config", spec["default_config"] or ""))

    profiles = [path(p) for p in (s["profile"] if isinstance(s.get("profile"), list)
                                  else [s.get("profile")]) if p]
    profile_files, profile_text = list(profiles), ""
    if s.get("profile_text", "").strip():
        pt = os.path.join(tmp, "profile_text.txt")
        profile_text = s["profile_text"].replace("\r", "") + "\n"
        with open(pt, "w", encoding="utf-8", newline="\n") as f:
            f.write(profile_text)
        profiles.append(pt)
    out["PROFILE"] = " ".join(profiles)

    try:
        board, ac, ap, entries, dev_opts = assembly(s)
    except wiring.WiringError as e:
        for line in str(e).splitlines():
            warn(line)
        sys.exit("session: wiring rejected")

    cli = board["config"] == "cli"
    if cli and "board_config" not in s:
        out["BOARD_CONFIG"] = board["defaults_cli"] or ""
    bc = s.get("bench_cli")
    if bc and not isinstance(bc, str):
        sys.exit(f"session: bench_cli {bc!r}: need a path")
    bench_cli = path(bc) if bc else ""
    disabled = s.get("disabled_lines")
    if disabled is not None:
        if not isinstance(disabled, list):
            sys.exit(f"session: disabled_lines {disabled!r}: need a list of line texts or layer:index refs")
        for x in disabled:
            if not isinstance(x, str):
                sys.exit(f"session: disabled_lines {x!r}: need a string (a line's text or layer:index)")
        disabled = disabled or None
    if not cli and (bench_cli or disabled):
        sys.exit(f"session: {'bench_cli' if bench_cli else 'disabled_lines'}: only on a CLI board (config: cli); "
                 f"{bid} takes its lines in the custom defaults (profile, profile_text)")
    out["MACHINE"], out["MSP_UART"], out["CONFIG"] = board["machine"], board["msp_uart"], board["config"]
    opts = [mopt("aircraft", ac["id"])]
    if wiring.fdm_on(ac, board):
        opts += ["fdm=on", mopt("aircraft-file", ap)]
    elif wiring.has_fdm(ac):
        warn(f"aircraft {ac['id']!r}: no FDM for it on {bid}, the IMU sees a board at rest")
    else:
        warn(f"aircraft {ac['id']!r}: no FDM for it, the IMU sees a board at rest")
    cfg = device_config(s, board, entries)
    if cfg and not cli:
        dc = os.path.join(tmp, "device_config.txt")
        with open(dc, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(cfg) + "\n")
        profiles.append(dc)
        out["PROFILE"] = " ".join(profiles)
    for k in ("imu", "imu-noise", "imu-accel-error", "crsf", "esc", "esc-timeout-ms", "baro", "baro-props", "servos",
              "osd", "mag", "mag-props", "gps", "gps-props", "battery", "battery-props", "actuators", "servo-rev"):
        if k in dev_opts:
            opts.append(mopt(k, dev_opts[k]))
    env = environment(s.get("environment"))
    if env:
        if any(dev in ("dps310", "spl06") for dev, _ in spec["modelled"]):
            opts.append(mopt("baro-env", env))
        else:
            warn(f"environment: {bid} has no modelled baro, not applied")

    if cli:
        if board["beeper"]:
            opts.append(mopt("beeper", board["beeper"]))
        else:
            warn("no beeper in board.json: `beep` events only from DShot beacons (motor_beep)")
    else:
        text = ""
        for p in [out["BOARD_CONFIG"]] + profiles:
            if p and os.path.exists(p):
                with open(p, encoding="utf-8", errors="replace") as f:
                    text += f.read() + "\n"
        pin, inv = beeper_from(text)
        if pin:
            opts.append(mopt("beeper", f"{pin}{':inv' if inv else ''}"))
        else:
            warn("no resource BEEPER in the board config: `beep` events only from DShot beacons (motor_beep)")

    t = s.get("time", {})
    shift = t.get("icount_shift", 2)
    if shift is not None and (not isinstance(shift, int) or isinstance(shift, bool) or not 0 <= shift <= 10):
        sys.exit(f"session: time.icount_shift {shift!r}: need an integer 0..10 or null")
    out["ICOUNT"] = f"-icount shift={shift},sleep=off" if shift is not None else ""
    if t.get("realtime"):
        opts.append("realtime=on")
    skip = t.get("idle_skip", False)
    if not isinstance(skip, bool):
        sys.exit(f"session: time.idle_skip {skip!r}: need true|false")
    if skip:
        if shift is None:
            sys.exit("session: time.idle_skip needs time.icount_shift (virtual time = instruction count)")
        opts.append("idle-skip=on")
        for k, lo, hi in (("idle_skip_n", 2, 1000), ("idle_skip_gran_us", 1, 100000)):
            if k in t:
                v = t[k]
                if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                    sys.exit(f"session: time.{k} {v!r}: need an integer {lo}..{hi}")
                opts.append(f"{k.replace('_', '-')}={v}")
    ports = s.get("ports", {})
    for k, default in (("control", 31250), ("msp", 31235)):
        p = ports.get(k, default)
        if not isinstance(p, int) or isinstance(p, bool) or not 1 <= p <= 65535:
            sys.exit(f"session: ports.{k} {p!r}: need a TCP port 1..65535")
    out["CTL_PORT"] = str(ports.get("control", 31250))
    out["UART_PORT"] = str(ports.get("msp", 31235))
    out["SESSION_OPTS"] = ",".join(opts)
    out["CONFIG_CLI"] = ""
    if cli:
        text, notes, errs, prov = config_cli(out["BOARD_CONFIG"], path(ac.get("firmware_cli") or ""), profile_files,
                                             profile_text, cfg, bench_cli, disabled)
        for m in notes + errs:
            warn(m)
        if errs:
            sys.exit("session: wiring rejected")
        if text:
            out["CONFIG_CLI"] = os.path.join(tmp, "config.cli")
            with open(out["CONFIG_CLI"], "wb") as f:
                f.write(text)
    else:
        prov = blob_provenance(out["BOARD_CONFIG"], profile_files, cfg)
    # the layered assembly (the report's config_src / config_disabled / config_conflict / config_json)
    out["CONFIG_JSON"] = os.path.join(tmp, "config.json")
    with open(out["CONFIG_JSON"], "wb") as f:
        f.write(config_json("cli" if cli else "blob", prov).encode("utf-8", "surrogateescape"))
    for k, v in out.items():
        print(f"{k}={shlex.quote(v)}")


if __name__ == "__main__":
    main()
