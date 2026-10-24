#!/bin/bash
# Speed fixes: compare QEMU builds of the core on the same session and plan (evidence/speed/README.md).
#   P  per build: RS7 liftoff to 45 virtual s with prof=on -> $OUT/rs7-liftoff-$TAG-$DATE-<label>.json/.txt,
#      then prof.py diff of each build against the one before it (and first vs last) -> $OUT/diff-<a>-<b>.txt
#   S  speed: liftoff to 45 s, prof=off, RUNS (default 3) per build, builds alternating; host s per 10 virtual s
#      over virtual 5..45 (ping-watched, 20 Hz)
#   E  determinism: FDM log (every 20 ms, to 45 s) of every P and S run: md5 per run, runs of a build identical?
#      builds identical to the first?
#   Q  flight equivalence (FDM log of the first run of each build vs the first build): lift-off time,
#      roll/pitch/yaw at t = 35, 40, 45 s, head speed range airborne, altitude at 45 s
#   T  firmware health on the pad (no plan, disarmed, t > 15 virtual s): CLI `tasks` + `status` per build
#   I  IMU trace (imu-log=, builds that have it): per sample INT1 rise / DMA done / INT1 fall, 45 s of the
#      liftoff plan; sample instants compared with the first build that has the log
# Everything on its own core: control 31280, MSP 31281, HMP 31282, motion 31283, pilot 31284 (PORT_BASE=<n>
# moves the five). No bench state (BENCH_STATE=): every run starts from the first start or its snapshot.
# usage: speed_evidence.sh "<stages>" <label>=<qemu-system-arm> [<label>=<qemu-system-arm> ...]
#   SESSION=<session.json> (default sessions/rs7.json), OUT=<dir> (default core/qemu/evidence/speed),
#   TAG=<host name in file names> (default homelab), RUNS=<speed runs per build> (default 3)
# Prints the log on stdout.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
STAGES=${1:?usage: speed_evidence.sh "<stages>" label=qemu ...}; shift
SESSION=${SESSION:-$HERE/../sessions/rs7.json}
P0=${PORT_BASE:-31280}
export CTL_PORT=$P0 UART_PORT=$((P0 + 1)) MON_PORT=$((P0 + 2)) MOTION_PORT=$((P0 + 3)) PILOT_PORT=$((P0 + 4))
export BENCH_STATE=
OUT=${OUT:-$HERE/../evidence/speed}
TAG=${TAG:-homelab}
RUNS=${RUNS:-3}
DATE=$(TZ=Asia/Yerevan date +%Y-%m-%d)
TMP=$(mktemp -d /tmp/speed-ev.XXXXXX)
QPID=
trap 'stop_core; rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
export ROOT
LABELS=()
declare -A BIN
for a in "$@"; do LABELS+=("${a%%=*}"); BIN[${a%%=*}]=${a#*=}; done

stop_core() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null && wait "$QPID" 2>/dev/null; QPID=; }
python3 - "$SESSION" "$TMP/session.json" "$CTL_PORT" "$UART_PORT" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["ports"] = {"control": int(sys.argv[3]), "msp": int(sys.argv[4])}
json.dump(d, open(sys.argv[2], "w"))
EOF
S=$TMP/session.json
# core <label> <prof 0|1> <name> [extra machine opts]: starts the core in the background (QPID)
core() {
  QEMU=${BIN[$1]} PROF=$2 MACHINE_OPTS="fdm-log=$TMP/$3-fdm.csv,fdm-log-ms=20${4:+,$4}" \
    "$HERE/../run.sh" --session "$S" >"$TMP/$3-core.log" 2>&1 &
  QPID=$!
}
# run <label> <name> <prof 0|1> [prof.py args]: liftoff to 45 s; FDM log cut at 45 s in $TMP/<name>.cut
run() {
  local l=$1 name=$2 prof=$3; shift 3
  core "$l" "$prof" "$name" "${EXTRA_OPTS:-}"
  python3 "$HERE/prof.py" run --session "$S" --pilot $PILOT_PORT --until 45 --speed-from 5 \
    --plan liftoff "$@" | grep -E '^speed|^wrote'
  stop_core
  awk -F, 'NR == 1 || $1 <= 45.0' "$TMP/$name-fdm.csv" > "$TMP/$name.cut"
}

echo "# speed evidence, $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD)$(git -C "$HERE" diff --quiet HEAD -- "$HERE/.." || echo +dirty); $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'); load $(cut -d' ' -f1-3 /proc/loadavg)"
for l in "${LABELS[@]}"; do echo "# $l = ${BIN[$l]} ($(strings "${BIN[$l]}" | grep -m1 '^flightbench '))"; done
echo "# session $SESSION; other qemu-system-arm processes: $(pgrep -c -x qemu-system-arm)"

