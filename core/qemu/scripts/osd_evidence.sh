#!/bin/bash
# OSD evidence (MSP DisplayPort OSD device msp_osd on PORT-A = uart4, scripts/osd_evidence.py): prints the log.
#   A  unit tests (devices incl. test_msp_osd, fdm, wiring, core/qemu/tests) and the manifest's msp_osd entry
#   B  boot with the device: session check (machine option, CLI lines), `osd` messages to 12 s (first full,
#      deltas, rate), canvas as text art, CLI serial/get/tasks, MSP_OSD_CANVAS, bench counters
#   C  lift-off plan with the device (flight.py --csv + flight_report.py; osd_evidence.py flight listens):
#      canvas before/after arming and in the hover, the element regions over time, rate while flying
#   D  reset firmware -> the first frame after the reboot is full (clear:true); msp_osd.link_lost on/off;
#      subscribe osd off/on
#   E  without the device: no `osd` messages, fault -> not wired, CLI status; lift-off plan (this build
#      and the base build = main): flight unchanged
#   F  speed in flight (flight.py --quiet: host s per 10 virtual s from 40 s), with vs without, 2 runs each
#   G  ctl_evidence.py and fault_evidence.py on the RS7 session
#   H  review fixes: the picture-gone message (osd link:false) on power off, in CLI mode (then a full
#      frame after exit), with msp_osd.link_lost; osd_framerate_hz = 2: no contact flapping
# usage: osd_evidence.sh [stages, default "A B C D E F G H"]
#   QEMU=<this build> BASE_QEMU=<base build> BASE_TREE=<base checkout> (E compares against it)
# Everything on its own core: control 31284, MSP 31280, HMP 31281, motion 31282, pilot 31283.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGES=${1:-A B C D E F G H}
export QEMU=${QEMU:-$HOME/flightbench-spike/qemu-osd/build/qemu-system-arm}
BASE_QEMU=${BASE_QEMU:-}
BASE_TREE=${BASE_TREE:-}
export CTL_PORT=31284 UART_PORT=31280 MON_PORT=31281 MOTION_PORT=31282 PILOT_PORT=31283
export ROOT
TMP=$(mktemp -d /tmp/osd-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT
stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }
PY="python3 $HERE/osd_evidence.py"

# session copy on our ports, with a python edit of d (the session dict): sess <name> <src> ['<python>']
sess() {
  python3 - "$2" "$TMP/$1.json" "${3:-}" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["ports"] = {"control": 31284, "msp": 31280}
exec(sys.argv[3])
json.dump(d, open(sys.argv[2], "w"))
PYEOF
  echo "$TMP/$1.json"
}
start_session() {   # start_session <session> [MACHINE_OPTS] [run.sh dir]
  MACHINE_OPTS=${2:-} "${3:-$ROOT}/core/qemu/run.sh" --session "$1" >"$TMP/core.log" 2>&1 &
  QPID=$!
  for _ in $(seq 100); do python3 -c "import socket; socket.create_connection(('127.0.0.1', $CTL_PORT))" 2>/dev/null && break; sleep 0.1; done
}
S=$HERE/../sessions
OSD='d["wiring"].append({"device":"msp_osd","port":"uart","pad":"PORT-A"})'
echo "# osd evidence, $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$ROOT" || echo +dirty); load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "# QEMU=$QEMU${BASE_QEMU:+; base QEMU=$BASE_QEMU, base tree $BASE_TREE}"

# flight <name> <session> <csv|quiet> [qemu] [tree] [osd]: liftoff plan to 62 s, FDM log every 20 ms;
# csv: flight.py MSP samples + flight_report.py; osd: osd_evidence.py flight listens next to it
flight() {
  local name=$1 opts="fdm-log=$TMP/$1-fdm.csv,fdm-log-ms=20" lp=
  QEMU=${4:-$QEMU} start_session "$2" "$opts" "${5:-$ROOT}"
  local start=$(date +%s.%N)
  if [ "${6:-}" = osd ]; then
    $PY flight --port $CTL_PORT --until 62 >"$TMP/$name-osd.txt" 2>&1 &
    lp=$!
  fi
  if [ "$3" = quiet ]; then
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --quiet | grep speed:
  else
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --csv "$TMP/$name-msp.csv" | grep speed:
  fi
  echo "host time for 62 virtual s (boot + flight): $(echo "$(date +%s.%N) - $start" | bc | cut -c1-6) s"
  [ "$3" = csv ] && python3 "$HERE/flight_report.py" "$TMP/$name-fdm.csv" "$TMP/$name-msp.csv" | grep -v '^$'
  if [ -n "$lp" ]; then
    wait $lp
    cat "$TMP/$name-osd.txt"
  fi
}

cli_show() {   # cli_show <cmd...>: CLI lines of interest (the CLI's exit reboots)
  python3 - "$CTL_PORT" "$@" <<'PYEOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(sys.argv[1]))
for cmd in sys.argv[2:]:
    for line in c.cli(cmd):
        if any(k in line for k in ("Arming", "CPU:", "serial ", "rror", "GYRO=", "OSD", "displayport", "vcd")):
            print(f"  cli {cmd}: {line.rstrip()}")
PYEOF
}

if [[ $STAGES == *A* ]]; then
  echo; echo "## A. unit tests, manifest"
  make -s -C "$ROOT/devices" test 2>&1 | grep -E "^OK|passed|FAIL|failure|test_msp_osd"
  make -s -C "$ROOT/fdm" test 2>&1 | grep -E "OK|passed|FAIL|failure" | tail -3
  python3 "$HERE/test_wiring.py" 2>&1 | tail -1
  make -s -C "$ROOT/core/qemu/tests" test 2>&1 | tail -1
  QEMU=/nonexistent "$ROOT/core/qemu/run.sh" --manifest | python3 -c "
