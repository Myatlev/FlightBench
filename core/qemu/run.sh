#!/bin/bash
# Boot a RotorFlight STM32F7X2 build on the nexus-xr machine (a bench: the machine of its board,
# e.g. speedybee-f405-wing for iNav).
# Control server (docs/protocol.md, JSON lines) -> TCP ${CTL_HOST:-127.0.0.1}:${CTL_PORT:-31250}
# The board's MSP UART (nexus-xr: USART1, MSP/CLI with an OPTIONS=USE_MSP_UART build;
#   speedybee-f405-wing: USART6) -> TCP ${UART_HOST:-127.0.0.1}:${UART_PORT:-31235}
#   (UART_HOST=<tailnet ip> lets the RotorFlight configurator on another machine connect: tcp://ip:31235)
# HMP monitor -> TCP 127.0.0.1:${MON_PORT:-31236}; gdbstub with GDB=1 -> :31234 (halted)
# IMU motion source (JSON lines, see README) -> TCP 127.0.0.1:${MOTION_PORT:-31237}
# Pilot (CRSF receiver channels, JSON lines, see README) -> TCP 127.0.0.1:${PILOT_PORT:-31238}
# Usage: core/qemu/run.sh --session <bench.json> [extra qemu args...]   (or FB_SESSION=...)
#        core/qemu/run.sh <firmware.elf|.hex> [extra qemu args...]      (bare: no bench)
#        core/qemu/run.sh --manifest [--pretty]   prints the core manifest (what this core can
#          emulate: boards/slots, devices with props/faults, aircraft) and exits; the control
#          server sends the same document after hello (docs/protocol.md)
#   --session: the machine resolves the bench itself (-M session=<bench>,root=<repo>: wiring,
#   device options, FDM, custom defaults, manifest; devices/src/session.c). This script only
#   builds the QEMU command line of docs/protocol.md "Launching the core": -icount from the
#   bench's time.icount_shift, the control/MSP ports from its `ports` and the machine type of its
#   board (-M <machine>; all from the `launch:` line of -M session-check=on), the sockets. No
#   python, no temp files.
#   Bare: the machine loads the firmware into its flash itself (fw=); the board config (+ PROFILE=
#   profiles) goes in as the custom defaults blob (defaults=, scripts/mkdefaults.sh), the manifest
#   from scripts/manifest.py. BOARD_CONFIG=boards/nexus-xr/NEXUS_XR.config (default);
#   BOARD_CONFIG= (empty) boots bare. PROFILE="models/rs7/qemu-profile.txt ..." appends profiles.
#   The machine has its default devices (the RS7 layout) and the aircraft parameters of
#   AIRCRAFT_FILE=models/rs7/aircraft.json (default; AIRCRAFT_FILE= for none).
#   --session also keeps the firmware's config as the bench state (-M bench-state=; docs/protocol.md):
#   <cfg-cache dir>/<session file name>.state.bin; BENCH_STATE=<file> another file, BENCH_STATE=
#   (empty) none (the evidence scripts: winproc.py), cfg-cache=off in MACHINE_OPTS none.
#   Both: MACHINE_OPTS="fdm-log=/tmp/flight.csv" adds machine options (they win over the
#   bench's). CTL_PORT= disables the control server (bare). PROF=1 turns on the host-time ledger
#   (-M prof=on; the control server's `prof` command, scripts/prof.py), PROF_PERIOD=<s> also sends
#   a `prof` message every s virtual seconds.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$HOME/flightbench-spike/qemu/build/qemu-system-arm}
# QEMU is a native exe: under MSYS2 it does not understand /tmp or /d/... paths inside -M values
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
# -M splits at commas: a literal comma in a value is ",,"
mval() { local v; v=$(native "$1"); printf '%s' "${v//,/,,}"; }
if [ "${1:-}" = --manifest ]; then
  shift
  if [ -x "$QEMU" ] && [ -z "${BOARD_CONFIG+set}" ]; then
    M=$("$QEMU" -M "nexus-xr,root=$(mval "$ROOT"),session-check=on" -display none)
    M=${M#manifest: }
    if [ "${1:-}" = --pretty ]; then
      printf '%s\n' "$M" | python3 -m json.tool --indent 1 --no-ensure-ascii
    else
      printf '%s\n' "$M"
    fi
    exit 0
  fi
  # no QEMU here, or another board config: the reference implementation
  # shellcheck disable=SC2086
  exec python3 "$HERE/scripts/manifest.py" ${BOARD_CONFIG:+--board-config "$BOARD_CONFIG"} "$@"
fi
SESSION=${FB_SESSION:-}
MACHINE=
if [ "${1:-}" = --session ]; then SESSION=${2:?usage: run.sh --session <bench.json>}; shift 2; fi
ARGS=()
if [ -n "$SESSION" ]; then
  OPTS="session=$(mval "$SESSION"),root=$(mval "$ROOT")"
  if ! CHECK=$("$QEMU" -M "nexus-xr,$OPTS,session-check=on" -display none 2>&1); then
    printf '%s\n' "$CHECK" | grep '^session: ' >&2 || printf '%s\n' "$CHECK" >&2
    echo "run.sh: session $SESSION rejected (see above)" >&2
    exit 1
  fi
  # the report's launch line (before the manifest; docs/protocol.md "Launching the core"):
  #   launch: icount_shift=<S|none> realtime=<on|off> control=<N> msp=<M> machine=<name>
  # (sed reads the whole report: quitting early would SIGPIPE printf under pipefail once the report
  # is larger than a pipe's buffer, e.g. a CLI board's config_src/config_json lines)
  LAUNCH=$(printf '%s\n' "$CHECK" | sed -n '/^manifest: /,$b;s/^launch: //p')
  launch() { local kv; for kv in $LAUNCH; do [ "${kv%%=*}" = "$1" ] && printf '%s' "${kv#*=}"; done; return 0; }
  SHIFT=$(launch icount_shift); CTL_PORT=$(launch control); UART_PORT=$(launch msp)
  MACHINE=$(launch machine)     # the bench board's QEMU machine; a report without it: nexus-xr
  if [ -z "$SHIFT" ] || [ -z "$CTL_PORT" ] || [ -z "$UART_PORT" ]; then
    echo "run.sh: no launch line in the session-check report of $QEMU" >&2
    exit 1
  fi
  [ "$SHIFT" != none ] && ARGS+=(-icount "shift=$SHIFT,sleep=off")
  echo "run.sh: session $SESSION; machine ${MACHINE:-nexus-xr}; icount shift $SHIFT; control $CTL_PORT, msp $UART_PORT" >&2
  # the bench state (the firmware's config as the bench left it; CLI boards): next to the cfg-cache,
  # <cache>/<bench id>.state.bin (+ .state.json), the bench id = the session file's name.
  # BENCH_STATE=<file> another file, BENCH_STATE= (empty) none; cfg-cache=off in MACHINE_OPTS: none
  if [ -z "${BENCH_STATE+set}" ]; then
    CACHE=$(printf '%s' ",${MACHINE_OPTS:-}" | sed -n 's/.*,cfg-cache=\([^,]*\).*/\1/p')
    if [ -z "$CACHE" ]; then
      if [ -n "${LOCALAPPDATA:-}" ] && command -v cygpath >/dev/null 2>&1; then
        CACHE=$(cygpath -u "$LOCALAPPDATA")/flightbench/config    # the core's default on Windows
      else
        CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/flightbench/config
      fi
    fi
    BENCH_STATE=
    [ "$CACHE" != off ] && BENCH_STATE=$CACHE/$(basename "${SESSION%.json}").state.bin
  fi
  if [ -n "$BENCH_STATE" ]; then
    OPTS+=",bench-state=$(mval "$BENCH_STATE")"
    echo "run.sh: bench state $BENCH_STATE" >&2
  fi
else
  ELF=${1:?usage: run.sh --session <bench.json> [qemu args] | run.sh <firmware.elf> [qemu args]}; shift
  TMP=$(mktemp -d /tmp/fbcore.XXXXXX)
  TMPN=$(native "$TMP")
  # the tmp dir (defaults blob, manifest) must outlive this script while QEMU runs (a firmware
  # reload re-reads the defaults), so it is removed only when we fail before the exec
  trap 'rm -rf "$TMP"' EXIT
  BOARD_CONFIG=${BOARD_CONFIG-$ROOT/boards/nexus-xr/NEXUS_XR.config}
  OPTS="fw=$(mval "$ELF")"
  AIRCRAFT_FILE=${AIRCRAFT_FILE-$ROOT/models/rs7/aircraft.json}
  [ -n "$AIRCRAFT_FILE" ] && OPTS+=",aircraft-file=$(mval "$AIRCRAFT_FILE")"
  if [ -n "$BOARD_CONFIG" ]; then
    # shellcheck disable=SC2086
    "$HERE/scripts/mkdefaults.sh" "$BOARD_CONFIG" "$TMP/defaults.bin" ${PROFILE:-} >/dev/null
    OPTS+=",defaults=$TMPN/defaults.bin"
  fi
  CTL_PORT=${CTL_PORT-31250}
  if [ -n "$CTL_PORT" ]; then
    if python3 "$HERE/scripts/manifest.py" ${BOARD_CONFIG:+--board-config "$BOARD_CONFIG"} --out "$TMP/manifest.json"; then
      OPTS+=",manifest=$TMPN/manifest.json"
    else
      echo "run.sh: no manifest (scripts/manifest.py failed); the control server runs without it" >&2
    fi
  fi
fi
[ -n "$CTL_PORT" ] && OPTS+=",ctl=${CTL_HOST:-127.0.0.1}:$CTL_PORT"
if [ "${PROF:-0}" = 1 ]; then
  OPTS+=",prof=on"
  [ -n "${PROF_PERIOD:-}" ] && OPTS+=",prof-period-s=$PROF_PERIOD"
fi
[ -n "${MACHINE_OPTS:-}" ] && OPTS+=",$MACHINE_OPTS"
[ "${GDB:-0}" = 1 ] && ARGS+=(-gdb tcp::31234 -S)
trap - EXIT
exec "$QEMU" -M "${MACHINE:-nexus-xr},$OPTS" -nographic "${ARGS[@]}" \
  -chardev socket,id=u1,host=${UART_HOST:-127.0.0.1},port=${UART_PORT:-31235},server=on,wait=off -serial chardev:u1 \
  -monitor tcp:127.0.0.1:${MON_PORT:-31236},server=on,wait=off \
  -chardev socket,id=fbmotion,host=127.0.0.1,port=${MOTION_PORT:-31237},server=on,wait=off \
  -chardev socket,id=fbpilot,host=127.0.0.1,port=${PILOT_PORT:-31238},server=on,wait=off "$@"
