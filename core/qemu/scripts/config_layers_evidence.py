#!/usr/bin/env python3
"""The firmware's config as layers with provenance, bench lines, explicit operations
(evidence/config-layers/).

The trainer (iNav 9.1.0, speedybee-f405-wing, with its GPS) with bench lines (`bench_cli`: a file
next to the bench) and `disabled_lines`, an empty cfg-cache and one bench-state file throughout:
  A  session-check: every script line with its layer and source, the conflict (a `set` in the bench
     lines over the aircraft's), the disabled device line, config_json
  S  first start: the `applying` event's `sent` (each line with its layer and source), the resolver's
     conflict note, the automatic functions; the firmware has the bench's value
  D  {"type":"diff","against":"script"}: the script against the firmware's `diff all`, setting by setting
  M  the configurator's Modes tab "Save" replayed (as bench_state_evidence.py M), then `diff against
     script` again: the ranges the configurator added are the firmware's only
  P  apply now: the bench lines edited (a value changed, a line added), {"type":"config","apply":true}:
     the delta against what the sector has + the settings the firmware's config changed (M's `aux 2`:
     the script's value again, `reasserted`), each with the context lines in force at it, typed +
     save; `applying` / `applied` {apply, delta, sent, removed, reasserted}; the same again ->
     nothing to type; `full` -> every line
  N  snapshot: {"type":"config","snapshot":"bench","path":..} -> the diff path (reboot), the file
     written, {"type":"config","snapshot":"written",..}; the bench then names that file as its
     bench_cli (what the app does): restart -> the bench state restored + the snapshot's lines as the
     delta (the running config: the bench state wins); config reset first_start -> the whole first
     start with the snapshot as layer 4: the Modes of M are back (the snapshot is the starting diff)
  E  {"type":"config","export":true}: the layered assembly, the script, `applied`, the last diff
  Z  refusals: apply while a run is busy, snapshot without a path, a bad `against`, config with two
     operations; apply on a bench whose file is broken

usage: config_layers_evidence.py [stages, default "A S D M P N E Z"]
  EV_TMP=<an earlier run's tmp dir>: go on with its bench, files and bench state (e.g. one stage again).
  QEMU=<qemu-system-arm> (default ~/flightbench-spike/qemu-cl), OUT=<dir> (default
  core/qemu/evidence/config-layers), PORT_BASE (default 31270: control, MSP, HMP, motion, pilot).
  Prints the log on stdout; the snapshot file and the JSON of the answers go to OUT.
"""
import datetime, json, os, shutil, socket, subprocess, sys, tempfile, time

sys.stdout.reconfigure(line_buffering=True)
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from core_client import Core  # noqa: E402
sys.path.insert(0, HERE)
import winproc  # noqa: E402
from configurator_handshake import v2, Reader  # noqa: E402

PORT0 = int(os.environ.get("PORT_BASE", "31270"))
CTL, MSP, MON, MOTION, PILOT = range(PORT0, PORT0 + 5)
STAGES = (sys.argv[1] if len(sys.argv) > 1 else "A S D M P N E Z").split()
QEMU = os.environ.get("QEMU", os.path.expanduser("~/flightbench-spike/qemu-cl/build/qemu-system-arm"))
OUT = os.environ.get("OUT", os.path.join(HERE, "../evidence/config-layers"))
TMP = os.environ.get("EV_TMP") or tempfile.mkdtemp(prefix="config-layers-ev.")   # EV_TMP: an earlier run's
CACHE = os.path.join(TMP, "cfg-cache")
BDIR = os.path.join(TMP, "bench")
STATE = os.path.join(BDIR, "trainer.state.bin")
BENCH = os.path.join(BDIR, "trainer.json")
BENCH_CLI = os.path.join(BDIR, "trainer.cli")
SNAP = os.path.join(BDIR, "trainer.snapshot.cli")
os.makedirs(OUT, exist_ok=True)
os.makedirs(BDIR, exist_ok=True)
MSP_MODE_RANGES, MSP_SET_MODE_RANGE, MSP_EEPROM_WRITE = 34, 35, 250

