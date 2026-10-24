#!/bin/bash
# Host-time ledger (-M prof=on) evidence:
#   A  tests: make test (devices, fdm, core/qemu/tests incl. test_prof), test_wiring.py, py_compile
#   B  RS7 liftoff plan to 45 virtual s with prof=on -> evidence/prof/rs7-liftoff-<host>-<date>.json/.txt
#   C  bare boot: the same session, no plan, 20 virtual s with prof=on -> ...-bare-boot-...json/.txt
#   D  overhead: liftoff to 45 s, prof=off and prof=on alternating, RUNS (default 3) each, host s per 10 virtual s (ping-watched,
#      virtual 5..45 s)
#   E  determinism: FDM log (every 20 ms, to 45 s) of every D run and of B: prof=on == prof=off
# Everything on its own core: control 31280, MSP 31281, HMP 31282, motion 31283, pilot 31284.
# usage: prof_evidence.sh [session.json, default sessions/rs7.json] [stages, default "A B C D E"]
#   QEMU=<qemu-system-arm with patches/prof-qemu.diff> (default ~/flightbench-spike/qemu-prof/build/...)
#   OUT=<dir> (default core/qemu/evidence/prof), TAG=<host name in file names> (default homelab)
# Prints the log on stdout.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SESSION=${1:-$HERE/../sessions/rs7.json}
STAGES=${2:-A B C D E}
export QEMU=${QEMU:-$HOME/flightbench-spike/qemu-prof/build/qemu-system-arm}
export CTL_PORT=31280 UART_PORT=31281 MON_PORT=31282 MOTION_PORT=31283 PILOT_PORT=31284
OUT=${OUT:-$HERE/../evidence/prof}
TAG=${TAG:-homelab}
DATE=$(TZ=Asia/Yerevan date +%Y-%m-%d)
TMP=$(mktemp -d /tmp/prof-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }
# the session with this script's ports
python3 - "$SESSION" "$TMP/session.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["ports"] = {"control": 31280, "msp": 31281}
json.dump(d, open(sys.argv[2], "w"))
EOF
S=$TMP/session.json
# run <name> <prof 0|1> <plan|""> <until> [extra prof.py args]: core + prof.py; FDM log in $TMP/<name>-fdm.csv
run() {
  local name=$1 prof=$2 plan=$3 until=$4; shift 4
  PROF=$prof MACHINE_OPTS="fdm-log=$TMP/$name-fdm.csv,fdm-log-ms=20" \
    "$HERE/../run.sh" --session "$S" >"$TMP/$name-core.log" 2>&1 &
  QPID=$!
  python3 "$HERE/prof.py" run --session "$S" --pilot $PILOT_PORT --until "$until" --speed-from 5 \
    ${plan:+--plan "$plan"} "$@" | grep -E '^speed|^wrote'
  stop_core
  grep -m1 'prof=on' "$TMP/$name-core.log"
}
cut45() { awk -F, 'NR == 1 || $1 <= 45.0' "$TMP/$1-fdm.csv" > "$TMP/$1.cut"; }

echo "# prof evidence, $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$HERE/.." || echo +dirty); $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'); load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "# QEMU=$QEMU; session $SESSION; other qemu-system-arm processes: $(pgrep -c -x qemu-system-arm)"

if [[ $STAGES == *A* ]]; then
  echo; echo "## A. tests"
  for d in devices fdm core/qemu/tests; do make -s -C "$ROOT/$d" test 2>&1 | grep -E 'OK|FAIL|checks|passed|failed|Error' ; done
  python3 "$HERE/test_wiring.py" 2>&1 | tail -1
  python3 -m py_compile "$HERE"/*.py && echo "py_compile: all scripts OK"
fi

if [[ $STAGES == *B* ]]; then
  echo; echo "## B. RS7 liftoff plan, 45 virtual s, prof=on"
  F=$OUT/rs7-liftoff-$TAG-$DATE
  run B 1 liftoff 45 --out "$F.json"
  python3 "$HERE/prof.py" show "$F.json" > "$F.txt" && echo "show -> $F.txt"
  cut45 B
fi

if [[ $STAGES == *C* ]]; then
  echo; echo "## C. bare boot: no plan (disarmed on the pad), 20 virtual s, prof=on"
  F=$OUT/rs7-bare-boot-$TAG-$DATE
  run C 1 "" 20 --out "$F.json"
  python3 "$HERE/prof.py" show "$F.json" > "$F.txt" && echo "show -> $F.txt"
fi

if [[ $STAGES == *D* ]]; then
  echo; echo "## D. overhead: ${RUNS:-3} runs each, alternating; liftoff to 45 virtual s, host s per 10 virtual s over virtual 5..45 (ping-watched, 20 Hz)"
  for r in $(seq "${RUNS:-3}"); do
    for p in 0 1; do
      echo "### prof=$([ $p = 1 ] && echo on || echo off) run $r"
      if [ $p = 1 ]; then run D$p-$r 1 liftoff 45 --out "$TMP/D1-$r.json"
      else run D$p-$r 0 liftoff 45 --no-dump | tee -a "$TMP/D0-speeds.txt"; fi
      cut45 D$p-$r
    done
  done
  python3 - "$TMP" <<'EOF2'
import json, glob, statistics, sys
on = [json.load(open(f))["meta"]["speed"]["host_s_per_10_virtual_s"] for f in sorted(glob.glob(sys.argv[1] + "/D1-*.json"))]
off = [float(l.split()[1]) for l in open(sys.argv[1] + "/D0-speeds.txt")]
print(f"prof=off: {', '.join(f'{x:.2f}' for x in off)}; mean {statistics.mean(off):.2f}, stdev {statistics.stdev(off):.2f}")
print(f"prof=on:  {', '.join(f'{x:.2f}' for x in on)}; mean {statistics.mean(on):.2f}, stdev {statistics.stdev(on):.2f}")
print(f"overhead: {100 * (statistics.mean(on) / statistics.mean(off) - 1):+.1f} % (means)")
EOF2
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. determinism: FDM log to 45 s (20 ms rows, printed precision)"
  for f in "$TMP"/*.cut; do echo "$(basename "$f" .cut): $(md5sum < "$f" | cut -c1-12) $(wc -l < "$f") rows"; done
  ref=$(ls "$TMP"/D0-*.cut 2>/dev/null | head -1)
  for f in "$TMP"/*.cut; do
    [ "$f" = "$ref" ] && continue
    cmp -s "$ref" "$f" && echo "$(basename "$f" .cut) == $(basename "$ref" .cut): identical" ||
      echo "$(basename "$f" .cut) vs $(basename "$ref" .cut): DIFFERENT ($(cmp "$ref" "$f" | grep -o 'line [0-9]*'))"
  done
fi
echo; echo "# qemu-system-arm processes left by this script: none (the other $(pgrep -c -x qemu-system-arm) are not ours)"