import json, sys
m = json.load(sys.stdin)
d = {x['id']: x for x in m['devices']}['msp_osd']
print('manifest msp_osd:', json.dumps(d))
for pid in ('uart3', 'uart4', 'uart6'):
    p = {x['id']: x for x in m['boards'][0]['pads']}[pid]
    print(f'manifest pad {pid}:', json.dumps(p))"
fi

if [[ $STAGES == *B* ]]; then
  echo; echo "## B. boot with msp_osd on PORT-A (rs7-pads.json + {\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"PORT-A\"})"
  f=$(sess b "$S/rs7-pads.json" "$OSD")
  python3 "$HERE/session.py" --check "$f" 2>/dev/null | grep -E "msp_osd|machine|config"
  start_session "$f"
  $PY boot --port $CTL_PORT --until 12 --msp $UART_PORT --cli
  python3 "$HERE/hmp.py" $MON_PORT "qom-get /machine bench" 2>/dev/null | grep -o 'osd=[^"]*' | head -1
  stop_core
  grep -iE "osd|error" "$TMP/core.log" | head -5
fi

if [[ $STAGES == *C* ]]; then
  echo; echo "## C. lift-off plan with the device"
  flight osd "$(sess c "$S/rs7-pads.json" "$OSD")" csv "" "" osd
  stop_core
fi

if [[ $STAGES == *D* ]]; then
  echo; echo "## D. reset firmware, msp_osd.link_lost, subscribe"
  start_session "$(sess d "$S/rs7-pads.json" "$OSD")"
  python3 -c "import sys; sys.path.insert(0, '$ROOT/tools'); from core_client import Core; c = Core($CTL_PORT); c.wait_t(8)"
  echo "### reset firmware at t ~8"
  $PY reset --port $CTL_PORT
  echo "### msp_osd.link_lost 1 s"
  $PY fault --port $CTL_PORT
  echo "### subscribe osd false / true"
  $PY sub --port $CTL_PORT
  stop_core
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. without the device"
  f=$(sess e "$S/rs7-pads.json")
  echo "machine: $(python3 "$HERE/session.py" "$f" "$TMP" 2>/dev/null | grep -o "SESSION_OPTS=.*")"
  start_session "$f"
  $PY none --port $CTL_PORT --until 10
  python3 "$HERE/hmp.py" $MON_PORT "qom-get /machine bench" 2>/dev/null | grep -o 'osd=[^"]*' | head -1
  cli_show status serial "get osd_displayport_device" "get displayport_msp_serial"
  stop_core
  if [ -n "$BASE_QEMU" ]; then
    echo "### base build (main), rs7-pads.json"
    flight base "$(sess base "$BASE_TREE/core/qemu/sessions/rs7-pads.json")" csv "$BASE_QEMU" "$BASE_TREE"
    stop_core
  fi
  echo "### this build, rs7-pads.json (no OSD device)"
  flight nosd "$(sess nosd "$S/rs7-pads.json")" csv
  stop_core
fi

if [[ $STAGES == *F* ]]; then
  echo; echo "## F. speed in flight (flight.py --quiet: host s per 10 virtual s from 40 s; idle skip on as in the session)"
  for i in 1 2; do
    echo "### with msp_osd, run $i"; flight fon$i "$(sess fon$i "$S/rs7-pads.json" "$OSD")" quiet; stop_core
    echo "### without, run $i"; flight foff$i "$(sess foff$i "$S/rs7-pads.json")" quiet; stop_core
  done
  for n in fon1 fon2 foff1 foff2; do
    awk -F, 'NR == 1 || $1 <= 62.0' "$TMP/$n-fdm.csv" | cut -d, -f1-16 > "$TMP/$n.cut"
    echo "$n FDM log to 62 s: $(md5sum < "$TMP/$n.cut" | cut -c1-12), $(wc -l < "$TMP/$n.cut") rows"
  done
fi

if [[ $STAGES == *G* ]]; then
  echo; echo "## G. ctl_evidence.py and fault_evidence.py (RS7 session)"
  start_session "$(sess g1 "$S/rs7.json")"
  python3 "$HERE/ctl_evidence.py" --port $CTL_PORT --msp $UART_PORT 2>&1 | tail -40
  stop_core
  start_session "$(sess g2 "$S/rs7.json")"
  python3 "$HERE/fault_evidence.py" --port $CTL_PORT --msp $UART_PORT --mon $MON_PORT 2>&1 | grep -E "PASS|FAIL|SUMMARY|msp_osd|1c|1d"
  stop_core
fi

if [[ $STAGES == *H* ]]; then
  echo; echo "## H. review fixes: link:false (power off, CLI mode, link_lost), osd_framerate_hz = 2"
  f=$(sess h "$S/rs7-pads.json" "$OSD")
  python3 "$HERE/session.py" --check "$f" 2>/dev/null | grep -E "osd_displayport_device"
  start_session "$f"
  python3 -c "import sys; sys.path.insert(0, '$ROOT/tools'); from core_client import Core; c = Core($CTL_PORT); c.wait_t(8)"
  echo "### power off 3 s (host), power on"
  $PY power --port $CTL_PORT
  echo "### CLI mode, then exit"
  $PY cli --port $CTL_PORT
  echo "### msp_osd.link_lost 1 s"
  $PY fault --port $CTL_PORT
  stop_core
  echo "### osd_framerate_hz = 2 (profile_text), 8..30 s"
  f=$(sess h2 "$S/rs7-pads.json" "$OSD; d['profile_text'] = 'set osd_framerate_hz = 2'")
  start_session "$f"
  $PY rate2 --port $CTL_PORT --mon $MON_PORT --from-t 8 --until 30
  stop_core
fi