# layer 4: one line over the aircraft's (inav.cli: nav_fw_bank_angle = 35), one of its own
BENCH_LINES = "# the bench's own lines (the app writes this file)\nset nav_fw_bank_angle = 40\nset nav_fw_launch_thr = 1750\n"
DISABLED = ["device:4"]             # the ublox's `set gps_ublox_nav_hz = 10`: iNav's default (5) instead


def write_bench(bench_cli=BENCH_CLI, disabled=DISABLED):
    d = json.load(open(os.path.join(ROOT, "core/qemu/sessions/trainer.json")))
    d["ports"] = {"control": CTL, "msp": MSP}
    d["bench_cli"] = bench_cli
    d["disabled_lines"] = disabled
    json.dump(d, open(BENCH, "w"), indent=1)


class Run:
    """run.sh --session in the background with the bench state; the app's client subscribed"""
    def __init__(self, name):
        self.log = os.path.join(TMP, name + ".log")
        env = dict(os.environ, QEMU=QEMU, MON_PORT=str(MON), MOTION_PORT=str(MOTION), PILOT_PORT=str(PILOT),
                   MACHINE_OPTS=f"cfg-cache={CACHE}", BENCH_STATE=STATE)
        self.p = winproc.popen_run_sh([os.path.join(HERE, "../run.sh"), "--session", BENCH],
                                      stdout=open(self.log, "w"), stderr=subprocess.STDOUT, env=env)
        self.core = Core(CTL, timeout=60)
        self.core.send({"type": "subscribe", "modes": True, "state_hz": 5})
        self.msgs = []

    def wait(self, pred, timeout, what):
        end = time.time() + timeout
        while time.time() < end:
            m = self.core.wait_for(lambda m: m["type"] != "state", end - time.time())
            if m is None:
                break
            self.msgs.append(m)
            if pred(m):
                return m
        print(f"  !! timeout waiting for {what}")
        return None

    def ask(self, msg, pred, timeout, what):
        self.core.send(msg)
        return self.wait(pred, timeout, what)

    def msgs_last_modes(self):
        ms = [m for m in self.msgs if m["type"] == "modes" and "ranges" in m]
        return ms[-1] if ms else None

    def lines(self, *keys):
        return [l.rstrip() for l in open(self.log) if any(k in l for k in keys)]

    def stop(self):
        winproc.stop(self.p, STATE)


def cfg(state, **kw):
    return lambda m: m["type"] == "event" and m.get("name") == "config" and m["data"].get("state") == state and \
        all(m["data"].get(k) == v for k, v in kw.items())


def is_type(t, key=None):
    return lambda m: m["type"] == t and (key is None or key in m)


def ranges_str(m):
    names = {b["id"]: b["name"] for b in m.get("boxes", [])}
    return "; ".join(f"slot {r['slot']} {names.get(r['box'], r['box'])} ch{r['rc_channel']} "
                     f"{r['start_us']}-{r['end_us']}" for r in m.get("ranges", []))


def up(run, n=1, timeout=900):
    """the firmware answers (n-th boot of this run), its boot calibration over"""
    end = time.time() + timeout
    while time.time() < end and len(run.lines("firmware says")) < n:
        time.sleep(0.5)
    run.core.wait_t(run.core.t() + 3, timeout=120)


CHECKED = ("automatic functions' results are in the sector", "run now", "calibrations are not checked")


def checked(run, n=1, timeout=300):
    """the restored sector's calibration check done (the config flow idle: apply/snapshot allowed)"""
    end = time.time() + timeout
    while time.time() < end and len(run.lines(*CHECKED)) < n:
        time.sleep(0.5)


def settle(run, s=3):
    run.core.wait_t(run.core.t() + s, timeout=120)


def sent_summary(ev):
    sent = ev["data"].get("sent", [])
    by = {}
    for x in sent:
        by[x["layer"]] = by.get(x["layer"], 0) + 1
    return f"{len(sent)} lines sent: " + ", ".join(f"{k} {v}" for k, v in by.items())


