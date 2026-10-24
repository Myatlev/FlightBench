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
[ -f build.ninja ] || ../configure --target-list=arm-softmmu --disable-docs --disable-gtk \
    --disable-sdl --disable-vnc --disable-opengl --disable-spice --disable-werror \
    --disable-xen --disable-kvm --disable-tools --enable-slirp
EXE=; case "$(uname -s)" in MINGW*|MSYS*) EXE=.exe;; esac   # mingw target name carries .exe
ninja "qemu-system-arm$EXE"
echo "built $Q/build/qemu-system-arm$EXE"