if [[ $STAGES == *P* ]]; then
  echo; echo "## P. RS7 liftoff plan, 45 virtual s, prof=on, per build"
  prev=
  for l in "${LABELS[@]}"; do
    F=$OUT/rs7-liftoff-$TAG-$DATE-$l
    echo "### $l"
    run "$l" "P-$l" 1 --out "$F.json"
    python3 "$HERE/prof.py" show "$F.json" > "$F.txt" && echo "show -> $(basename "$F").txt"
    if [ -n "$prev" ]; then
      python3 "$HERE/prof.py" diff "$OUT/rs7-liftoff-$TAG-$DATE-$prev.json" "$F.json" > "$OUT/diff-$prev-$l.txt" &&
        echo "diff -> diff-$prev-$l.txt"
    fi
    prev=$l
  done
  first=${LABELS[0]}
  if [ "${#LABELS[@]}" -gt 2 ]; then
    python3 "$HERE/prof.py" diff "$OUT/rs7-liftoff-$TAG-$DATE-$first.json" "$OUT/rs7-liftoff-$TAG-$DATE-$prev.json" \
      > "$OUT/diff-$first-$prev.txt" && echo "diff -> diff-$first-$prev.txt"
  fi
fi

if [[ $STAGES == *S* ]]; then
  echo; echo "## S. speed: prof=off, $RUNS runs per build, alternating; liftoff to 45 virtual s, host s per 10 virtual s over virtual 5..45"
  for r in $(seq "$RUNS"); do
    for l in "${LABELS[@]}"; do
      echo "### $l run $r"
      run "$l" "S-$l-$r" 0 --no-dump | tee -a "$TMP/speeds-$l.txt"
    done
  done
  echo "### summary"
  for l in "${LABELS[@]}"; do
    python3 - "$l" "$TMP/speeds-$l.txt" <<'EOF2'
import statistics, sys
x = [float(l.split()[1]) for l in open(sys.argv[2]) if l.startswith("speed")]
print(f"{sys.argv[1]}: {', '.join(f'{v:.2f}' for v in x)}; mean {statistics.mean(x):.2f}"
      + (f", stdev {statistics.stdev(x):.2f}" if len(x) > 1 else ""))
EOF2
  done
fi

if [[ $STAGES == *E* ]]; then
  echo; echo "## E. determinism: FDM log to 45 s (20 ms rows, printed precision)"
  for l in "${LABELS[@]}"; do
    for f in "$TMP"/[PS]-"$l"*.cut; do
      [ -f "$f" ] && echo "$(basename "$f" .cut): $(md5sum < "$f" | cut -c1-12) $(wc -l < "$f") rows"
    done
    n=$(ls "$TMP"/[PS]-"$l"*.cut 2>/dev/null | xargs -r md5sum | cut -d' ' -f1 | sort -u | wc -l)
    echo "=> $l: $n distinct FDM log(s) over $(ls "$TMP"/[PS]-"$l"*.cut 2>/dev/null | wc -l) runs"
  done
  ref=$(ls "$TMP"/[PS]-"${LABELS[0]}"*.cut 2>/dev/null | head -1)
  for l in "${LABELS[@]:1}"; do
    f=$(ls "$TMP"/[PS]-"$l"*.cut 2>/dev/null | head -1)
    [ -n "$f" ] && { cmp -s "$ref" "$f" && echo "$l == ${LABELS[0]}: byte-identical" ||
      echo "$l vs ${LABELS[0]}: DIFFERENT ($(cmp "$ref" "$f" | grep -o 'line [0-9]*'))"; }
  done
fi

if [[ $STAGES == *Q* ]]; then
  echo; echo "## Q. flight equivalence (FDM log; reference ${LABELS[0]})"
  args=()
  for l in "${LABELS[@]}"; do f=$(ls "$TMP"/[PS]-"$l"*.cut 2>/dev/null | head -1); [ -n "$f" ] && args+=("$l=$f"); done
  python3 - "${args[@]}" <<'EOF3'
import csv, sys
def load(p):
    return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(open(p))]
def at(r, t):
    return min(r, key=lambda x: abs(x["t"] - t))
res = {}
for a in sys.argv[1:]:
    l, p = a.split("=", 1)
    r = load(p)
    air = [x for x in r if x["on_ground"] == 0]
    q = {"liftoff": air[0]["t"] if air else float("nan")}
    for t in (35, 40, 45):
        x = at(r, t)
        q.update({f"roll{t}": x["roll"], f"pitch{t}": x["pitch"], f"yaw{t}": x["yaw"]})
    q["rpm_min"] = min(x["head_rpm"] for x in air) if air else 0
    q["rpm_max"] = max(x["head_rpm"] for x in air) if air else 0
    q["alt45"] = at(r, 45)["alt"]
    res[l] = q
ref = next(iter(res.values()))
print("| | " + " | ".join(res) + " | criterion |")
print("|---|" + "---|" * len(res) + "---|")
def row(name, key, fmt, crit):
    print(f"| {name} | " + " | ".join(fmt.format(q[key]) for q in res.values()) + f" | {crit} |")
