#!/bin/bash
# Standalone macOS build: Godot export + the repo data the app reads at run time + the core, as FlightBench.app
# (the macOS counterpart of export.ps1; see app/README.md -> Export).
#   app/tools/export.sh                        # release -> dist/FlightBench-macos/FlightBench.app (+ LICENSE, README.txt)
#   app/tools/export.sh --data-only            # only refresh the bundle's data/
#   app/tools/export.sh --presets rs7-first-flight   # release profile: these presets + their firmware only (v0.1)
#   app/tools/export.sh --zip                  # also pack dist/FlightBench-macos-v<version>.zip (ditto, keeps the bundle's attributes)
#   app/tools/export.sh --universal            # keep the x86_64 half of the app (default: arm64 only, like the core)
#   app/tools/export.sh --suffix all-presets   # zip name FlightBench-macos-v<version>-<suffix>.zip (a test build next to the release one)
#   app/tools/export.sh --sign "Developer ID Application: Name (TEAMID)"   # code-sign everything, hardened runtime
#   app/tools/export.sh --sign ... --notarize <notarytool keychain profile>  # + notarize (xcrun notarytool) and staple
# Godot: --godot <Godot .NET binary>, else $GODOT, else `godot` on PATH, else /Applications/Godot_mono.app.
# Core:  --core-dir <dir>, else $CORE_DIR, else <repo>/dist/core (where core/qemu/tools/package-mac.sh puts it:
#        qemu-system-arm + lib/*.dylib, install names @rpath). Without it the build has no bundled core.
#
# Bundle layout (what the app looks for, scripts/CoreLauncher.cs CoreLocator and AppSettings.DefaultRepoRoot):
#   Contents/MacOS/FlightBench                      Godot's binary (the PCK is in Contents/Resources/FlightBench.pck,
#                                                    the .NET runtime + FlightBench.dll in Contents/Resources/data_*)
#   Contents/Resources/data/{boards,models,presets,firmware}   the data root (= export.ps1's data/)
#   Contents/Resources/data/CORE-MANIFEST.txt       what core went in
#   Contents/Helpers/qemu-system-arm                the core (an executable may not live in Resources/ of a signed bundle)
#   Contents/Frameworks/*.dylib                     its libraries (the core's second rpath, @executable_path/../Frameworks)
# Signing is inside-out: every Mach-O in the bundle (the .NET runtime's dylibs sit in Resources/, codesign --deep
# would skip them), the core with core/qemu/tools/qemu-entitlements.plist (JIT), the app with tools/macos-entitlements.plist.
# Without --sign the bundle is signed ad hoc (it runs locally; Gatekeeper refuses it on another Mac).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); APP=$(cd "$HERE/.." && pwd); REPO=$(cd "$APP/.." && pwd)
[ "$(uname -s)" = Darwin ] || { echo "export.sh: run on macOS (Windows: export.ps1)" >&2; exit 1; }

DATA_ONLY=; ZIP=; UNIVERSAL=; SUFFIX=; PRESETS=("*"); CORE_DIR=${CORE_DIR:-$REPO/dist/core}; GODOT=${GODOT:-}; SIGN=; NOTARIZE=
while [ $# -gt 0 ]; do
  case "$1" in
    --data-only) DATA_ONLY=1;;
    --zip) ZIP=1;;
    --universal) UNIVERSAL=1;;
    --suffix) SUFFIX=-${2:?--suffix <text>}; shift;;
    --presets) IFS=', ' read -r -a PRESETS <<< "${2:?--presets <names>}"; shift;;
    --core-dir) CORE_DIR=${2:?}; shift;;
    --godot) GODOT=${2:?}; shift;;
    --sign) SIGN=${2:?--sign <identity>}; shift;;
    --notarize) NOTARIZE=${2:?--notarize <keychain profile>}; shift;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    *) echo "export.sh: unknown option $1" >&2; exit 2;;
  esac; shift
done
[ -z "$NOTARIZE" ] || [ -n "$SIGN" ] || { echo "export.sh: --notarize needs --sign" >&2; exit 2; }

