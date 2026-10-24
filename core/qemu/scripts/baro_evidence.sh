#!/bin/bash
# Baro evidence (I2C3 + DPS310/SPL06 on it, scripts/baro_evidence.py): prints the log.
#   A  unit tests (devices, fdm, wiring, the I2C controller: core/qemu/tests) and the manifest's dps310 entry
#   B  boot on the pad: baro signals, the firmware's altitude after calibration, CLI status/tasks
#   C  lift-off plan (flight.py --csv + flight_report.py): the base build (main, no baro model) and
#      this build; t / fdm_alt / fc_baro_alt table in the climb; I2C errors after the flight
#   D  baro detached: CLI status, the same flight, I2C errors after it, fault -> not wired
#   E  dps310.freeze in a climb, then off; freeze across an MCU reset and a power cycle (on the pad)
#   F  speed: host s per 10 virtual s in flight (flight.py --quiet, from 40 s), baro on vs detached;
#      determinism: md5 of the FDM log and of the baro trace (the chip's pressure incl. its noise,
#      baro_reg_pa, and the firmware's altitude, fc_baro_alt, from the FDM log) of the two runs
#   G  ctl_evidence.py and fault_evidence.py on the RS7 session
# usage: baro_evidence.sh [stages, default "A B C D E F G"]
#   QEMU=<this build> BASE_QEMU=<base build> BASE_TREE=<base checkout> (C compares against it)
# Everything on its own core: control 31284, MSP 31280, HMP 31281, motion 31282, pilot 31283.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGES=${1:-A B C D E F G}
export QEMU=${QEMU:-$HOME/flightbench-spike/qemu-baro/build/qemu-system-arm}
BASE_QEMU=${BASE_QEMU:-}
BASE_TREE=${BASE_TREE:-}
export CTL_PORT=31284 UART_PORT=31280 MON_PORT=31281 MOTION_PORT=31282 PILOT_PORT=31283
export ROOT
TMP=$(mktemp -d /tmp/baro-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT
stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }
PY="python3 $HERE/baro_evidence.py"

# session copy on our ports, with a python edit of d (the session dict): sess <name> <src> ['<python>']
sess() {
  python3 - "$2" "$TMP/$1.json" "${3:-}" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["ports"] = {"control": 31284, "msp": 31280}
exec(sys.argv[3])
json.dump(d, open(sys.argv[2], "w"))
EOF
  echo "$TMP/$1.json"
}
start_session() {   # start_session <session> [MACHINE_OPTS] [run.sh dir]
  MACHINE_OPTS=${2:-} "${3:-$ROOT}/core/qemu/run.sh" --session "$1" >"$TMP/core.log" 2>&1 &
  QPID=$!
  for _ in $(seq 100); do python3 -c "import socket; socket.create_connection(('127.0.0.1', $CTL_PORT))" 2>/dev/null && break; sleep 0.1; done
}
S=$HERE/../sessions
DETACH='d["wiring"].append({"device":"dps310","detached":True})'
echo "# baro evidence, $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$ROOT" || echo +dirty); load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "# QEMU=$QEMU${BASE_QEMU:+; base QEMU=$BASE_QEMU, base tree $BASE_TREE}"

# flight <name> <session> <csv|quiet> [qemu] [tree]: liftoff plan to 62 s, FDM log every 20 ms;
# csv: flight.py MSP samples + baro_evidence.py record next to it, flight_report.py
flight() {
  local name=$1 opts="fdm-log=$TMP/$1-fdm.csv,fdm-log-ms=20"
  QEMU=${4:-$QEMU} start_session "$2" "$opts" "${5:-$ROOT}"
  local start=$(date +%s.%N) rec=
  if [ "$3" = quiet ]; then
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --quiet | grep speed:
  else
    if [ -z "${4:-}" ]; then
      $PY record --port $CTL_PORT --out "$TMP/$name-baro.csv" --until 62 & rec=$!
    fi
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --csv "$TMP/$name-msp.csv" | grep speed:
    [ -n "$rec" ] && wait $rec
  fi
  echo "host time for 62 virtual s (boot + flight): $(echo "$(date +%s.%N) - $start" | bc | cut -c1-6) s"
  [ "$3" = csv ] && python3 "$HERE/flight_report.py" "$TMP/$name-fdm.csv" "$TMP/$name-msp.csv" | grep -v '^$'
}

if [[ $STAGES == *A* ]]; then
  echo; echo "## A. unit tests, manifest"
  make -s -C "$ROOT/devices" test 2>&1 | grep -E "^OK|passed|FAIL|failure"
  make -s -C "$ROOT/fdm" test 2>&1 | grep -E "OK|passed|FAIL|failure" | tail -3
  python3 "$HERE/test_wiring.py" 2>&1 | tail -1
  make -s -C "$ROOT/core/qemu/tests" test 2>&1 | tail -1
  QEMU=/nonexistent "$ROOT/core/qemu/run.sh" --manifest | python3 -c "
import json, sys
m = json.load(sys.stdin)
d = {x['id']: x for x in m['devices']}['dps310']
p = {x['id']: x for x in m['boards'][0]['pads']}['i2c3']
print('manifest dps310:', json.dumps(d))
print('manifest pad i2c3:', json.dumps(p))"
fi

if [[ $STAGES == *B* ]]; then
  echo; echo "## B. boot on the pad (rs7-pads.json: the onboard baro stays attached)"
  f=$(sess b "$S/rs7-pads.json")
  echo "machine: $(python3 "$HERE/session.py" "$f" "$TMP" 2>/dev/null | grep -o 'baro=[a-z]*')"
  start_session "$f"
  $PY pad --port $CTL_PORT --until 17
  python3 "$HERE/hmp.py" $MON_PORT "qom-get /machine bench" 2>/dev/null | grep -o 'baro=.*' | head -1
  stop_core
  grep -E "I2C|i2c|baro" "$TMP/core.log" | head -5
