#!/bin/bash
# Milestone 2 step B, stage 1-2 evidence: RS7 profile, -icount shift=2. Servo pulse widths
# decoded from the timer compare registers, the DShot600 frames decoded from the TIM1_CH2
# DMA buffer, and the bidirectional eRPM reply decoded by the firmware.
# Usage: outputs_evidence.sh <elf>   (prints the log)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ELF=${1:?usage: outputs_evidence.sh <elf>}
S=$HERE
PROFILE=$ROOT/models/rs7/qemu-profile.txt "$HERE/../run.sh" "$ELF" -icount shift=2,sleep=off \
  >/tmp/outputs-evidence-qemu.log 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT
echo "# $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD); -icount shift=2,sleep=off; PROFILE=models/rs7/qemu-profile.txt"
sleep 16      # ~7 virtual s: boot, defaults applied, config write + reset
echo "## idle outputs (disarmed): servos at centre (tail at centre + tail_center_trim), DShot 0"
python3 $S/hmp.py 31236 "qom-get /machine bench"
echo "## CLI overrides"
CLI_DRAIN=2 python3 $S/cli.py 31235 0 version "get motor_pwm_protocol" "get dshot_bidir" \
  "servo override 1 250" "servo override 2 -250" "servo override 3 500" "servo override 4 -500" \
  "motor override 1 500" | grep -v '^$'
sleep 6
echo "## decoded by the machine (expected with +GEO: mid + 1000*asin(0.766*v)*1.1459 ="
echo "##   1720.8, 1279.2, 1950.4; servo 4 clamped at its min -350 -> 1150; DShot 48 + 0.5*1999 = 1047)"
python3 $S/hmp.py 31236 "qom-get /machine bench"
echo "## eRPM decoded by the firmware from the GCR edges"
CLI_DRAIN=3 python3 $S/cli.py 31235 0 version "motor status" dshot_telemetry_info | grep -v '^$'
python3 $S/hmp.py 31236 "qom-get /machine bench"
