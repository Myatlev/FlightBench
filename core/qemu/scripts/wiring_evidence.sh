#!/bin/bash
# Wiring model evidence (board x devices x assembly, scripts/wiring.py): prints the log.
#   A  manifest: pads/devices/ports, old fields, unit tests (test_wiring.py)
#   B  session validation: rs7.json (old form) and rs7-pads.json (by labels) accepted, rejections
#   C  flight equivalence with the RS7 session: flight.py --csv + flight_report.py (this build,
#      old and new form; the base build for comparison), determinism: two quiet runs, FDM log md5
#   D  servo2 reversed: the FDM gets the mirrored pulse, state.actuators agrees
#   E  no crsf wired: RXLOSS forever, crsf fault "not wired"; no esc: no DShot decode, motor 0,
#      esc fault "not wired"
#   F  ctl_evidence.py and fault_evidence.py on the RS7 session
#   G  review fixes: old-form defaults, rejections (session.py and the machine's own -M checks),
#      the app's smoke bench, unwired outputs null in state, IMU detached
# usage: wiring_evidence.sh [stages, default "A B C D E F G"]
#   QEMU=<this build> BASE_QEMU=<base build> BASE_TREE=<base checkout> (C compares against it)
# Everything on its own core: control 31274, MSP 31270, HMP 31271, motion 31272, pilot 31273.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGES=${1:-A B C D E F G}
export QEMU=${QEMU:-$HOME/flightbench-spike/qemu-wiring2/build/qemu-system-arm}
BASE_QEMU=${BASE_QEMU:-}
BASE_TREE=${BASE_TREE:-}
export CTL_PORT=31274 UART_PORT=31270 MON_PORT=31271 MOTION_PORT=31272 PILOT_PORT=31273
export ROOT
TMP=$(mktemp -d /tmp/wiring-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT
stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }

# session copy on our ports, with a python edit of d (the session dict): sess <name> <src> ['<python>']
sess() {
  python3 - "$2" "$TMP/$1.json" "${3:-}" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["ports"] = {"control": 31274, "msp": 31270}
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
echo "# wiring evidence, $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$ROOT" || echo +dirty); load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "# QEMU=$QEMU${BASE_QEMU:+; base QEMU=$BASE_QEMU, base tree $BASE_TREE}"

if [[ $STAGES == *A* ]]; then
  echo; echo "## A. manifest (run.sh --manifest) and unit tests"
  QEMU=/nonexistent "$ROOT/core/qemu/run.sh" --manifest > "$TMP/manifest.json"
  python3 - "$TMP/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
print(f"valid JSON, {len(open(sys.argv[1]).read())} bytes; top-level keys {list(m)}")
b = m["boards"][0]
print(f"board {b['id']} ({b['name']}, {b['mcu']}): {len(b['slots'])} old slots: " + " ".join(f"{s['id']}:{s['kind']}" for s in b["slots"]))
print(f"{len(b['pads'])} pads:")
for p in b["pads"]:
    extra = " ".join(f"{k}={p[k]}" for k in ("timer", "dma", "bus") if k in p)
    ob = f" ONBOARD {p['device']} ({p.get('part')}) detachable={p['detachable']}" if p["onboard"] else ""
    print(f"  {p['id']:7s} label={str(p['label']):7s} {p['kind']:7s} flags={','.join(p['flags']) or '-':10s} "
          f"pins={p.get('pins', {})} {extra}{ob}  machine models: {','.join(p.get('devices', [])) or '-'}")
for d in m["devices"]:
    print(f"  device {d['id']:9s} kind={d['kind']:6s} implemented={d['implemented']!s:5s} ports: " +
          ", ".join(f"{q['id']}({q['kind']},{q['dir']}{',optional' if q.get('optional') else ''}{',bundled' if q.get('bundled') else ''})" for q in d["ports"]))
