#!/usr/bin/env bash
set -euo pipefail

FQBN="${FQBN:-arduino:renesas_uno:unor4wifi}"
SKETCH_DIR="${SKETCH_DIR:-firmware/lv-test-plate}"
ARDUINO_PORT="${ARDUINO_PORT:-/dev/ttyACM0}"

echo "Uploading to ${ARDUINO_PORT}. If upload fails, unplug/replug the board and re-check the port with: arduino-cli board list"
arduino-cli upload -p "$ARDUINO_PORT" --fqbn "$FQBN" "$SKETCH_DIR"

