#!/bin/bash
# Milestone 2 step B, stages 2-4 evidence: RS7 profile, FDM on, -icount shift=2. The scripted pilot
# (flight.py, plan "liftoff") arms on AUX1, switches the throttle on, lets the governor spool to
# 1800, raises collective, flies roll/pitch/yaw doublets and hovers; the machine logs the FDM,
# flight.py logs MSP_STATUS/ATTITUDE/DEBUG; flight_report.py merges and summarises.
# Usage: flight_evidence.sh <elf> [outdir]   (prints the log; CSVs in outdir, default evidence/)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ELF=${1:?usage: flight_evidence.sh <elf> [outdir]}
OUT=${2:-$HERE/../evidence}
UNTIL=${UNTIL:-62}
S=$HERE
FDM=$(mktemp --suffix=.csv); MSP=$(mktemp --suffix=.csv)
PROFILE=$ROOT/models/rs7/qemu-profile.txt MACHINE_OPTS="fdm=on,fdm-log=$FDM,fdm-log-ms=20" \
  "$HERE/../run.sh" "$ELF" -icount shift=2,sleep=off >/tmp/flight-evidence-qemu.log 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT
echo "# $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD); -icount shift=2,sleep=off; PROFILE=models/rs7/qemu-profile.txt; fdm=on"
START=$(date +%s.%N)
python3 $S/flight.py --plan liftoff --coll 1620 --until "$UNTIL" --csv "$MSP"
END=$(date +%s.%N)
echo "## host time for $UNTIL virtual s (boot + flight): $(echo "$END - $START" | bc) s"
# No CLI "status" here: while armed the firmware skips non-MSP bytes on its MSP ports
# (fc/tasks.c: MSP_SKIP_NON_MSP_DATA), so '#' does not enter the CLI; MSP_STATUS above is the
# armed/flags evidence.
echo "## report"
mkdir -p "$OUT"
cp "$FDM" "$OUT/flight-fdm-2026-09-25.csv"
python3 $S/flight_report.py "$FDM" "$MSP" "$OUT/flight-2026-09-25.csv"
rm -f "$FDM" "$MSP"
