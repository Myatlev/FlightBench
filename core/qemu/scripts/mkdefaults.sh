#!/bin/bash
# REFERENCE IMPLEMENTATION for benches: -M nexus-xr,session=<bench> builds the blob in memory
# (devices/src/session.c, byte-identical: core/qemu/tests/test_session_equiv.sh). Still used by
# run.sh's bare mode (run.sh <elf>, BOARD_CONFIG/PROFILE).
# Build the custom-defaults blob that the configurator would insert into the
# firmware image at __custom_defaults_start (0x08002808 for STM32F7X2 builds).
# Usage: mkdefaults.sh <board.config> <out.bin|out.hex> [profile.txt ...]
# Profiles (CLI commands, e.g. models/rs7/qemu-profile.txt) are appended after
# the board config; comment-only and blank lines are dropped to save space
# (the firmware's CLI ignores them anyway).
set -euo pipefail
IN=${1:?config}; OUT=${2:?out}; shift 2
{ printf '## Rotorflight Custom Defaults\n'; tr -d '\r' < "$IN"
  for P in "$@"; do tr -d '\r' < "$P" | grep -v -E '^[[:space:]]*(#|$)'; done; } > "$OUT"
SZ=$(stat -c %s "$OUT")
MAX=$((0x08004000 - 0x08002808))
[ "$SZ" -lt "$MAX" ] || { echo "defaults too big: $SZ >= $MAX" >&2; exit 1; }
echo "$OUT: $SZ bytes (max $MAX)"
# QEMU's generic loader limits raw images to ram_size (0 on this machine);
# Intel HEX carries its own address and has no such limit.
if [ "${OUT##*.}" = hex ]; then
  # any binutils objcopy converts binary -> ihex; the host one will do
  OBJCOPY=${OBJCOPY:-$(command -v arm-none-eabi-objcopy || command -v objcopy)}
  mv "$OUT" "$OUT.bin"
  "$OBJCOPY" -I binary -O ihex --change-addresses 0x08002808 "$OUT.bin" "$OUT"
  rm -f "$OUT.bin"
fi
