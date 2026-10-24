#!/bin/bash
# Package the Windows core for the client build: a self-contained folder with qemu-system-arm.exe
# (stripped), the mingw DLLs it links (from ldd), libslirp-0.dll (a QEMU subproject), the machine's
# qemu-bundle pieces it needs (none for nexus-xr), and a manifest of what went in.
# Run under MSYS2 MINGW64:  core/qemu/tools/package-win.sh [qemu-build-dir] [out-dir]
#   default build dir: ~/flightbench-spike/qemu/build  (or QEMU_BUILD=)
#   default out dir:   dist/core                        (app/tools/export.ps1 bundles it from there)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd)
B=${1:-${QEMU_BUILD:-$HOME/flightbench-spike/qemu/build}}
OUT=${2:-$ROOT/dist/core}
case "$(uname -s)" in MINGW*|MSYS*) ;; *) echo "package-win.sh: run under MSYS2 MINGW64" >&2; exit 1;; esac
EXE=$B/qemu-system-arm.exe
[ -f "$EXE" ] || { echo "package-win.sh: no $EXE (build first: core/qemu/build.sh $B/..)" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
strip -o "$OUT/qemu-system-arm.exe" "$EXE"
# DLLs: everything ldd resolves outside the Windows system directories
PATH="$B/subprojects/slirp:$PATH" ldd "$EXE" | awk '/=> \//{print $3}' | grep -viE '^/c/windows/' | sort -u | while read -r dll; do
  cp "$dll" "$OUT/"
done
# sanity: every DLL the exe wants is now next to it (system ones excepted)
missing=$(cd "$OUT" && PATH="$OUT:$PATH" ldd ./qemu-system-arm.exe | grep -i "not found" | grep -viE 'api-ms-win|ext-ms-win' || true)
[ -z "$missing" ] || { echo "package-win.sh: unresolved: $missing" >&2; exit 1; }
{
  echo "# FlightBench Windows core, packaged $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "flightbench $(git -C "$ROOT" rev-parse --short HEAD) ($(git -C "$ROOT" rev-parse --abbrev-ref HEAD))"
  echo "qemu $(git -C "$B/.." describe --tags --always 2>/dev/null) + core/qemu/patches/"
  echo "toolchain $(gcc --version | head -1)"
  echo; ls -l "$OUT" | awk 'NR>1{printf "%10d  %s\n",$5,$9}'
} > "$OUT/CORE-MANIFEST.txt"
du -sh "$OUT" | cut -f1 | xargs -I{} echo "packaged $OUT ({}), $(ls "$OUT"/*.dll | wc -l) DLLs"
