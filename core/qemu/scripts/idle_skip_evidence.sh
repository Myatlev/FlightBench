#!/bin/bash
# Idle skip ("external WFI", -M nexus-xr,idle-skip=on) evidence: off vs on for
#   A  speed, idle firmware on the pad (speed.py through run.sh --session; + halt mode, + bare no-gyro)
#   B  speed in flight (flight.py --quiet, liftoff plan, host s over virtual 40..62)
#   C  firmware health: CLI tasks + status on the pad
#   D  flight equivalence (flight.py --csv + flight_report.py) and determinism (two quiet runs:
#      FDM log identical: 20 ms sampling, printed precision), ground_evidence.py
#   E  realtime throttle with the skip on (realtime_evidence.py, icount shift=3)
#   F  MSP over the host socket while the skip is on (uart_evidence.py)
# Everything on its own core: control 31274, MSP 31270, HMP 31271, motion 31272, pilot 31273.
# usage: idle_skip_evidence.sh <session.json with those ports> [stages, default "A B C D E F"]
#   QEMU=<qemu-system-arm> (default ~/flightbench-spike/qemu/build/qemu-system-arm)
# Prints the log on stdout.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SESSION=${1:?usage: idle_skip_evidence.sh <session.json> [stages]}
STAGES=${2:-A B C D E F}
export QEMU=${QEMU:-$HOME/flightbench-spike/qemu/build/qemu-system-arm}
export PATH=$PATH:$HOME/rotorflight/firmware/tools/gcc-arm-none-eabi-9-2020-q2-update/bin
export CTL_PORT=31274 UART_PORT=31270 MON_PORT=31271 MOTION_PORT=31272 PILOT_PORT=31273
ELF=$(python3 -c "import json,os,sys; print(os.path.expanduser(json.load(open(sys.argv[1]))['firmware']))" "$SESSION")
TMP=$(mktemp -d /tmp/idle-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT

stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }
# session copy with time.* overrides: sess <name> '<json fragment for "time">'
sess() {
  python3 - "$SESSION" "$TMP/$1.json" "$2" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1])); d["time"].update(json.loads(sys.argv[3]))
json.dump(d, open(sys.argv[2], "w"))
EOF
  echo "$TMP/$1.json"
}
start_session() {   # start_session <session file> [MACHINE_OPTS]
  MACHINE_OPTS=${2:-} "$HERE/../run.sh" --session "$1" >"$TMP/core.log" 2>&1 &
  QPID=$!
  for _ in $(seq 100); do python3 -c "import socket; socket.create_connection(('127.0.0.1', $CTL_PORT))" 2>/dev/null && break; sleep 0.1; done
}
wait_virtual() {    # wait_virtual <seconds>: until the core's virtual clock passes it
  python3 - "$1" <<'EOF'
import os, sys, time
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"])); c.wait_t(float(sys.argv[1]))
EOF
}
bench() { python3 "$HERE/hmp.py" $MON_PORT "qom-get /machine bench" | grep -o 'idle_skips=[0-9]* idle_skipped_s=[0-9.]*'; }
export ROOT

echo "# idle skip evidence, $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$HERE/.." || echo +dirty); $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'); load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "# QEMU=$QEMU; session $SESSION (firmware $ELF)"

if [[ $STAGES == *A* ]]; then
  echo; echo "## A. speed, idle firmware on the pad, disarmed (speed.py: host s for 10 virtual s of sysTickUptime after 5 s)"
  for m in off on halt; do
    case $m in off) S=$(sess a-off '{"icount_shift":2,"idle_skip":false}'); O= ;;
               on) S=$(sess a-on '{"icount_shift":2,"idle_skip":true}'); O= ;;
               halt) S=$(sess a-on '{"icount_shift":2,"idle_skip":true}'); O=idle-skip-mode=halt ;; esac
    for r in 1 2; do
      python3 "$HERE/speed.py" --qemu "$QEMU" --elf "$ELF" --session "$S" --port $MON_PORT --warmup 5000 ${O:+--machine-opts "$O"}
      [ $m = halt ] && break
    done
  done
  echo "### bare firmware (no board config: NOGYRO, no FDM/receiver/control server), like README 'Measurements'"
  for m in "" ",idle-skip=on"; do
    python3 "$HERE/speed.py" --qemu "$QEMU" --elf "$ELF" --icount shift=2,sleep=off --port $MON_PORT --machine "nexus-xr$m"
  done
fi

if [[ $STAGES == *C* ]]; then
  echo; echo "## C. firmware health on the pad (disarmed, t > 15 virtual s): CLI tasks + status"
  for m in off on; do
    S=$(sess c-$m "{\"icount_shift\":2,\"idle_skip\":$([ $m = on ] && echo true || echo false)}")
    start_session "$S"
    wait_virtual 15
    echo "### idle_skip $m ($(bench))"
    CLI_DRAIN=3 python3 "$HERE/cli.py" $UART_PORT 3 tasks status 2>&1 | grep -v '^\s*$'
    stop_core
  done
