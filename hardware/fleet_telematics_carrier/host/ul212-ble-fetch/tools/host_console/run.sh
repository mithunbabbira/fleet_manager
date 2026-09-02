#!/bin/sh
# Run UL212 Host Console from anywhere.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1

PY=""
for c in python3 /Library/Frameworks/Python.framework/Versions/3.14/bin/python3 /usr/bin/python3; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -m pip --version >/dev/null 2>&1; then
    PY=$c
    break
  fi
done
if [ -z "$PY" ]; then
  echo "Need python3 with pip. Try: python3 -m pip install -r tools/host_console/requirements.txt"
  exit 1
fi

"$PY" -m pip install -q -r tools/host_console/requirements.txt
exec "$PY" tools/host_console/app.py "$@"