a = m["aircraft"][0]
print(f"aircraft {a['id']}: actuators {a['actuators']}, fdm_inputs {a.get('fdm_inputs')}, gear {a.get('transmission')}, esc {a.get('esc')}")
EOF
  echo "### unit tests (test_wiring.py; the pad table vs the 13 slots the app derives, sessions)"
  python3 "$HERE/test_wiring.py" -v 2>&1 | grep -E "ok$|FAIL|ERROR|^Ran|^OK"
  echo "### devices/ unit tests"
  make -s -C "$ROOT/devices" test 2>&1 | grep -E "passed|failure"
fi

if [[ $STAGES == *B* ]]; then
  echo; echo "## B. session validation (session.py --check)"
  for f in rs7.json rs7-pads.json; do
    echo "### $f"; python3 "$HERE/session.py" --check "$S/$f" 2>&1
  done
  bad() {  # bad <label> <python edit of d>
    local f; f=$(sess "bad" "$S/rs7-pads.json" "$2")
    echo "### reject: $1"
    python3 "$HERE/session.py" --check "$f" 2>&1; echo "exit $?"
  }
  bad "unknown pad" 'd["wiring"][3]["pad"]="S9"'
  bad "servo port on a uart pad" 'd["wiring"][3]["pad"]="PORT-A"'
  bad "removing a soldered onboard device (blackbox flash; the baro is detachable since the baro branch)" 'd["wiring"].append({"device":"spiflash","detached":True})'
  python3 - "$ROOT/boards/nexus-xr/board.json" "$TMP/board-gyro-soldered.json" <<'EOF'
import json, sys
b = json.load(open(sys.argv[1]))
for o in b["onboard"]:
    if o["device"] == "icm42688p": del o["detachable"]
json.dump(b, open(sys.argv[2], "w"))
EOF
  echo "### reject: removing the onboard gyro on a board.json without detachable (a copy with it deleted)"
  python3 - "$S/rs7-pads.json" "$TMP/board-gyro-soldered.json" <<'EOF' 2>&1
import json, os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "core", "qemu", "scripts"))
import session, wiring
s = json.load(open(sys.argv[1])); s["wiring"].append({"device": "icm42688p", "detached": True})
try:
    session.assembly(s, sys.argv[2]); print("accepted?!")
except wiring.WiringError as e:
    print(f"session: {e}")
EOF
  echo "### the real board.json (gyro detachable: true): the same removal is accepted"
  python3 "$HERE/session.py" --check "$(sess detach "$S/rs7-pads.json" 'd["wiring"].append({"device":"icm42688p","detached":True})')" 2>&1 | grep -E "icm|machine"
  bad "duplicate pad use" 'd["wiring"][4]["pad"]="servo1"'
  bad "second device on the onboard receiver's pad" 'd["wiring"].append({"device":"gps","pad":"uart5"})'
  echo "### accepted: two devices on the I2C bus (compass next to the onboard baro)"
  python3 "$HERE/session.py" --check "$(sess i2c "$S/rs7-pads.json" 'd["wiring"].append({"device":"compass","pad":"i2c3"})')" 2>&1 | grep -E "i2c3"
  bad "unknown device" 'd["wiring"].append({"device":"lidar","pad":"PORT-B"})'
  bad "receiver on the RX-only TELEM pad" 'd["wiring"].append({"device":"crsf","detached":True}); d["wiring"].append({"device":"crsf","pad":"TELEM"})'
fi

flight() {   # flight <name> <session> <quiet|csv> [qemu] [tree]
  local name=$1 opts="fdm-log=$TMP/$1-fdm.csv,fdm-log-ms=20"
  QEMU=${4:-$QEMU} start_session "$2" "$opts" "${5:-$ROOT}"
  local start=$(date +%s.%N)
  if [ "$3" = quiet ]; then
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --quiet | grep speed:
  else
    python3 "$HERE/flight.py" --plan liftoff --coll 1620 --until 62 --msp $UART_PORT --mon $MON_PORT \
      --pilot $PILOT_PORT --csv "$TMP/$name-msp.csv" | grep speed:
  fi
  echo "host time for 62 virtual s (boot + flight): $(echo "$(date +%s.%N) - $start" | bc | cut -c1-6) s"
  stop_core
  awk -F, 'NR == 1 || $1 <= 62.0' "$TMP/$name-fdm.csv" | cut -d, -f1-16 > "$TMP/$name.cut"
  [ "$3" = csv ] && python3 "$HERE/flight_report.py" "$TMP/$name-fdm.csv" "$TMP/$name-msp.csv" | grep -v '^$'
}

