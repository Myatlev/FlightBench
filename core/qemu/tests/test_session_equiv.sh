#!/bin/bash
# Session equivalence: the machine's bench resolver (C, devices/src/session.c, run as
# `qemu-system-arm -M nexus-xr,session=<bench>,root=<repo>,session-check=on`) against the Python
# reference implementation (scripts/session.py run mode + --check, mkdefaults.sh, manifest.py, as
# run.sh ran them; tests/session_ref.py puts their output in the session-check report format).
# For every bench: the same exit (accepted / rejected), the same `session:` warning and error
# lines, and a byte-identical report: the resolved devices and machine options (the
# `session.py --check` lines), firmware/board config/aircraft/profiles, the -M option string
# (SESSION_OPTS), icount, realtime, ports, the launch line (machine=), the manifest line (byte for
# byte), a CLI board's script (config_cli lines) and the custom-defaults blob (byte for byte).
# Benches: core/qemu/sessions/*.json, tests/benches/*.json (old slot form, new form with detached
# onboard devices, a WiringError case, the old form on a board config without motor/servo pads), the JSON files under app/tests/ that are benches (have
# "wiring" or "firmware"), and tests/gen_benches.py's (the test_wiring.py cases, the speedybee-f405-wing
# ones (output pads, CLI scripts, firmware_cli) and the other bench
# blocks, ~170).
#   QEMU=<qemu-system-arm with the session options> core/qemu/tests/test_session_equiv.sh
#   (make -C core/qemu/tests equiv); VERBOSE=1 prints a line per generated bench too.
#   SESSION_TOOL=devices/build/fb_session core/qemu/tests/test_session_equiv.sh
#   (make -C core/qemu/tests equiv-host): the same C resolver through the host tool
#   (devices/tools/fb_session.c prints what session-check=on prints), no QEMU build needed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
QEMU=${QEMU:-$HOME/flightbench-spike/qemu/build/qemu-system-arm}
TMP=$(mktemp -d /tmp/session-equiv.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
TOOL=${SESSION_TOOL:-}
if [ -n "$TOOL" ]; then
  if [ ! -x "$TOOL" ]; then
    echo "test_session_equiv: SESSION_TOOL=$TOOL is not an executable (make -C devices tools)" >&2
    exit 1
  fi
elif ! "$QEMU" -M nexus-xr,help 2>/dev/null | grep -q 'session-check='; then
  echo "test_session_equiv: $QEMU has no -M session-check (build the tree: core/qemu/build.sh <dir>; QEMU=...)" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "test_session_equiv: no python3 (the reference implementation, scripts/session.py, needs it)" >&2
  exit 1
fi
BENCHES=("$ROOT"/core/qemu/sessions/*.json "$HERE"/benches/*.json)
APP=0
while IFS= read -r f; do BENCHES+=("$f"); APP=$((APP + 1)); done < <(python3 - "$ROOT/app/tests" <<'EOF'
import glob, json, os, sys
for p in sorted(glob.glob(os.path.join(sys.argv[1], "**", "*.json"), recursive=True)):
    try:
        d = json.load(open(p, encoding="utf-8"))
    except ValueError:
        continue
    if isinstance(d, dict) and ("wiring" in d or "firmware" in d):
        print(p)
EOF
)
NFIX=${#BENCHES[@]}
python3 "$HERE/gen_benches.py" "$TMP/gen"
BENCHES+=("$TMP"/gen/*.json)
if [ -n "$TOOL" ]; then
  echo "# session equivalence: $TOOL (host tool, the same resolver) vs the Python reference"
else
  echo "# session equivalence: $QEMU ($(strings "$QEMU" | grep -m1 '^flightbench ')) vs the Python reference"
fi
echo "# $((NFIX - APP)) fixed benches, $APP app fixtures that are benches, $((${#BENCHES[@]} - NFIX)) generated"
pass=0 fail=0 i=0
for b in "${BENCHES[@]}"; do
  i=$((i + 1))
  if [ -n "$TOOL" ]; then
    "$TOOL" --root "$ROOT" "$b" >"$TMP/c.out" 2>"$TMP/c.all"
  else
    # MSYS2 converts POSIX paths in arguments, not inside the -M string: give a native QEMU D:/...
    bq=$b rq=$ROOT
    if command -v cygpath >/dev/null 2>&1; then bq=$(cygpath -m "$b"); rq=$(cygpath -m "$ROOT"); fi
    "$QEMU" -M "nexus-xr,session=${bq//,/,,},root=${rq//,/,,},session-check=on" -display none \
      >"$TMP/c.out" 2>"$TMP/c.all"
  fi
  crc=$?
  grep '^session: ' "$TMP/c.all" >"$TMP/c.err"
  python3 "$HERE/session_ref.py" "$b" >"$TMP/r.out" 2>"$TMP/r.err"
  rrc=$?
  name=${b#"$ROOT"/}; name=${name#"$TMP"/}
  why=
  [ "$crc" = "$rrc" ] || why="exit C $crc, reference $rrc"
  cmp -s "$TMP/c.err" "$TMP/r.err" || why="${why:+$why; }session: lines differ"
  cmp -s "$TMP/c.out" "$TMP/r.out" || why="${why:+$why; }report differs"
  if [ -n "$why" ]; then
    fail=$((fail + 1))
    echo "FAIL $name: $why"
    diff "$TMP/r.err" "$TMP/c.err" | head -10
    diff <(head -c 100000 "$TMP/r.out") <(head -c 100000 "$TMP/c.out") | cut -c1-200 | head -10
    continue
  fi
  pass=$((pass + 1))
  if [ "$i" -le "$NFIX" ] || [ -n "${VERBOSE:-}" ]; then
    nw=$(grep -c . "$TMP/c.err")
    if [ "$crc" = 0 ]; then
      python3 - "$name" "$TMP/c.out" "$nw" <<'EOF'
import sys
name, path, nw = sys.argv[1], sys.argv[2], sys.argv[3]
b = open(path, "rb").read()
head, _, blob = b.partition(b"\ndefaults: ")
lines = head.decode().split("\n")
dev = sum(1 for l in lines if " -> " in l)
man = next(l for l in lines if l.startswith("manifest: "))
blob_n = blob.split(b" ", 1)[0].decode() if not blob.startswith(b"none") else "none"
cli = next((l for l in lines if l.startswith("config_cli: ")), "config_cli: none")[12:]
src = sum(1 for l in lines if l.startswith("config_src: "))
dis = sum(1 for l in lines if l.startswith("config_disabled: "))
con = sum(1 for l in lines if l.startswith("config_conflict: "))
config = (f"defaults blob {blob_n} B byte-identical" if blob_n != "none" else "no blob") + \
    (f"; CLI script {cli} identical ({src} with provenance, {dis} disabled, {con} conflicts; config_json)"
     if cli != "none" else "; config_json")
print(f"PASS {name}: accepted; {dev} assembly entries, machine options, -M string, icount/ports identical; "
      f"manifest {len(man) - 10} B identical; {config}; {nw} warnings identical")
EOF
    else
      echo "PASS $name: rejected by both, $(grep -c . "$TMP/c.err") session: lines identical"
    fi
  fi
done
echo "# $pass passed, $fail failed (of ${#BENCHES[@]})"
# the settings normaliser (devices/src/cli_norm.c vs scripts/cli_norm.py): a script against a `diff
# all` text, tests/fixtures/cli_diff/<name>.script.txt + <name>.diff.txt, through the host tool
# (the QEMU binary has no mode for it; the machine links the same devices/src/cli_norm.c)
DIFFTOOL=${TOOL:-$ROOT/devices/build/fb_session}
dpass=0 dfail=0
if [ -x "$DIFFTOOL" ]; then
  for sc in "$HERE"/fixtures/cli_diff/*.script.txt; do
    df=${sc%.script.txt}.diff.txt name=$(basename "${sc%.script.txt}")
    "$DIFFTOOL" --cli-diff "$sc" "$df" >"$TMP/cd.c" 2>&1
    python3 "$ROOT/core/qemu/scripts/cli_norm.py" --diff "$sc" "$df" >"$TMP/cd.r" 2>&1
    if cmp -s "$TMP/cd.c" "$TMP/cd.r"; then
      dpass=$((dpass + 1))
      echo "PASS cli_diff/$name: $(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(", ".join(f"{len(d[k])} {k}" for k in d))' "$TMP/cd.c") identical"
    else
      dfail=$((dfail + 1))
      echo "FAIL cli_diff/$name: the C and Python diffs differ"
      diff "$TMP/cd.r" "$TMP/cd.c" | cut -c1-200 | head -6
    fi
  done
  echo "# cli_diff: $dpass passed, $dfail failed"
else
  echo "# cli_diff: skipped, no $DIFFTOOL (make -C devices tools)"
fi
[ "$fail" = 0 ] && [ "$dfail" = 0 ]
