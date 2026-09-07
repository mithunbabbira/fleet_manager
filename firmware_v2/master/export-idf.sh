#!/usr/bin/env bash
# Activate the ESP-IDF install this project expects (5.2.3).
# Usage: source firmware_v2/master/export-idf.sh
set -euo pipefail
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
if [[ ! -f "$IDF_EXPORT" ]]; then
  echo "export-idf: missing $IDF_EXPORT (need ESP-IDF 5.2.3)" >&2
  return 1 2>/dev/null || exit 1
fi
# shellcheck disable=SC1090
source "$IDF_EXPORT"
VER="$(idf.py --version 2>/dev/null || true)"
echo "export-idf: $VER"
case "$VER" in
  *5.2.3*) ;;
  *)
    echo "export-idf: WARNING — expected ESP-IDF v5.2.3; got: ${VER:-unknown}" >&2
    echo "export-idf: IDF 6.x will fail on REQUIRES json for this tree." >&2
    ;;
esac
