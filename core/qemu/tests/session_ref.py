#!/usr/bin/env python3
"""The reference side of the session equivalence test (test_session_equiv.sh): what the C
resolver's `-M nexus-xr,session=<bench>,session-check=on` report must be, built only from the
reference implementation - scripts/session.py (run mode and --check), scripts/mkdefaults.sh and
scripts/manifest.py, run as core/qemu/run.sh ran them before the machine resolved benches itself;
on a CLI board (speedybee-f405-wing) the CLI script session.py assembles instead of the blob.

  session_ref.py <bench.json>     report on stdout (the format of devices/src/session.c report()),
                                  the `session: ` lines on stderr (without the final
                                  "session: wiring rejected"), exit 2 when the bench is rejected

The data root is the repo root (the Python scripts know no other).
"""
import json, os, re, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, "..", "scripts")
sys.path.insert(0, SCRIPTS)
import wiring  # noqa: E402

ROOT = wiring.ROOT


def run(args):
    if sys.platform == "win32" and args[0].endswith(".sh"):
        # a native Python cannot exec a shell script; PATH's bash (MSYS2), not System32's (WSL)
        import shutil
        args = [shutil.which("bash") or "bash"] + args
        # the shell's messages in English: main() maps "line N: <path>: No such file or directory"
        return subprocess.run(args, capture_output=True, text=True, env=dict(os.environ, LC_ALL="C"))
    return subprocess.run(args, capture_output=True, text=True)


LAYER_ORDER = ("board", "aircraft", "profile", "device", "bench")


def order(cj, x):
    """a line's place in the script: layers in order, then the index in its layer"""
    return (LAYER_ORDER.index(x["layer"]), x["idx"] if "idx" in x else int(x["ref"].split(":")[1]))


def main():
    bench = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        r = run([sys.executable, os.path.join(SCRIPTS, "session.py"), bench, tmp])
        warns = [line for line in r.stderr.splitlines() if line != "session: wiring rejected"]
        if r.returncode != 0:
            sys.stderr.write("".join(line + "\n" for line in warns))
            return 2
        env = {}
        for line in r.stdout.splitlines():
            k, v = line.split("=", 1)
            env[k] = " ".join(shlex.split(v))
        chk = run([sys.executable, os.path.join(SCRIPTS, "session.py"), "--check", bench])
        if chk.returncode != 0:
            sys.stderr.write(chk.stderr)
            return 2
        with open(bench, encoding="utf-8") as f:
            s = json.load(f)
        ap = wiring.aircraft_path(s.get("aircraft"))
        ac = wiring.load_aircraft(ap)
        out = chk.stdout
        out += f"root: {ROOT}\n"
        out += f"firmware: {env['ELF']}\n"
        board = s.get("board", "nexus-xr")
        cli = env["CONFIG"] == "cli"
        out += f"board: {board}\n"
        out += f"board_machine: {env['MACHINE']}\n"
        out += f"board_config: {env['BOARD_CONFIG']}\n"
        out += f"aircraft: {ac['id']} {ap}\n"
        text_size = None
        for p in env["PROFILE"].split():
            if p == os.path.join(tmp, "profile_text.txt"):
                text_size = os.path.getsize(p)
            elif p != os.path.join(tmp, "device_config.txt"):
                out += f"profile: {p}\n"
        if text_size is not None:
            out += f"profile_text: {text_size} bytes\n"
        opts = env["SESSION_OPTS"]
        out += f"options: {opts}\n"
        out += f"icount: {env['ICOUNT'].replace('-icount ', '') or 'none'}\n"
        out += f"realtime: {'on' if 'realtime=on' in opts.split(',') else 'off'}\n"
        out += f"ports: control={env['CTL_PORT']} msp={env['UART_PORT']}\n"
        shift = env["ICOUNT"].replace("-icount shift=", "").replace(",sleep=off", "") or "none"
        out += (f"launch: icount_shift={shift} realtime={'on' if 'realtime=on' in opts.split(',') else 'off'}"
                f" control={env['CTL_PORT']} msp={env['UART_PORT']} machine={env['MACHINE']}\n")
        mf = run([sys.executable, os.path.join(SCRIPTS, "manifest.py"), "--board", board] +
                 (["--board-config", env["BOARD_CONFIG"]] if env["BOARD_CONFIG"] and not cli else []))
        if mf.returncode != 0:
            sys.stderr.write(mf.stderr)
            return 2
        # the resolver builds the manifest after the defaults blob: its warnings (aircraft left
        # out) follow, and a defaults failure stops before them
        mf_warns = [f"session: {line}" for line in mf.stderr.splitlines()]
        out += f"manifest: {mf.stdout}"
        text = b""
        if env["CONFIG_CLI"]:
            with open(env["CONFIG_CLI"], "rb") as f:
                text = f.read()
        lines = text.split(b"\n")[:-1]
        out += f"config_cli: {len(lines)} lines\n" if lines else "config_cli: none\n"
        out += "".join(f"config_cli| {x.decode('utf-8', 'surrogateescape')}\n" for x in lines)
        # the layered assembly: session.py's config.json, as session.c report() writes it
        with open(env["CONFIG_JSON"], "rb") as f:
            cj_text = f.read().decode("utf-8", "surrogateescape")
        cj = json.loads(cj_text)
        if cli:
            for x in sorted(cj["lines"] + cj["disabled"], key=lambda x: order(cj, x)):
                if "by" in x:
                    out += f"config_disabled: [{x['layer']}:{x['source']}] {x['text']} (disabled_lines {x['by']!r})\n"
                else:
                    out += f"config_src: [{x['layer']}:{x['source']}] {x['text']}\n"
        for c in cj["conflicts"]:
            out += f"config_conflict: {c['setting']}:" + " ->".join(
                f" [{x['layer']}:{x['source']}] {x['text']}" for x in c["lines"]) + " (the last wins)\n"
        out += f"config_json: {cj_text}\n"
        blob = b""
        if env["BOARD_CONFIG"] and not cli:
            d = run([os.path.join(SCRIPTS, "mkdefaults.sh"), env["BOARD_CONFIG"], os.path.join(tmp, "d.bin")]
                    + env["PROFILE"].split())
            if d.returncode != 0:
                # mkdefaults.sh's own messages (set -e: the shell's for a missing file) in the
                # resolver's words: "defaults too big: ..." / "profile <path>: <strerror>"
                for line in d.stderr.splitlines():
                    m = re.match(r".*: line \d+: (.*): (No such file or directory|Permission denied)$", line)
                    warns.append(f"session: profile {m.group(1)}: {m.group(2)}" if m else f"session: {line}")
                sys.stderr.write("".join(line + "\n" for line in warns))
                return 2
            with open(os.path.join(tmp, "d.bin"), "rb") as f:
                blob = f.read()
            out += f"defaults: {len(blob)} bytes (max {0x08004000 - 0x08002808})\n"
        else:
            out += "defaults: none\n"
        warns += mf_warns
    sys.stderr.write("".join(line + "\n" for line in warns))
    sys.stdout.buffer.write(out.encode("utf-8", "surrogateescape") + blob)
    return 0


if __name__ == "__main__":
    # LF on every platform: the C resolver writes LF, Windows' text-mode stderr would write CRLF
    sys.stderr.reconfigure(newline="\n")
    sys.exit(main())