row("lift-off t, s", "liftoff", "{:.2f}", "±0.1 s")
for t in (35, 40, 45):
    for ax in ("roll", "pitch", "yaw"):
        row(f"{ax} at {t} s, deg", f"{ax}{t}", "{:.2f}", "±1°")
print("| head speed airborne, rpm | " + " | ".join(f"{q['rpm_min']:.0f}..{q['rpm_max']:.0f}" for q in res.values()) + " | 1793..1802 |")
row("altitude at 45 s, m", "alt45", "{:.3f}", "±2 %")
for l, q in list(res.items())[1:]:
    bad = []
    if abs(q["liftoff"] - ref["liftoff"]) > 0.1: bad.append("lift-off")
    for t in (35, 40, 45):
        for ax in ("roll", "pitch", "yaw"):
            d = abs(q[f"{ax}{t}"] - ref[f"{ax}{t}"]); d = min(d, 360 - d)
            if d > 1.0: bad.append(f"{ax}{t}")
    if round(q["rpm_min"]) < 1793 or round(q["rpm_max"]) > 1802: bad.append("head speed")
    if abs(q["alt45"] - ref["alt45"]) > 0.02 * abs(ref["alt45"]): bad.append("alt45")
    print(f"{l} vs {next(iter(res))}: " + ("EQUIVALENT" if not bad else "NOT equivalent: " + ", ".join(bad)))
EOF3
fi

if [[ $STAGES == *T* ]]; then
  echo; echo "## T. firmware health on the pad (no plan, disarmed, t > 15 virtual s): CLI tasks + status"
  for l in "${LABELS[@]}"; do
    core "$l" 0 "T-$l"
    python3 - <<'EOF4'
import os, sys, time
sys.path.insert(0, os.path.join(os.environ["ROOT"], "tools"))
from core_client import Core
for _ in range(100):
    try:
        c = Core(int(os.environ["CTL_PORT"])); break
    except OSError:
        time.sleep(0.1)
c.wait_t(15.0)
EOF4
    echo "### $l"
    CLI_DRAIN=3 python3 "$HERE/cli.py" $UART_PORT 1 tasks status 2>&1 | grep -E 'late|^[0-9]+ - |Total|CPU:|RX Check'
    stop_core
  done
fi

if [[ $STAGES == *I* ]]; then
  echo; echo "## I. IMU trace (imu-log=), liftoff to 45 virtual s, prof=off"
  for l in "${LABELS[@]}"; do
    EXTRA_OPTS="imu-log=$TMP/I-$l-imu.log" run "$l" "I-$l" 0 --no-dump
    if [ ! -s "$TMP/I-$l-imu.log" ]; then echo "$l: no IMU trace (build without imu-log)"; continue; fi
    echo "$l: $(wc -l < "$TMP/I-$l-imu.log") events; samples $(grep -c '^S' "$TMP/I-$l-imu.log"), md5 of the sample instants $(grep '^S' "$TMP/I-$l-imu.log" | md5sum | cut -c1-12)"
  done
  python3 - "$TMP" "${LABELS[@]}" <<'EOF5'
import os, sys
d = sys.argv[1]
logs = {l: f"{d}/I-{l}-imu.log" for l in sys.argv[2:]
        if os.path.exists(f"{d}/I-{l}-imu.log") and os.path.getsize(f"{d}/I-{l}-imu.log")}
samples = {}
for l, p in logs.items():
    order, off, cur, seq, s = {}, {}, None, [], []
    for line in open(p):
        k, t = line.split()[0], int(line.split()[1])
        if k == "S":
            if seq:
                order[tuple(seq)] = order.get(tuple(seq), 0) + 1
            cur, seq = t, ["S"]
            s.append(t)
        elif cur is not None:
            seq.append(k)
            off.setdefault(k, []).append(t - cur)
    if seq:
        order[tuple(seq)] = order.get(tuple(seq), 0) + 1
    samples[l] = s
    print(f"{l}: event orders per sample {dict(sorted(order.items(), key=lambda x: -x[1])[:4])}; offsets from "
          "the sample (ns, min..max): " + ", ".join(f"{k} {min(v)}..{max(v)}" for k, v in off.items()))
def cmp(a, b):
    x, y = samples[a], samples[b]
    n = min(len(x), len(y))
    if x == y:
        return f"{a} vs {b}: sample instants IDENTICAL ({len(x)} samples)"
    dd = sorted({u - v for u, v in zip(x[:n], y[:n])})
    return (f"{a} vs {b}: sample instants DIFFERENT ({len(x)} vs {len(y)} samples; "
            + (f"the same grid shifted by a constant {dd[0]} ns)" if len(dd) == 1
               else f"{len(dd)} distinct offsets {dd[0]}..{dd[-1]} ns)"))
ls = list(samples)
for i in range(1, len(ls)):
    print(cmp(ls[i], ls[i - 1]))
    if i > 1:
        print(cmp(ls[i], ls[0]))
EOF5
fi
echo; echo "# qemu-system-arm processes left by this script: none (the other $(pgrep -c -x qemu-system-arm) are not ours)"
