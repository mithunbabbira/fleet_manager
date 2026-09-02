#!/usr/bin/env sh
cd "$(dirname "$0")/../.." || exit 1
exec python3 tools/carrier_console/app.py "$@"
