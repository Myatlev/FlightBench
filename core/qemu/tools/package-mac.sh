#!/bin/bash
# Package the macOS core for the client build: a self-contained folder with qemu-system-arm
# (stripped), the Homebrew dylibs it links (transitively), install names rewritten to @rpath, and a
# manifest of what went in. Run on macOS:  core/qemu/tools/package-mac.sh [qemu-build-dir] [out-dir]
#   default build dir: ~/flightbench-spike/qemu/build  (or QEMU_BUILD=)
#   default out dir:   dist/core                        (app/tools/export.sh bundles it from there)
# Layout: <out>/qemu-system-arm + <out>/lib/*.dylib. The binary carries two rpaths, so the same files
# work as this folder (@executable_path/lib) and inside FlightBench.app (Contents/Helpers/qemu-system-arm
# with the dylibs in Contents/Frameworks: @executable_path/../Frameworks).
# Signing: ad-hoc here with the JIT entitlements (QEMU's TCG needs MAP_JIT on Apple silicon; an
# arm64 binary must be signed to run at all). export.sh re-signs with the Developer ID when asked.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../../.." && pwd)
B=${1:-${QEMU_BUILD:-$HOME/flightbench-spike/qemu/build}}
OUT=${2:-$ROOT/dist/core}
[ "$(uname -s)" = Darwin ] || { echo "package-mac.sh: run on macOS" >&2; exit 1; }
EXE=$B/qemu-system-arm
[ -f "$EXE" ] || { echo "package-mac.sh: no $EXE (build first: core/qemu/build.sh $B/..)" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/lib"
strip -o "$OUT/qemu-system-arm" "$EXE"

# the dylibs outside the system (Homebrew / MacPorts / /usr/local), transitively
is_ext() { case "$1" in /opt/homebrew/*|/opt/local/*|/usr/local/*) return 0;; *) return 1;; esac; }
deps_of() { otool -L "$1" | awk 'NR>1{print $1}'; }
queue=("$OUT/qemu-system-arm"); seen=""
while [ ${#queue[@]} -gt 0 ]; do
  f=${queue[0]}; queue=("${queue[@]:1}")
  while read -r d; do
    is_ext "$d" || continue
    real=$(cd "$(dirname "$d")" && pwd -P)/$(basename "$(readlink "$d" || echo "$d")")
    [ -f "$real" ] || real=$d
    name=$(basename "$d")
    case " $seen " in *" $name "*) ;; *)
      seen="$seen $name"
      cp "$real" "$OUT/lib/$name"; chmod u+w "$OUT/lib/$name"
      queue+=("$OUT/lib/$name")
    ;; esac
  done < <(deps_of "$f")
done

# install names: every external reference -> @rpath/<name>; each dylib's own id -> @rpath/<name>
for f in "$OUT/qemu-system-arm" "$OUT"/lib/*.dylib; do
  while read -r d; do
    is_ext "$d" && install_name_tool -change "$d" "@rpath/$(basename "$d")" "$f"
  done < <(deps_of "$f")
done
for f in "$OUT"/lib/*.dylib; do install_name_tool -id "@rpath/$(basename "$f")" "$f"; done
# the build's own rpaths (Homebrew prefixes) would be searched first on a machine that has them: drop them
while read -r rp; do install_name_tool -delete_rpath "$rp" "$OUT/qemu-system-arm"; done \
  < <(otool -l "$OUT/qemu-system-arm" | awk '/LC_RPATH/{r=1} r && /path /{print $2; r=0}')
install_name_tool -add_rpath "@executable_path/lib" -add_rpath "@executable_path/../Frameworks" "$OUT/qemu-system-arm"

# nothing external may be left
left=$(for f in "$OUT/qemu-system-arm" "$OUT"/lib/*.dylib; do deps_of "$f"; done | while read -r d; do is_ext "$d" && echo "$d"; done || true)
[ -z "$left" ] || { echo "package-mac.sh: still linked outside the package: $left" >&2; exit 1; }

# ad-hoc signature with the JIT entitlements (install_name_tool invalidated the linker's one)
ENT="$HERE/qemu-entitlements.plist"
for f in "$OUT"/lib/*.dylib; do codesign --force --sign - "$f"; done
codesign --force --sign - --entitlements "$ENT" "$OUT/qemu-system-arm"

{
  echo "# FlightBench macOS core, packaged $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "flightbench $(git -C "$ROOT" rev-parse --short HEAD) ($(git -C "$ROOT" rev-parse --abbrev-ref HEAD))"
  echo "qemu $(git -C "$B/.." describe --tags --always 2>/dev/null) + core/qemu/patches/"
  echo "toolchain $(cc --version | head -1); $(sw_vers -productName) $(sw_vers -productVersion) $(uname -m)"
  echo; (cd "$OUT" && find . -type f | sort | while read -r f; do printf "%10d  %s\n" "$(stat -f %z "$f")" "${f#./}"; done)
} > "$OUT/CORE-MANIFEST.txt"
echo "packaged $OUT ($(du -sh "$OUT" | cut -f1)), $(ls "$OUT"/lib/*.dylib | wc -l | tr -d ' ') dylibs"