OUT=$REPO/dist/FlightBench-macos; BUNDLE=$OUT/FlightBench.app
RES=$BUNDLE/Contents/Resources; DATA=$RES/data; HELPERS=$BUNDLE/Contents/Helpers; FW=$BUNDLE/Contents/Frameworks

if [ -z "$DATA_ONLY" ]; then
  if [ -z "$GODOT" ]; then
    for c in "$(command -v godot || true)" /Applications/Godot_mono.app/Contents/MacOS/Godot /Applications/Godot.app/Contents/MacOS/Godot; do
      [ -n "$c" ] && [ -x "$c" ] && { GODOT=$c; break; }
    done
  fi
  [ -n "$GODOT" ] && [ -x "$GODOT" ] || { echo "export.sh: Godot 4.7 .NET binary not found (--godot, GODOT, or godot on PATH)" >&2; exit 1; }
  # Godot's .NET export publishes with /usr/local/share/dotnet/dotnet when that exists (GodotTools.DotNetFinder): it must
  # carry an SDK 8 (`brew install --cask dotnet-sdk@8`), a Homebrew dotnet@8 on PATH alone is not seen by the export.
  mkdir -p "$OUT"; rm -rf "$BUNDLE"
  "$GODOT" --headless --path "$APP" --export-release macOS "$BUNDLE" 2>&1 | grep -viE 'editor_(fonts|theme)|HotReloadAssemblyWatcher|godot_icall|NativeCalls|CSharpInstanceBridge|ScriptMethods|at: start \(scene/main/timer|Unable to start the timer|Case mismatch|open_internal|^\[ *[0-9]+% \]|\[ DONE \]|^\[0m$|^$' || true
  [ -x "$BUNDLE/Contents/MacOS/FlightBench" ] || { echo "export.sh: export produced no $BUNDLE/Contents/MacOS/FlightBench" >&2; exit 1; }
  # Godot exits 0 even when its .NET publish failed (the bundle then has no data_FlightBench_macos_* and crashes at start)
  ls -d "$RES"/data_FlightBench_macos_* >/dev/null 2>&1 || { echo "export.sh: no .NET publish output in the bundle (Resources/data_FlightBench_macos_*): the C# build failed — see the dotnet errors above / Godot's MSBuild log" >&2; exit 1; }
  # Godot's official macOS template is universal only (an `arm64` preset is refused: no godot_macos_release.arm64), so
  # the export is universal; the core is arm64, so by default the app is thinned to arm64 too: the Godot binary's
  # x86_64 slice and the x86_64 .NET runtime go (about a third of the bundle). --universal keeps them.
  if [ -z "$UNIVERSAL" ]; then
    exe=$BUNDLE/Contents/MacOS/FlightBench
    if lipo "$exe" -verify_arch x86_64 2>/dev/null; then lipo "$exe" -thin arm64 -output "$exe.arm64" && mv "$exe.arm64" "$exe"; fi
    rm -rf "$RES"/data_*_x86_64
    echo "arch: arm64 only ($(lipo -archs "$exe")); --universal keeps the x86_64 half"
  fi
fi
[ -d "$BUNDLE" ] || { echo "export.sh: no bundle at $BUNDLE (run without --data-only first)" >&2; exit 1; }

