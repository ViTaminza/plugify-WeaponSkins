#!/bin/bash
# Assembles build/package/WeaponSkins: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/WeaponSkins"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/WeaponSkins/libWeaponSkins.so" "$OUT/bin/"
cp "$ROOT/build/plugins/WeaponSkins/WeaponSkins.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