fi

fdm_summary() {   # FDM log only (quiet runs): lift-off, airborne attitude/head speed maxima, crashes
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

flight() {   # flight <name> <off|on> <quiet|csv>
  local name=$1 skip=$2 mode=$3 opts="fdm=on,fdm-log=$TMP/$1-fdm.csv,fdm-log-ms=20"
  [ "$skip" = on ] && opts+=",idle-skip=on"
  CTL_PORT=$CTL_PORT PROFILE=$ROOT/models/rs7/qemu-profile.txt MACHINE_OPTS="$opts" \
    "$HERE/../run.sh" "$ELF" -icount shift=2,sleep=off >"$TMP/core.log" 2>&1 &
  QPID=$!
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
  fi
}

if [[ $STAGES == *B* ]]; then
  echo; echo "## B. speed in flight (liftoff plan: armed 14 s, governor 1800, airborne from ~34 s; host s over virtual 40..62, flight.py --quiet)"
  for m in off on; do
    echo "### idle_skip $m"
    flight b-$m $m quiet
  done
fi

if [[ $STAGES == *D* ]]; then
  echo; echo "## D. flight equivalence (flight.py --csv, MSP polled every 0.25 host s) and determinism"
  for r in off on-1 on-2; do
    echo "### idle_skip ${r%-*} (run $r)"
    flight d-$r ${r%-*} csv
  done
  echo "### determinism: two quiet runs each (no host polling; FDM log every 20 ms, compared up to t = 62.0 s)"
  for m in on off; do
    flight det-$m-1 $m quiet >/dev/null
    flight det-$m-2 $m quiet >/dev/null
    for r in 1 2; do awk -F, 'NR == 1 || $1 <= 62.0' "$TMP/det-$m-$r-fdm.csv" > "$TMP/det-$m-$r.cut"; done
    echo "idle_skip $m: $(md5sum < "$TMP/det-$m-1.cut" | cut -c1-12) vs $(md5sum < "$TMP/det-$m-2.cut" | cut -c1-12), $(wc -l < "$TMP/det-$m-1.cut") rows: $(cmp -s "$TMP/det-$m-1.cut" "$TMP/det-$m-2.cut" && echo identical || echo DIFFERENT)"
  done
  cmp -s "$TMP/det-on-1.cut" "$TMP/det-off-1.cut" && echo "on vs off: identical" ||     echo "on vs off: DIFFERENT (first differing row: $(cmp "$TMP/det-on-1.cut" "$TMP/det-off-1.cut" | grep -o 'line [0-9]*')); not guaranteed -- a task due by time alone may start up to idle-skip-gran-us later with the skip -- but identical on 2026-09-26, so find out why"
  echo "### ground_evidence.py with the skip on (session, 2 runs)"
  for r in 1 2; do
    start_session "$(sess d-ground '{"icount_shift":2,"idle_skip":true}')"
    python3 "$HERE/ground_evidence.py" --port $CTL_PORT 2>&1 | grep -E 'CHECK|event crash|reboot' | head -20
    echo "-- $(bench)"
    stop_core
  done
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. realtime throttle with the skip on (realtime_evidence.py)"
  for sh in 3 2 nogyro; do
    if [ $sh = nogyro ]; then
      echo "### icount shift=2, idle_skip on, no gyro wired (NOGYRO: the machine outruns the wall clock)"
      S=$(sess e-nogyro '{"icount_shift":2,"idle_skip":true}')
      python3 -c "import json,sys; d=json.load(open(sys.argv[1])); d['wiring']=[w for w in d['wiring'] if w['device']!='icm42688p']; json.dump(d,open(sys.argv[1],'w'))" "$S"
    else
      echo "### icount shift=$sh, idle_skip on"
      S=$(sess e-$sh "{\"icount_shift\":$sh,\"idle_skip\":true}")
    fi
    start_session "$S"
    wait_virtual 6
    timeout 200 python3 "$HERE/realtime_evidence.py" $CTL_PORT $PILOT_PORT 2>&1 | grep -E 'realtime=|hello' | head -9
    echo "-- $(bench)"
    stop_core
  done
fi

if [[ $STAGES == *F* ]]; then
  echo; echo "## F. MSP over the host socket (USART1, :31270) while the skip is on/off (uart_evidence.py)"
  for m in off on; do
    echo "### idle_skip $m"
    start_session "$(sess f-$m "{\"icount_shift\":2,\"idle_skip\":$([ $m = on ] && echo true || echo false)}")"
    timeout 300 python3 "$HERE/uart_evidence.py" $CTL_PORT $UART_PORT 2>&1 | grep -v '^$'
    echo "-- $(bench)"
    stop_core
  done
fi
