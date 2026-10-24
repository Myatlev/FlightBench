#!/bin/bash
# Milestone 2 step A evidence: boot the MSP build with the board config at -icount shift=2
# and record status/tasks, MSP_RAW_IMU (before/after MSP_ACC_CALIBRATION), a motion-source
# step and the IMU sample counter. Usage: gyro_evidence.sh <elf>   (prints the log)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ELF=${1:?usage: gyro_evidence.sh <elf>}
S=$HERE
"$HERE/../run.sh" "$ELF" -icount shift=2,sleep=off >/tmp/gyro-evidence-qemu.log 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT
echo "# $(TZ=Asia/Yerevan date '+%Y-%m-%d %H:%M') Yerevan; $(git -C "$HERE" rev-parse --short HEAD); -icount shift=2,sleep=off"
sleep 16      # ~8 virtual s: boot, config write + reset, gyro calibration
echo "## HMP"; python3 $S/hmp.py 31236 "qom-get /machine imu-odr" "qom-get /machine imu-samples"
echo "## MSP_RAW_IMU (before acc calibration)"; python3 $S/msp.py 31235 raw_imu 3
echo "## MSP_ACC_CALIBRATION"; python3 $S/msp.py 31235 acc_calibration; sleep 4
echo "## MSP_RAW_IMU (after)"; python3 $S/msp.py 31235 raw_imu 3
echo "## motion source: chip gyro (10, -20, 30) dps"
printf '{"gyro_dps":[10,-20,30]}\n' | timeout 2 nc -q1 127.0.0.1 31237; sleep 1
python3 $S/msp.py 31235 raw_imu 2
printf '{"gyro_dps":[0,0,0]}\n' | timeout 2 nc -q1 127.0.0.1 31237; sleep 1
python3 $S/msp.py 31235 raw_imu 1
echo "## CLI"; CLI_DRAIN=6 python3 $S/cli.py 31235 0 status tasks "get gyro_1" "get pid_process_denom"
echo "## HMP"; python3 $S/hmp.py 31236 "qom-get /machine imu-samples"