if [[ $STAGES == *C* ]]; then
  echo; echo "## C. flight equivalence with the RS7 session (liftoff plan, fdm-log every 20 ms)"
  if [ -n "$BASE_QEMU" ]; then
    echo "### base build (main, before the wiring model), rs7.json"
    flight base "$(sess base "$BASE_TREE/core/qemu/sessions/rs7.json")" csv "$BASE_QEMU" "$BASE_TREE"
  fi
  echo "### this build, rs7.json (old form)"
  flight new-old "$(sess new-old "$S/rs7.json")" csv
  echo "### this build, rs7-pads.json (new form, pads by label)"
  flight new-pads "$(sess new-pads "$S/rs7-pads.json")" csv
  echo "### determinism: two quiet runs (no host polling), FDM log (first 16 columns = the base log's) up to t = 62.0 s"
  flight q1 "$(sess q1 "$S/rs7.json")" quiet >/dev/null
  flight q2 "$(sess q2 "$S/rs7-pads.json")" quiet >/dev/null
  for n in q1 q2; do echo "$n: $(md5sum < "$TMP/$n.cut" | cut -c1-12), $(wc -l < "$TMP/$n.cut") rows"; done
  cmp -s "$TMP/q1.cut" "$TMP/q2.cut" && echo "rs7.json vs rs7-pads.json quiet runs: identical" || echo "rs7.json vs rs7-pads.json: DIFFERENT"
  if [ -n "$BASE_QEMU" ]; then
    flight qb "$(sess qb "$BASE_TREE/core/qemu/sessions/rs7.json")" quiet "$BASE_QEMU" "$BASE_TREE" >/dev/null
    echo "base quiet run: $(md5sum < "$TMP/qb.cut" | cut -c1-12), $(wc -l < "$TMP/qb.cut") rows"
    cmp -s "$TMP/q1.cut" "$TMP/qb.cut" && echo "this build vs base build: identical" || echo "this build vs base build: DIFFERENT ($(cmp "$TMP/q1.cut" "$TMP/qb.cut" | grep -o 'line [0-9]*'))"
  fi
fi

if [[ $STAGES == *D* ]]; then
  echo; echo "## D. servo2 (swash_b) reversed: roll stick on the pad, disarmed (the firmware mixes the swash anyway)"
  for m in normal reversed; do
    edit=''; [ $m = reversed ] && edit='d["wiring"][5]["reversed"]=True'
    f=$(sess d-$m "$S/rs7.json" "$edit")
    echo "### $m: $(python3 "$HERE/session.py" "$f" "$TMP" 2>/dev/null | grep -o 'servos=[^,]*,actuators=[^,]*\(,servo-rev=[0-9]*\)\?')"
    start_session "$f" "fdm-log=$TMP/d-$m.csv,fdm-log-ms=10"
    python3 - <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"]))
c.wait_t(8.0)
c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 1000, 1000]})
c.wait_t(9.0)
base = c.state
c.send({"type": "rc", "ch_us": [1800, 1500, 1500, 1500, 1000, 1000]})   # ch1 = roll (map AECR1T23)
c.wait_t(10.5)
m = c.state
print(f"t={base['t']:.2f} centred: pwm_us {base['pwm_us'][:4]} actuators {base['actuators']}")
print(f"t={m['t']:.2f} roll 1800: pwm_us {m['pwm_us'][:4]} actuators {m['actuators']}")
EOF
    stop_core
    python3 - "$TMP/d-$m.csv" <<'EOF'
