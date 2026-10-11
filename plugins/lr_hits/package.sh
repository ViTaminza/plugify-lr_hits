#!/bin/bash
# Assembles build/package/lr_hits: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/lr_hits"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/lr_hits/liblr_hits.so" "$OUT/bin/"
cp "$ROOT/build/plugins/lr_hits/lr_hits.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
