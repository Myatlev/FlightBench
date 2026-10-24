#!/bin/bash
# Reproduce the FlightBench QEMU build from a clean upstream checkout.
# Usage: core/qemu/build.sh [qemu-src-dir]   (default ~/flightbench-spike/qemu)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
Q=${1:-$HOME/flightbench-spike/qemu}
TAG=v11.1.1
[ -d "$Q/.git" ] || git clone --depth 1 --branch "$TAG" https://gitlab.com/qemu-project/qemu.git "$Q"
"$HERE/apply.sh" "$Q"
mkdir -p "$Q/build"
cd "$Q/build"
# PYTHON=<python3> for configure (QEMU wants 3.9+ with tomli/tomllib); on macOS a pyenv shim on PATH
# may be an old interpreter without tomli, so the Homebrew python is the default there.
PY=${PYTHON:-}
[ -n "$PY" ] || { [ "$(uname -s)" = Darwin ] && [ -x /opt/homebrew/bin/python3 ] && PY=/opt/homebrew/bin/python3; } || true
# macOS: a stale /usr/local/lib (an old Intel or i386 libz etc.) is searched before the SDK; put the SDK first
if [ "$(uname -s)" = Darwin ] && SDK=$(xcrun --show-sdk-path 2>/dev/null); then export LDFLAGS="-L$SDK/usr/lib ${LDFLAGS:-}"; fi
[ -f build.ninja ] || ../configure --target-list=arm-softmmu --disable-docs --disable-gtk \
    --disable-sdl --disable-vnc --disable-opengl --disable-spice --disable-werror \
    --disable-xen --disable-kvm --disable-tools --enable-slirp ${PY:+--python="$PY"}
EXE=; case "$(uname -s)" in MINGW*|MSYS*) EXE=.exe;; esac   # mingw target name carries .exe
ninja "qemu-system-arm$EXE"
echo "built $Q/build/qemu-system-arm$EXE"