def show_against(m, keys=("changed", "only_firmware", "only_script"), limit=12):
    if not m:
        print("  against script: -")
        return
    print(f"  diff against script (diff {m.get('source')} at t={m.get('at')}, stale {m.get('stale')}"
          f"{', error ' + m['error'] if m.get('error') else ''}): {len(m.get('sent', []))} script settings, "
          f"{len(m.get('current', []))} firmware settings; " +
          ", ".join(f"{len(m.get(k, []))} {k}" for k in keys))
    for k in keys:
        for x in m.get(k, [])[:limit]:
            src = f" [{x['layer']}:{x['source']}]" if x.get("layer") else ""
            print(f"    {k}: {x['setting']}: script {x.get('script', '-')!r} firmware {x.get('firmware', '-')!r}{src}")
        if len(m.get(k, [])) > limit:
            print(f"    {k}: ... {len(m[k]) - limit} more")


def against(run, refresh=True, timeout=120):
    """the comparison; a refresh refused while iNav's boot calibration runs is asked again"""
    for _ in range(15):
        m = run.ask({"type": "diff", "against": "script", "refresh": refresh},
                    lambda m: m["type"] == "diff" and m.get("against") == "script", timeout, "diff against script")
        if not m or m.get("error") != "calibrating":
            return m
        print(f"  (diff refused: calibrating at t={m['t']:.2f}; again in 2 virtual s)")
        settle(run, 2)
    return m


def save(name, obj):
    with open(os.path.join(OUT, name), "w") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)


def msp(s, r, cmd, payload=b"", timeout=5):
    s.sendall(v2(cmd, payload))
    end = time.time() + timeout
    while time.time() < end:
        f = r.frame(end)
        if f and f[2] == cmd:
            return f[3]
    raise TimeoutError(f"MSP {cmd}: no reply")


# ---- stages ------------------------------------------------------------------------------------

def stage_a():
    print("\n## A. session-check: the layers of the trainer + bench lines + disabled_lines", flush=True)
    print(f"  bench_cli {BENCH_CLI}:\n    " + "\n    ".join(BENCH_LINES.rstrip().split("\n")))
    print(f"  disabled_lines {DISABLED}")
    r = subprocess.run([QEMU, "-M", f"speedybee-f405-wing,session={BENCH},root={ROOT},session-check=on",
                        "-display", "none"], capture_output=True, text=True)
    print(f"  exit {r.returncode}; session: lines:\n    " + "\n    ".join(r.stderr.strip().split("\n")))
    lines = r.stdout.split("\n")
    src = [l for l in lines if l.startswith("config_src: ")]
    by = {}
    for l in src:
        la = l.split("[", 1)[1].split(":", 1)[0]
        by[la] = by.get(la, 0) + 1
    print(f"  config_src: {len(src)} lines ({', '.join(f'{k} {v}' for k, v in by.items())}); first/last of each layer:")
    for la in by:
        ls = [l for l in src if l.startswith(f"config_src: [{la}:")]
        print("    " + ls[0] + ("\n    ...\n    " + ls[-1] if len(ls) > 1 else ""))
    for l in lines:
        if l.startswith(("config_disabled: ", "config_conflict: ")):
            print("  " + l)
    cj = next(l for l in lines if l.startswith("config_json: "))[len("config_json: "):]
    j = json.loads(cj)
    save("a-config_json.json", j)
    print(f"  config_json: kind {j['kind']}, layers {[(x['layer'], x['file']) for x in j['layers']]}, "
          f"{len(j['lines'])} lines, {len(j['disabled'])} disabled, {len(j['conflicts'])} conflicts "
          f"(a-config_json.json); a line: {json.dumps(j['lines'][-1])}; the conflict: "
          f"{json.dumps(j['conflicts'][0]) if j['conflicts'] else '-'}")
    ok = r.returncode == 0 and "bench" in by and j["disabled"] and j["disabled"][0]["ref"] == "device:4" and \
        any(c["setting"] == "set nav_fw_bank_angle" and c["wins"] == "bench:1" for c in j["conflicts"])
    print(f"  A: {'PASS' if ok else 'FAIL'}")


