#!/bin/bash
# icount shift with the idle skip on: does a slower virtual CPU buy speed without changing the
# firmware's behaviour? RS7 session (time.idle_skip true) at icount_shift 1, 2, 3 (500/250/125 MIPS):
#   A  idle on the pad, disarmed (speed.py --session, two runs each)
#   B  in flight (flight.py --quiet, liftoff plan, host s over virtual 40..62) + FDM summary
#   C  firmware health on the pad: CLI tasks + status
#   D  flight equivalence: flight.py --csv + flight_report.py (MSP polled), one run each
#   E  realtime:true at shift 3 (realtime_evidence.py)
# Everything on its own core: control 31274, MSP 31270, HMP 31271, motion 31272, pilot 31273.
# usage: shift_evidence.sh [session.json, default sessions/rs7.json] [stages, default "A B C D E"] [shifts, default "1 2 3"]
#   QEMU=<qemu-system-arm> (default ~/flightbench-spike/qemu/build/qemu-system-arm)
# Prints the log on stdout.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SESSION=${1:-$HERE/../sessions/rs7.json}
STAGES=${2:-A B C D E}
SHIFTS=${3:-1 2 3}
export QEMU=${QEMU:-$HOME/flightbench-spike/qemu/build/qemu-system-arm}
export PATH=$PATH:$HOME/rotorflight/firmware/tools/gcc-arm-none-eabi-9-2020-q2-update/bin
export CTL_PORT=31274 UART_PORT=31270 MON_PORT=31271 MOTION_PORT=31272 PILOT_PORT=31273
ELF=$(python3 -c "import json,os,sys; print(os.path.expanduser(json.load(open(sys.argv[1]))['firmware']))" "$SESSION")
TMP=$(mktemp -d /tmp/shift-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT
export ROOT

stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }
# session copy with our ports and time.* overrides: sess <name> '<json fragment for "time">'
sess() {
  python3 - "$SESSION" "$TMP/$1.json" "$2" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1])); d["time"].update(json.loads(sys.argv[3]))
d["ports"] = {"control": 31274, "msp": 31270}
json.dump(d, open(sys.argv[2], "w"))
EOF
  echo "$TMP/$1.json"
}
start_session() {   # start_session <session file> [MACHINE_OPTS]
  MACHINE_OPTS=${2:-} "$HERE/../run.sh" --session "$1" >"$TMP/core.log" 2>&1 &
  QPID=$!
  for _ in $(seq 100); do python3 -c "import socket; socket.create_connection(('127.0.0.1', $CTL_PORT))" 2>/dev/null && break; sleep 0.1; done
}
wait_virtual() {
  python3 - "$1" <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"])); c.wait_t(float(sys.argv[1]))
EOF
}
bench() { python3 "$HERE/hmp.py" $MON_PORT "qom-get /machine bench" | grep -o 'idle_skips=[0-9]* idle_skipped_s=[0-9.]*'; }
mips() { echo $((1000 >> $1)); }

echo "# icount shift evidence (idle skip on), $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$HERE/.." || echo +dirty); $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'); load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "# QEMU=$QEMU; session $SESSION (firmware $ELF); shifts $SHIFTS"

if [[ $STAGES == *A* ]]; then
  echo; echo "## A. idle on the pad, disarmed (speed.py: host s for 10 virtual s of sysTickUptime after 5 s)"
  for sh in $SHIFTS; do
    echo "### shift $sh ($(mips $sh) MIPS)"
    S=$(sess a-$sh "{\"icount_shift\":$sh,\"idle_skip\":true}")
    for r in 1 2; do
      python3 "$HERE/speed.py" --qemu "$QEMU" --elf "$ELF" --session "$S" --port $MON_PORT --warmup 5000
    done
  done
fi