import csv, sys
r = list(csv.DictReader(open(sys.argv[1])))
a = next(x for x in r if 8.95 <= float(x["t"]) <= 9.0)
b = r[[i for i, x in enumerate(r) if float(x["t"]) <= 10.5][-1]]
for x, what in ((a, "centred"), (b, "roll")):
    out = [int(x[f"servo{i}"]) for i in range(1, 5)]
    fin = [int(x[f"in{i}"]) for i in range(1, 5)]
    print(f"FDM log t={float(x['t']):.2f} {what}: servo outputs {out} -> FDM inputs {fin}; "
          f"swash deflection seen by the FDM (in - 1500) {[v - 1500 for v in fin[:3]]}; roll {x['roll']} pitch {x['pitch']}")
EOF
  done
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. devices follow the wiring"
  echo "### no crsf wired (new form, the onboard receiver detached): $(python3 "$HERE/session.py" "$(sess e-nocrsf "$S/rs7-pads.json" 'd["wiring"].append({"device":"crsf","detached":True})')" "$TMP" 2>/dev/null | grep -o 'crsf=[a-z0-9]*')"
  start_session "$TMP/e-nocrsf.json"
  python3 - <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"]))
seen = []
for t in (6, 10, 15, 20, 25, 30):
    c.wait_t(t)
    s = c.state
    seen.append("RXLOSS" in s["fc"]["arming_flags"])
    print(f"t={s['t']:.2f} fc.arming_flags={s['fc']['arming_flags']} crsf.telemetry via link={s['fc']['link']}")
c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 2000, 1000]})   # arm switch: nothing reaches the FC
c.wait_t(33)
print(f"t={c.state['t']:.2f} after rc (arm switch on): armed={c.state['fc']['armed']} flags={c.state['fc']['arming_flags']}")
k = len(c.msgs)
c.send({"type": "fault", "device": "crsf", "kind": "link_lost", "on": True})
c.send({"type": "fault", "device": "icm42688p", "kind": "freeze", "on": False})
c.wait_t(c.t() + 0.3)
for m in c.msgs[k:]:
    if m["type"] in ("error", "event"):
        print(f"reply: {m}")
print(f"RXLOSS in every sample t=6..30: {all(seen)}; state.faults={c.state['faults']}")
EOF
  echo "-- bench: $(python3 "$HERE/hmp.py" $MON_PORT 'qom-get /machine bench' | grep -o 'crsf_rx_frames=[0-9]*')"
  stop_core
  echo "### no esc wired (new form without the esc entries): $(python3 "$HERE/session.py" "$(sess e-noesc "$S/rs7-pads.json" 'd["wiring"]=[w for w in d["wiring"] if w["device"]!="esc"]')" "$TMP" 2>/dev/null | grep -o 'esc=[a-z]*')"
  start_session "$TMP/e-noesc.json"
  python3 - <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"]))
c.wait_t(10)
c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 2000, 1000]})   # arm
c.wait_t(13)
c.send({"type": "rc", "ch_us": [1500, 1500, 1500, 1500, 2000, 2000]})   # throttle switch on
for t in (16, 20, 25):
    c.wait_t(t)
    s = c.state
    print(f"t={s['t']:.2f} armed={s['fc']['armed']} flags={s['fc']['arming_flags']} dshot={s['dshot']} "
          f"motor_rpm={s['motor_rpm']} rpm={s['rpm']} actuators.motor={s['actuators']['motor']}")
k = len(c.msgs)
c.send({"type": "fault", "device": "esc", "kind": "telemetry_lost", "on": True})
c.wait_t(c.t() + 0.3)
for m in c.msgs[k:]:
    if m["type"] in ("error", "event"):
        print(f"reply: {m}")
