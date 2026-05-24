#!/usr/bin/env bash
set -euo pipefail

FQBN="${FQBN:-arduino:renesas_uno:unor4wifi}"
SKETCH_DIR="${SKETCH_DIR:-firmware/lv-test-plate}"

arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"