if [[ $STAGES == *C* ]]; then
  echo; echo "## C. firmware health on the pad (disarmed, t > 15 virtual s): CLI tasks + status"
  for sh in $SHIFTS; do
    start_session "$(sess c-$sh "{\"icount_shift\":$sh,\"idle_skip\":true}")"
    wait_virtual 15
    echo "### shift $sh ($(mips $sh) MIPS; $(bench))"
    CLI_DRAIN=3 python3 "$HERE/cli.py" $UART_PORT 3 tasks status 2>&1 | grep -v '^\s*$'
    stop_core
  done
fi

fdm_summary() {   # FDM log only: lift-off, airborne attitude/head speed maxima
  python3 - "$1" <<'EOF2'
import csv, sys
r = [{k: float(v) for k, v in x.items()} for x in csv.DictReader(open(sys.argv[1]))]
air = [x for x in r if x["on_ground"] == 0]
if not air:
    print("never airborne"); sys.exit()
lo = air[0]["t"]; touch = [x["t"] for x in r if x["t"] > lo and x["on_ground"] == 1]
print(f"lift-off at t={lo:.2f} s; {'touched down at %.2f' % touch[0] if touch else 'never touched down'}; "
      f"alt max {max(x['alt'] for x in r):.2f} m, end {r[-1]['alt']:.2f} m (t={r[-1]['t']:.2f})")
print(f"FDM airborne: |roll| max {max(abs(x['roll']) for x in air):.2f}, |pitch| max {max(abs(x['pitch']) for x in air):.2f} deg; "
      f"head speed {min(x['head_rpm'] for x in air):.0f}..{max(x['head_rpm'] for x in air):.0f} rpm")
EOF2
}

flight() {   # flight <name> <shift> <quiet|csv>
  local name=$1 sh=$2 mode=$3
  start_session "$(sess $name "{\"icount_shift\":$sh,\"idle_skip\":true}")" "fdm-log=$TMP/$name-fdm.csv,fdm-log-ms=20"
  local start=$(date +%s.%N)
  if [ "$mode" = quiet ]; then
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --quiet | grep speed:
  else
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --csv "$TMP/$name-msp.csv" | grep speed:
  fi
  echo "host time for 62 virtual s (boot + flight): $(echo "$(date +%s.%N) - $start" | bc | cut -c1-6) s; $(bench)"
  stop_core
  if [ "$mode" = csv ]; then
    python3 "$HERE/flight_report.py" "$TMP/$name-fdm.csv" "$TMP/$name-msp.csv" | grep -v '^$'
  else
    fdm_summary "$TMP/$name-fdm.csv"
    awk -F, 'NR == 1 || $1 <= 62.0' "$TMP/$name-fdm.csv" > "$TMP/$name.cut"
    echo "FDM log to t = 62.0 s: md5 $(md5sum < "$TMP/$name.cut" | cut -c1-12), $(wc -l < "$TMP/$name.cut") rows"
  fi
}

if [[ $STAGES == *B* ]]; then
  echo; echo "## B. in flight (liftoff plan: armed 14 s, governor 1800, airborne from ~34 s; host s over virtual 40..62, flight.py --quiet)"
  for sh in $SHIFTS; do
    echo "### shift $sh ($(mips $sh) MIPS)"
    flight b-$sh $sh quiet
  done
fi

if [[ $STAGES == *D* ]]; then
  echo; echo "## D. flight equivalence (flight.py --csv, MSP polled every 0.25 host s; reference evidence/flight-2026-09-25.log)"
  for sh in $SHIFTS; do
    echo "### shift $sh ($(mips $sh) MIPS)"
    flight d-$sh $sh csv
  done
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. realtime:true at shift 3 (realtime_evidence.py; the realtime=True speed=1.0 lines are the answer)"
  start_session "$(sess e-3 '{"icount_shift":3,"idle_skip":true,"realtime":true}')"
  wait_virtual 6
  timeout 200 python3 "$HERE/realtime_evidence.py" $CTL_PORT $PILOT_PORT 2>&1 | grep -E 'realtime=|hello' | head -9
  echo "-- $(bench)"
  stop_core
fi