fi

if [[ $STAGES == *C* ]]; then
  echo; echo "## C. lift-off plan: flight unchanged, baro altitude follows the FDM"
  if [ -n "$BASE_QEMU" ]; then
    echo "### base build (main, I2C3 a storage stub: no baro), rs7-pads.json"
    flight base "$(sess base "$BASE_TREE/core/qemu/sessions/rs7-pads.json")" csv "$BASE_QEMU" "$BASE_TREE"
    stop_core
  fi
  echo "### this build, rs7-pads.json (baro on)"
  flight baro "$(sess baro "$S/rs7-pads.json")" csv
  echo "### t / fdm_alt / fc_baro_alt (MSP_ALTITUDE = the firmware's baro altitude), every 1 s from lift-off"
  $PY table "$TMP/baro-baro.csv" --from 33 --to 62 --every 1
  echo "### on the pad before arming (t 12..14) and after lift-off in the climb (t 38..62)"
  $PY table "$TMP/baro-baro.csv" --from 12 --to 14 --every 1
  $PY table "$TMP/baro-baro.csv" --from 38 --to 62 --every 99
  echo "### after the flight: disarm, CLI status"
  $PY cli --port $CTL_PORT status
  stop_core
fi

if [[ $STAGES == *D* ]]; then
  echo; echo "## D. baro detached ({\"device\":\"dps310\",\"detached\":true})"
  f=$(sess d "$S/rs7-pads.json" "$DETACH")
  echo "machine: $(python3 "$HERE/session.py" "$f" "$TMP" 2>/dev/null | grep -o 'baro=[a-z]*')"
  start_session "$f"
  $PY notwired --port $CTL_PORT
  $PY pad --port $CTL_PORT --until 12 | grep -v "^t="
  stop_core
  grep -E "I2C|i2c" "$TMP/core.log" | head -5
  echo "### the lift-off plan"
  flight det "$(sess det "$S/rs7-pads.json" "$DETACH")" csv
  echo "### after the flight: disarm, CLI status (I2C errors not growing)"
  $PY cli --port $CTL_PORT status
  stop_core
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. dps310.freeze in a climb (liftoff plan + collective 1660 at 48..56 s; freeze 50..54 s)"
  start_session "$(sess e "$S/rs7-pads.json")"
  $PY freeze --port $CTL_PORT --pilot $PILOT_PORT --until 62 --freeze-on 50 --freeze-off 54
  stop_core
  echo "### freeze across reset (on the pad: freeze, MCU reset, power cycle, off; bench baro_regs = the chip's PSR/TMP bytes)"
  start_session "$(sess e2 "$S/rs7-pads.json")"
  $PY freezereset --port $CTL_PORT --mon $MON_PORT
  stop_core
fi

if [[ $STAGES == *F* ]]; then
  echo; echo "## F. speed in flight (flight.py --quiet: host s per 10 virtual s from 40 s; idle skip on as in the session)"
  for i in 1 2; do
    echo "### baro on, run $i"; flight fon$i "$(sess fon$i "$S/rs7-pads.json")" quiet; stop_core
    echo "### baro detached, run $i"; flight foff$i "$(sess foff$i "$S/rs7-pads.json" "$DETACH")" quiet; stop_core
  done
  for n in fon1 fon2 foff1 foff2; do
    awk -F, 'NR == 1 || $1 <= 62.0' "$TMP/$n-fdm.csv" | cut -d, -f1-16 > "$TMP/$n.cut"
    echo "$n FDM log to 62 s: $(md5sum < "$TMP/$n.cut" | cut -c1-12), $(wc -l < "$TMP/$n.cut") rows"
  done
  for n in fon1 fon2; do
    awk -F, 'NR == 1 || $1 <= 62.0' "$TMP/$n-fdm.csv" | cut -d, -f1,21,22 > "$TMP/$n.baro"
    echo "$n baro trace to 62 s (t, baro_reg_pa, fc_baro_alt): $(md5sum < "$TMP/$n.baro" | cut -c1-12)," \
      "$(awk -F, 'NR > 1 && $3 != "nan"' "$TMP/$n.baro" | wc -l) rows with fc_baro_alt," \
      "$(awk -F, 'NR > 1 && $2 != "nan"' "$TMP/$n.baro" | cut -d, -f2 | sort -u | wc -l) distinct pressures;" \
      "fc_baro_alt t 40..62: $(awk -F, 'NR > 1 && $1 >= 40 && $3 != "nan" { if (n++ == 0 || $3 < lo) lo = $3; if (n == 1 || $3 > hi) hi = $3 } END { print lo ".." hi " m" }' "$TMP/$n.baro")"
  done
  cmp -s "$TMP/fon1.baro" "$TMP/fon2.baro" && echo "baro trace: identical between the two runs" || echo "baro trace: DIFFERS between the two runs"

fi

if [[ $STAGES == *G* ]]; then
  echo; echo "## G. ctl_evidence.py and fault_evidence.py (RS7 session, baro on)"
  start_session "$(sess g1 "$S/rs7.json")"
  python3 "$HERE/ctl_evidence.py" --port $CTL_PORT --msp $UART_PORT 2>&1 | tail -40
  stop_core
  start_session "$(sess g2 "$S/rs7.json")"
  python3 "$HERE/fault_evidence.py" --port $CTL_PORT --msp $UART_PORT --mon $MON_PORT 2>&1 | grep -E "PASS|FAIL|SUMMARY|dps310|1c|1d"
  stop_core
fi
