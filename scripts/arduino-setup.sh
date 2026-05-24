#!/usr/bin/env bash
set -euo pipefail

arduino-cli core update-index
arduino-cli core install arduino:renesas_uno
arduino-cli lib install ArduinoMqttClient
arduino-cli lib install ArduinoJson

echo "Arduino Uno R4 WiFi tooling is ready."