EOF
  echo "-- bench: $(python3 "$HERE/hmp.py" $MON_PORT 'qom-get /machine bench' | grep -o 'dshot=[0-9]* bidir=[0-9]* frames=[0-9]*')"
  stop_core
fi

if [[ $STAGES == *F* ]]; then
  echo; echo "## F. ctl_evidence.py and fault_evidence.py on the RS7 session (this build)"
  echo "### ctl_evidence.py"
  start_session "$(sess f-ctl "$S/rs7.json")"
  timeout 900 python3 "$HERE/ctl_evidence.py" --port $CTL_PORT --msp $UART_PORT 2>&1 | grep -v "^$" | cut -c1-400
  stop_core
  echo "### fault_evidence.py"
  start_session "$(sess f-fault "$S/rs7.json")"
  timeout 900 python3 "$HERE/fault_evidence.py" --port $CTL_PORT --msp $UART_PORT --mon $MON_PORT 2>&1 | grep -E "CHECK|SUMMARY" | tail -20
  stop_core
fi

if [[ $STAGES == *G* ]]; then
  echo; echo "## G. review fixes"
  echo "### old form without the crsf entry: receiver assumed on uart5 (as before the wiring model)"
  python3 "$HERE/session.py" "$(sess g-oldnocrsf "$S/rs7.json" 'd["wiring"]=[w for w in d["wiring"] if w["device"]!="crsf"]')" "$TMP" 2>&1 \
    | grep -E "old-form|SESSION_OPTS" | sed 's/,beeper=.*//'
  echo "### old form with crsf on uart3 (PORT-C), gyro only otherwise"
  python3 "$HERE/session.py" "$(sess g-olduart3 "$S/rs7.json" 'd["wiring"]=[{"device":"icm42688p","slot":"gyro1"},{"device":"crsf","slot":"uart3"}]')" "$TMP" 2>&1 \
    | grep -E "old-form|SESSION_OPTS" | sed 's/,beeper=.*//'
  echo "### rejections (session.py)"
  for e in 'd["wiring"][3]["actuator"]="motor"' 'd["wiring"][0]["actuator"]="tail"' 'd["wiring"][6]["reversed"]="yes"' \
           'd["wiring"][3]["trim"]=3' 'd["wiring"].append({"device":"icm42688p","slot":"gyro1"})' \
           'd["wiring"][0]["signal_timeout_ms"]=5000' 'd["wiring"]+=[{"device":"compass","pad":"i2c3"},{"device":"compass","pad":"i2c3"}]'; do
    echo "-- $e: $(python3 "$HERE/session.py" "$(sess g-bad "$S/rs7-pads.json" "$e")" "$TMP" 2>&1 >/dev/null | grep -v 'wiring rejected' | grep -v 'left out' | tr '\n' ' ')"
  done
  python3 - "$ROOT/models/rs7/aircraft.json" "$TMP/ac-notrans/aircraft.json" <<'EOF'
