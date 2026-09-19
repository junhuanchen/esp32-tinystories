#!/usr/bin/env bash
# Build the standalone ESP-IDF firmware. Flashing is opt-in because it replaces
# the current partition table, application, and model on the connected board.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$ROOT/idf"
FLASH=0
MONITOR=0
PORT=""
IDF_DIR="${IDF_PATH:-}"

usage() {
  cat >&2 <<'EOF'
usage: scripts/build_idf.sh [--flash --port /dev/ttyACM0 [--monitor]] [--idf-path PATH]

Builds the ESP-IDF TinyStories firmware. --flash is required before any board
write; it flashes both the firmware and the model partition.
EOF
  exit 2
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --flash) FLASH=1 ;;
    --monitor) MONITOR=1 ;;
    --port) shift; [ "$#" -gt 0 ] || usage; PORT=$1 ;;
    --idf-path) shift; [ "$#" -gt 0 ] || usage; IDF_DIR=$1 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
  shift
done

[ -f "$PROJECT/CMakeLists.txt" ] || { echo "ESP-IDF project not found: $PROJECT" >&2; exit 1; }

if ! command -v idf.py >/dev/null 2>&1; then
  if [ -z "$IDF_DIR" ]; then IDF_DIR="$HOME/esp/esp-idf"; fi
  [ -f "$IDF_DIR/export.sh" ] || {
    echo "ESP-IDF was not found. Set IDF_PATH or pass --idf-path /path/to/esp-idf." >&2
    exit 1
  }
  # shellcheck disable=SC1090
  . "$IDF_DIR/export.sh"
fi
command -v idf.py >/dev/null 2>&1 || { echo "ESP-IDF activation did not provide idf.py" >&2; exit 1; }

cd "$PROJECT"
idf.py set-target esp32s3
idf.py build

if [ "$MONITOR" -eq 1 ] && [ "$FLASH" -ne 1 ]; then
  echo "--monitor requires --flash and --port /dev/ttyACM0." >&2
  exit 2
fi
if [ "$FLASH" -ne 1 ]; then
  echo "build complete; add --flash --port /dev/ttyACM0 to write the board."
  exit 0
fi
[ -n "$PORT" ] || { echo "--flash requires --port /dev/ttyACM0." >&2; exit 2; }

idf.py -p "$PORT" flash
if [ "$MONITOR" -eq 1 ]; then idf.py -p "$PORT" monitor; fi