# ---- data/: rebuilt from scratch, only what the app reads from boards/ and models/ (export.ps1's rules) ----
rm -rf "$DATA"; mkdir -p "$DATA"
copy_rel() { local src=$1 rel=${1#"$REPO"/}; mkdir -p "$DATA/$(dirname "$rel")"; cp "$src" "$DATA/$rel"; }
for b in "$REPO"/boards/*/; do
  case "$(basename "$b")" in .*) continue;; esac
  for f in "$b"*; do [ -f "$f" ] || continue
    case "$(basename "$f")" in *.config|*.cli|board.json|README.md) copy_rel "$f";; esac
  done
done
for m in "$REPO"/models/*/; do
  case "$(basename "$m")" in .*) continue;; esac
  for f in "$m"*; do [ -f "$f" ] || continue
    case "$(basename "$f")" in aircraft.json|README.md|*.cli) copy_rel "$f";; esac
  done
  for f in "$m"env/*.json "$m"visual/*.tscn; do [ -f "$f" ] && copy_rel "$f"; done
  while IFS= read -r f; do copy_rel "$f"; done < <(find "$m" -type f -name '*profile*.txt')
done
# presets and the firmware they name (paths in a preset are relative to the data root)
match_preset() { local n=$1 p; for p in "${PRESETS[@]}"; do [ "$p" = "*" ] && return 0; case "$n" in $p) return 0;; esac; done; return 1; }
declare -a KEEP_FW=(); kept=0; left=()
for f in "$REPO"/presets/*.json; do
  n=$(basename "$f" .json)
  if match_preset "$n"; then
    copy_rel "$f"; kept=$((kept+1))
    fw=$(python3 -c 'import json,sys; v=json.load(open(sys.argv[1])).get("firmwarePath") or ""; print(v.replace("\\","/").removeprefix("./"))' "$f")
    [ -n "$fw" ] && KEEP_FW+=("$fw")
  else left+=("$(basename "$f")"); fi
done
[ -f "$REPO/presets/README.md" ] && copy_rel "$REPO/presets/README.md"
[ "$kept" -gt 0 ] || { echo "export.sh: --presets ${PRESETS[*]} matches no preset in $REPO/presets" >&2; exit 1; }
[ ${#left[@]} -eq 0 ] || echo "presets: not shipped (--presets ${PRESETS[*]}): ${left[*]}"
dropped=()
while IFS= read -r f; do
  rel=${f#"$REPO"/}
  if [ "$(basename "$f")" = FIRMWARE.md ] || [ "${PRESETS[0]}" = "*" ]; then copy_rel "$f"; continue; fi
  keep=; for k in "${KEEP_FW[@]}"; do [ "$k" = "$rel" ] && keep=1; done
  if [ -n "$keep" ]; then copy_rel "$f"; else dropped+=("${rel#firmware/}"); fi
done < <(find "$REPO/firmware" -type f)
[ ${#dropped[@]} -eq 0 ] || echo "firmware: not shipped (no kept preset names it): ${dropped[*]}"
# every relative path a preset names must be in data/ now
for f in "$DATA"/presets/*.json; do
  python3 - "$f" "$DATA" <<'PY'
import json, os, sys
pf, data = sys.argv[1], sys.argv[2]; pj = json.load(open(pf))
for k in ("firmwarePath", "boardConfigFile", "profileFile", "visual"):
    v = pj.get(k)
    if v and not os.path.exists(os.path.join(data, v)): sys.exit(f"preset {os.path.basename(pf)}: {k} '{v}' is not in {data}")
print(f"  preset {os.path.basename(pf)}: '{pj.get('name')}' - paths present in data/")
PY
done
echo "data: $(find "$DATA" -type f | wc -l | tr -d ' ') files, $(du -sh "$DATA" | cut -f1) -> $DATA"

# ---- the core: Contents/Helpers/qemu-system-arm + Contents/Frameworks/*.dylib ----
rm -rf "$HELPERS"; rm -f "$FW"/lib*.dylib
if [ -x "$CORE_DIR/qemu-system-arm" ]; then
  mkdir -p "$HELPERS" "$FW"
  cp "$CORE_DIR/qemu-system-arm" "$HELPERS/"
  [ -d "$CORE_DIR/lib" ] && cp "$CORE_DIR"/lib/*.dylib "$FW/"
  [ -f "$CORE_DIR/CORE-MANIFEST.txt" ] && cp "$CORE_DIR/CORE-MANIFEST.txt" "$DATA/CORE-MANIFEST.txt"
  echo "core: $CORE_DIR -> Contents/Helpers (+ $(ls "$FW"/lib*.dylib 2>/dev/null | wc -l | tr -d ' ') dylibs in Contents/Frameworks)"
  head -3 "$CORE_DIR/CORE-MANIFEST.txt" 2>/dev/null | sed 's/^/  | /'
else
  echo "core: none bundled ($CORE_DIR/qemu-system-arm not found; --core-dir or CORE_DIR; core/qemu/tools/package-mac.sh makes it)"
fi

# ---- LICENSE + README.txt next to the bundle (a GPL distribution of QEMU and of FlightBench) ----
version=dev; v=$(sed -n 's/^config\/version="\([^"]*\)"/\1/p' "$APP/project.godot"); [ -n "$v" ] && version="v$v"
commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no 2>/dev/null)" ] || commit="$commit+dirty"
date=$(date -u +%Y-%m-%d)
cp "$REPO/LICENSE" "$OUT/LICENSE"
sed -e "s/{{VERSION}}/$version/g" -e "s/{{COMMIT}}/$commit/g" -e "s/{{DATE}}/$date/g" "$HERE/dist-README-macos.txt" > "$OUT/README.txt"
# the bundle's version fields follow project.godot
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${version#v}" -c "Set :CFBundleVersion ${version#v}" "$BUNDLE/Contents/Info.plist" 2>/dev/null || true
echo "dist: LICENSE + README.txt ($version, $commit, $date) -> $OUT"

# ---- signing: inside-out, every Mach-O ----
ID=${SIGN:--}   # "-" = ad hoc
# the hardened runtime (and with it library validation: every dylib must carry the same Team ID) only with a real
# identity; an ad-hoc signature has no Team ID, so a hardened ad-hoc core could not load its own Frameworks/
if [ -n "$SIGN" ]; then sign() { codesign --force --timestamp --options runtime --sign "$ID" "$@"; }
else sign() { codesign --force --sign - "$@"; }; fi
[ -n "$SIGN" ] && echo "codesign: $SIGN" || echo "codesign: ad hoc (no --sign): runs here, not on another Mac"
is_macho() { [ "$(head -c 4 "$1" | xxd -p)" = cffaedfe ] || [ "$(head -c 4 "$1" | xxd -p)" = cafebabe ]; }
# 1. libraries and the .NET runtime's native parts (anywhere in the bundle but the main executable and the core)
while IFS= read -r f; do
  case "$f" in "$BUNDLE/Contents/MacOS/FlightBench"|"$HELPERS/qemu-system-arm") continue;; esac
  is_macho "$f" && sign "$f" 2>&1 | grep -v 'replacing existing signature' || true
done < <(find "$BUNDLE" -type f)
# 2. the core with the JIT entitlements
[ -x "$HELPERS/qemu-system-arm" ] && sign --entitlements "$REPO/core/qemu/tools/qemu-entitlements.plist" "$HELPERS/qemu-system-arm" 2>&1 | grep -v 'replacing existing signature' || true
# 3. the app itself (its entitlements: .NET's JIT)
sign --entitlements "$HERE/macos-entitlements.plist" "$BUNDLE" 2>&1 | grep -v 'replacing existing signature' || true
codesign --verify --deep --strict "$BUNDLE" && echo "codesign: bundle verifies"

if [ -n "$NOTARIZE" ]; then
  tmpzip=$(mktemp -d)/FlightBench.zip
  ditto -c -k --keepParent "$BUNDLE" "$tmpzip"
  echo "notarytool: submitting $(du -h "$tmpzip" | cut -f1) (profile $NOTARIZE) ..."
  xcrun notarytool submit "$tmpzip" --keychain-profile "$NOTARIZE" --wait
  xcrun stapler staple "$BUNDLE"
  spctl --assess --type execute -vv "$BUNDLE" && echo "gatekeeper: accepted"
  rm -rf "$(dirname "$tmpzip")"
fi

echo "build: $(find "$OUT" -type f | wc -l | tr -d ' ') files, $(du -sh "$OUT" | cut -f1) -> $OUT"
if [ -n "$ZIP" ]; then
  zip=$REPO/dist/FlightBench-macos-$version$SUFFIX.zip; rm -f "$zip"
  (cd "$REPO/dist" && ditto -c -k --keepParent FlightBench-macos "$zip")
  echo "zip: $(du -h "$zip" | cut -f1) -> $zip"
fi