import json, os, sys
a = json.load(open(sys.argv[1])); a.pop("transmission")
os.makedirs(os.path.dirname(sys.argv[2]), exist_ok=True); json.dump(a, open(sys.argv[2], "w"))
EOF
  echo "-- aircraft without transmission: $(python3 "$HERE/session.py" "$(sess g-notrans "$S/rs7-pads.json" "d['aircraft']='$TMP/ac-notrans/aircraft.json'")" "$TMP" 2>&1 >/dev/null | tr '\n' ' ')"
  echo "### the machine itself (-M, no session.py)"
  cp "$ROOT/models/rs7/aircraft.json" "$TMP/ac-kv.json"; sed -i 's/"kv": 520/"kv": "520"/' "$TMP/ac-kv.json"
  cp "$ROOT/models/rs7/aircraft.json" "$TMP/ac-poles.json"; sed -i 's/"poles": 10/"poles": 7/' "$TMP/ac-poles.json"
  cp "$ROOT/models/rs7/aircraft.json" "$TMP/ac-centre.json"; sed -i 's/"tail": 1500}/"tail": 2600}/' "$TMP/ac-centre.json"
  ELF=$(python3 -c "import json,os; print(os.path.expanduser(json.load(open('$S/rs7.json'))['firmware']))")
  for m in "crsf=uart2" "fw=$ELF,crsf=uart3,imu=off" "fdm=on,aircraft-file=$TMP/ac-notrans/aircraft.json" "fdm=on,aircraft-file=$TMP/ac-kv.json" \
           "fdm=on,aircraft-file=$TMP/ac-poles.json" "fdm=on,aircraft-file=$TMP/ac-centre.json"; do
    timeout 3 "$QEMU" -M "nexus-xr,$m" -display none -monitor none -serial null </dev/null >"$TMP/q.out" 2>&1
    rc=$?
    if [ $rc = 124 ]; then out="accepted (ran until the 3 s timeout)"; else out="exit $rc: $(grep -m1 nexus-xr "$TMP/q.out")"; fi
    echo "-- -M $m: $out" | sed "s|$TMP/||g; s|fw=[^,]*/|fw=|"
  done
  echo "### the app's smoke bench (gyro only, old form, the app's keys, aircraft as another machine's path): starts"
  f=$(sess g-smoke "$S/rs7.json" 'd["wiring"]=[{"device":"icm42688p","slot":"gyro1","actuator":"","reversed":False,"option":""}]; d["aircraft"]="C:/Users/you/flightbench/models/rs7/aircraft.json"; d["time"]={"icount_shift":2,"realtime":False}')
  python3 "$HERE/session.py" "$f" "$TMP" 2>&1 | grep -E "old-form|SESSION_OPTS" | sed 's/,beeper=.*//'
  start_session "$f"
  python3 - <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"]))
c.wait_t(8.0)
s = c.state
print(f"t={s['t']:.2f} fc.msp={s['fc']['msp']} flags={s['fc']['arming_flags']} link={s['fc']['link']} "
      f"actuators={s['actuators']} pwm_us={s['pwm_us']}")
EOF
  stop_core
  echo "### unwired outputs in state: new form without servo3 (swash_c) and without the ESC"
  f=$(sess g-null "$S/rs7-pads.json" 'd["wiring"]=[w for w in d["wiring"] if w["device"]!="esc" and w.get("actuator")!="swash_c"]')
  python3 "$HERE/session.py" "$f" "$TMP" 2>/dev/null | grep -o 'esc=[a-z]*,servos=[0-9]*'
  start_session "$f"
  python3 - <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"]))
c.wait_t(8.0)
s = c.state
print(f"t={s['t']:.2f} actuators={s['actuators']} pwm_us={s['pwm_us']} dshot={s['dshot']}")
EOF
  stop_core
  echo "### IMU detached (new form): no CS into the model, INT1 not driven, no samples"
  f=$(sess g-noimu "$S/rs7-pads.json" 'd["wiring"].append({"device":"icm42688p","detached":True})')
  python3 "$HERE/session.py" "$f" "$TMP" 2>/dev/null | grep -o 'imu=[a-z]*'
  start_session "$f"
  python3 - <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
c = Core(int(os.environ["CTL_PORT"]))
c.send({"type": "subscribe", "state_hz": 100, "signals": ["imu.samples", "imu.odr_hz"]})
c.wait_t(8.0)
smp = c.wait_for(lambda m: m["type"] == "samples")
print(f"t={c.state['t']:.2f} samples {smp['v'] if smp else None}; flags={c.state['fc']['arming_flags']}")
st = [l for l in c.cli("status") if "Devices" in l or "GYRO" in l.upper() or "Arming" in l]
print("cli status: " + " | ".join(st))
k = len(c.msgs)
c.send({"type": "fault", "device": "icm42688p", "kind": "freeze", "on": True})
c.wait_t(c.t() + 0.3)
print("fault on the detached gyro: " + "; ".join(str(m) for m in c.msgs[k:] if m["type"] == "error"))
EOF
  stop_core
fi
