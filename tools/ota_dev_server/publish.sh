#!/usr/bin/env bash
# Publish current build .bin into tools/ota_dev_server/firmware/<VERSION>/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VER="$(tr -d '[:space:]' < "$ROOT/VERSION")"
BIN="$ROOT/build/elm327_esp32c6.bin"
DEST="$ROOT/tools/ota_dev_server/firmware/$VER"

if [[ ! -f "$BIN" ]]; then
  echo "missing $BIN — run idf.py build first" >&2
  exit 1
fi

mkdir -p "$DEST"
cp "$BIN" "$DEST/elm327_esp32c6.bin"
shasum -a 256 "$DEST/elm327_esp32c6.bin" | awk '{print $1}' > "$DEST/elm327_esp32c6.bin.sha256"
echo "published $VER ($(wc -c < "$DEST/elm327_esp32c6.bin") bytes)"
echo "  $DEST/elm327_esp32c6.bin"
cat "$DEST/elm327_esp32c6.bin.sha256"