def stage_s():
    print("\n## S. first start with the layers (empty cfg-cache, no bench state)", flush=True)
    for p in (STATE, STATE[:-4] + ".json"):
        if os.path.exists(p):
            os.unlink(p)
    run = Run("s-first")
    try:
        ap = run.wait(cfg("applying"), 600, "applying")
        if ap:
            print(f"  event config applying: {sent_summary(ap)}")
            for x in [y for y in ap["data"]["sent"] if y["layer"] != "aircraft"]:
                print(f"    {json.dumps(x)}")
            save("s-applying.json", ap)
        done = run.wait(lambda m: cfg("applied")(m) and "functions" in m["data"] or cfg("failed")(m), 900,
                        "the first start applied")
        print(f"  {json.dumps(done['data']) if done else '-'}")
        print("  " + "\n  ".join(run.lines("session: config:")))
        up(run, 3)
        d = against(run)
        show_against(d)
        fw = {x["setting"]: x["text"] for x in (d or {}).get("current", [])}
        print(f"  the firmware: {fw.get('set nav_fw_bank_angle', '(default)')!r}, "
              f"{fw.get('set nav_fw_launch_thr', '(default)')!r}, gps_ublox_nav_hz {fw.get('set gps_ublox_nav_hz', '(default: not in the diff)')!r}")
        ok = ap and done and cfg("applied")(done) and fw.get("set nav_fw_bank_angle") == "set nav_fw_bank_angle = 40" \
            and "set gps_ublox_nav_hz" not in fw
        print(f"  S: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


def stage_d():
    print("\n## D. diff against script on a restart (refresh:false = the bench state's diff, then refresh)", flush=True)
    run = Run("d-diff")
    try:
        run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        checked(run)
        m0 = against(run, refresh=False)
        show_against(m0, limit=4)
        m1 = against(run)
        show_against(m1, limit=40)
        save("d-against.json", m1)
        ok = m1 and not m1.get("error") and m1["source"] == "request" and not m1["changed"]
        print(f"  D: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


EXTRA = [("ANGLE", 900, 1300), ("HORIZON", 1300, 1700), ("MANUAL", 1500, 2100)]
AUX = 3                             # aux 4 = rc channel 8


def stage_m():
    print("\n## M. the configurator's Modes tab Save replayed, then diff against script", flush=True)
    run = Run("m-modes")
    try:
        m0 = run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        checked(run)
        ids = {b["name"]: b["id"] for b in m0["boxes"]}
        s = socket.create_connection(("127.0.0.1", MSP), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        r = Reader(s, time.time(), False)
        raw = msp(s, r, MSP_MODE_RANGES)
        slots = [raw[i:i + 4] for i in range(0, len(raw), 4)]
        used = [bytes(x) for x in slots if x[2] < x[3]]
        used += [bytes([ids[n], AUX, (a - 900) // 25, (b - 900) // 25]) for n, a, b in EXTRA]
        order = [b["id"] for b in m0["boxes"]]
        new = sorted(used, key=lambda x: order.index(x[0]))
        new += [bytes([0, 0, 0, 0])] * (len(slots) - len(new))
        for i, x in enumerate(new):
            msp(s, r, MSP_SET_MODE_RANGE, bytes([i]) + x)
        msp(s, r, MSP_EEPROM_WRITE)
        s.close()
        want = {(ids[n], AUX, a, b) for n, a, b in EXTRA}
        m1 = run.wait(lambda m: m["type"] == "modes" and want <= {(x["box"], x["channel"], x["start_us"], x["end_us"])
                                                                   for x in m.get("ranges", [])}, 60, "the new modes")
        print(f"  modes after the save: {ranges_str(m1) if m1 else '-'}")
        st = [m for m in run.msgs if m["type"] == "diff" and m.get("stale") and "against" not in m] or \
            run.wait(lambda m: m["type"] == "diff" and m.get("stale") and "against" not in m, 30, "diff stale")
        print(f"  the diff marked stale by the configurator's save: {bool(st)}")
        d = against(run)
        show_against(d)
        save("m-against.json", d)
        aux = [x for x in (d or {}).get("changed", []) + (d or {}).get("only_firmware", []) if x["setting"].startswith("aux ")]
        ok = m1 and d and len(aux) >= 3
        print(f"  {len(aux)} aux settings differ from the script (the configurator's slots)")
        print(f"  M: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


def stage_p():
    print("\n## P. apply now: the bench lines edited, config apply (the delta), again (nothing), full", flush=True)
    run = Run("p-apply")
    try:
        run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        checked(run)
        new = BENCH_LINES.replace("= 40", "= 42") + "set nav_fw_launch_idle_thr = 1100\n"
        open(BENCH_CLI, "w").write(new)
        print("  bench_cli now:\n    " + "\n    ".join(new.rstrip().split("\n")))
        # the bench state's diff is stale (M's configurator save): the core runs `diff all` first
        ap = run.ask({"type": "config", "apply": True}, lambda m: cfg("applying")(m) or m["type"] == "error", 180, "applying")
        print(f"  event config applying: {json.dumps(ap['data'] if ap and ap['type'] == 'event' else ap)}")
        dn = run.wait(lambda m: cfg("applied")(m) or cfg("failed")(m), 120, "applied")
        print(f"  event config {dn['data']['state'] if dn else '-'}: {json.dumps(dn['data']) if dn else '-'}")
        save("p-applying.json", ap)
        print("  " + "\n  ".join(run.lines("config apply", "config: apply", "diff: ")))
        up(run, 3)          # boots: the start, the diff's exit, the apply's save
        d = against(run, refresh=False)
        show_against(d, limit=6)
        fw = {x["setting"]: x["text"] for x in (d or {}).get("current", [])}
        print(f"  the firmware: {fw.get('set nav_fw_bank_angle')!r}, {fw.get('set nav_fw_launch_idle_thr')!r}, "
              f"{fw.get('aux 2')!r} (M's configurator had aux 2 1 3 900 1300)")
        again = run.ask({"type": "config", "apply": True}, lambda m: cfg("applied")(m) or m["type"] == "error", 30, "again")
        print(f"  apply again: {json.dumps(again['data'] if again and again['type'] == 'event' else again)}")
        up(run, 3)
        fa = run.ask({"type": "config", "apply": True, "full": True}, lambda m: cfg("applying")(m) or m["type"] == "error", 60, "full")
        print(f"  apply full: applying {sent_summary(fa) if fa and fa['type'] == 'event' else fa}")
        fd = run.wait(lambda m: cfg("applied")(m) or cfg("failed")(m), 180, "full applied")
        print(f"  apply full: {json.dumps(fd['data']) if fd else '-'}")
        up(run, 4)
        sent = [x["text"] for x in (ap or {}).get("data", {}).get("sent", [])]
        ctx = ("control_profile ", "battery_profile ", "mixer_profile ")
        re_ = [x["setting"] for x in (ap or {}).get("data", {}).get("reasserted", [])]
        print(f"  reasserted: {re_}; the modes after apply full: "
              f"{ranges_str(run.msgs_last_modes()) if run.msgs_last_modes() else '-'}")
        ok = ap and ap["type"] == "event" and ap["data"].get("delta") and \
            [x for x in sent if not x.startswith(ctx)] == ["aux 2 36 2 1700 2100", "set nav_fw_bank_angle = 42",
                                                           "set nav_fw_launch_idle_thr = 1100"] and \
            re_ == ["aux 2"] and fw.get("aux 2") == "aux 2 36 2 1700 2100" and \
            ap["data"].get("removed") == ["set nav_fw_bank_angle = 40"] and dn and cfg("applied")(dn) and \
            fw.get("set nav_fw_bank_angle") == "set nav_fw_bank_angle = 42" and again and again["type"] == "event" and \
            again["data"].get("lines") == 0 and fd and cfg("applied")(fd) and not fd["data"].get("delta")
        print(f"  P: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


def stage_n():
    print("\n## N. snapshot -> the bench's lines -> restart (bench state wins) -> reset first_start (the snapshot is the start)", flush=True)
    run = Run("n-snapshot")
    try:
        m0 = run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        checked(run)
        ms0 = [x for x in run.msgs if x["type"] == "modes" and "ranges" in x]
        before = ranges_str(ms0[-1]) if ms0 else ranges_str(m0 or {})
        print(f"  modes before the snapshot: {before}")
        sn = run.ask({"type": "config", "snapshot": "bench", "path": SNAP},
                     lambda m: m["type"] == "config" and "snapshot" in m or m["type"] == "error", 120, "snapshot")
        print(f"  answer: {json.dumps(sn)}")
        print("  " + "\n  ".join(run.lines("config snapshot")))
        up(run, 2)
    finally:
        run.stop()
    if not sn or sn.get("snapshot") != "written":
        print("  N: FAIL (no snapshot)")
        return
    shutil.copy(SNAP, os.path.join(OUT, "trainer.snapshot.cli"))
    txt = open(SNAP).read().split("\n")
    print(f"  the file ({len(txt)} lines, trainer.snapshot.cli in OUT): " + " | ".join(txt[:4]) + " | ...")
    # the app names the file as the bench's lines (its own edits folded in: the snapshot has them)
    write_bench(bench_cli=SNAP, disabled=DISABLED)
    print(f"  the bench's bench_cli now {SNAP}")
    run = Run("n-restart")
    try:
        m = run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        end = time.time() + 120
        while time.time() < end and not run.lines("automatic functions' results are in the sector", "run now",
                                                  "calibrations are not checked"):
            time.sleep(0.5)
        settle(run)
        for x in run.msgs:
            if x["type"] == "event" and x["name"] == "config":
                d = dict(x["data"])
                if "sent" in d:
                    d["sent"] = sent_summary(x)
                print(f"  event config {json.dumps(d)[:400]}")
        ms = [x for x in run.msgs if x["type"] == "modes" and "ranges" in x]
        print(f"  modes: {ranges_str(ms[-1]) if ms else '-'}")
        restored = [x for x in run.msgs if cfg("restored", source="bench")(x)]
        mod_kept = ms and ranges_str(ms[-1]) == before
        print(f"  restored from the bench state: {bool(restored)}; the modes as before the snapshot: {bool(mod_kept)}")
        # a fresh first start: the script (with the snapshot as layer 4), the functions
        rs = run.ask({"type": "config", "reset": "first_start"}, cfg("reset"), 30, "reset")
        ap = run.wait(cfg("applying"), 300, "applying")
        print(f"  reset first_start: applying {sent_summary(ap) if ap else '-'}")
        done = run.wait(lambda m: cfg("applied")(m) and "functions" in m["data"] or cfg("failed")(m), 900, "applied")
        print(f"  {json.dumps(done['data']) if done else '-'}")
        up(run, 4)
        m2 = run.wait(lambda m: m["type"] == "modes" and "ranges" in m and m["t"] > (done or {}).get("t", 0), 120, "modes")
        print(f"  modes after the first start: {ranges_str(m2) if m2 else '-'}")
        fresh = m2 and ranges_str(m2) == before
        d = against(run)
        show_against(d, limit=8)
        save("n-against.json", d)
        ok = restored and mod_kept and rs and done and cfg("applied")(done) and fresh
        print(f"  N: {'PASS' if ok else 'FAIL'} (the modes of the snapshot back after a fresh first start: {bool(fresh)})")
    finally:
        run.stop()


def stage_e():
    print("\n## E. export", flush=True)
    run = Run("e-export")
    try:
        run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        checked(run)
        ex = run.ask({"type": "config", "export": True}, lambda m: m["type"] == "config" and "export" in m or
                     m["type"] == "error", 60, "export")
        save("e-export.json", ex)
        e = (ex or {}).get("export", {})
        a = e.get("assembly") or {}
        print(f"  bench {e.get('bench')}, root {e.get('root')}")
        print(f"  assembly: kind {a.get('kind')}, layers {[(x['layer'], x['file']) for x in a.get('layers', [])]}, "
              f"{len(a.get('lines', []))} lines, {len(a.get('disabled', []))} disabled, {len(a.get('conflicts', []))} conflicts")
        print(f"  script {len((e.get('script') or '').splitlines())} lines, applied {e.get('applied')}, diff "
              f"{(e.get('diff') or {}).get('source')} at {(e.get('diff') or {}).get('at')}, "
              f"{len(((e.get('diff') or {}).get('text') or '').splitlines())} lines")
        print("  " + "\n  ".join(run.lines("config export")))
        ok = ex and ex["type"] == "config" and a.get("kind") == "cli" and e.get("applied") is True
        print(f"  E: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


def stage_z():
    print("\n## Z. refusals", flush=True)
    run = Run("z-refuse")
    ok = True
    try:
        run.wait(is_type("modes", "ranges"), 900, "modes")
        up(run)
        checked(run)
        for msg, want in (({"type": "config", "snapshot": "bench"}, "need \"path\""),
                          ({"type": "config", "apply": True, "export": True}, "need one of"),
                          ({"type": "diff", "against": "board"}, "against must be"),
                          ({"type": "config", "apply": False}, "apply must be true")):
            e = run.ask(msg, is_type("error"), 10, "error")
            print(f"  {json.dumps(msg)} -> {e['what'] if e else '-'}")
            ok &= bool(e and want in e["what"])
        # busy: a second apply while the first runs
        cur = json.load(open(BENCH))["bench_cli"]      # the file the bench names now (after N: the snapshot)
        open(cur, "a").write("set nav_fw_launch_idle_thr = 1150\n")
        run.core.send({"type": "config", "apply": True})
        e = run.ask({"type": "config", "apply": True}, is_type("error"), 10, "busy")
        print(f"  apply while an apply runs -> {e['what'] if e else '-'}")
        ok &= bool(e and "busy" in e["what"])
        run.wait(lambda m: cfg("applied")(m) or cfg("failed")(m), 120, "applied")
        up(run, 2)
        # the bench file broken: nothing changes, the resolver's error
        good = open(BENCH).read()
        open(BENCH, "w").write(good.replace('"wiring"', '"wiring": 5, "x"', 1))   # wiring not a list
        e = run.ask({"type": "config", "apply": True}, is_type("error"), 30, "broken bench")
        print(f"  apply on a broken bench -> {e['what'] if e else '-'}")
        ok &= bool(e and "rejected" in e["what"])
        open(BENCH, "w").write(good)
        print("  " + "\n  ".join(run.lines("config apply")[-4:]))
        print(f"  Z: {'PASS' if ok else 'FAIL'}")
    finally:
        run.stop()


def main():
    rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    print(f"# config-layers evidence, {datetime.datetime.now():%Y-%m-%d %H:%M} (host local), {rev}; QEMU {QEMU}; "
          f"ports {CTL}..{PILOT}; tmp {TMP}")
    if not os.environ.get("EV_TMP"):
        open(BENCH_CLI, "w").write(BENCH_LINES)
        write_bench()
    for st in STAGES:
        {"A": stage_a, "S": stage_s, "D": stage_d, "M": stage_m, "P": stage_p, "N": stage_n, "E": stage_e,
         "Z": stage_z}[st]()
    left = subprocess.run(["pgrep", "-f", f"ctl=127.0.0.1:{CTL}"], capture_output=True, text=True).stdout.split()
    print(f"# qemu-system-arm on my ports left: {len(left)}")


if __name__ == "__main__":
    main()
