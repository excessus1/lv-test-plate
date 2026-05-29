# lv-test-plate

First-pass low-voltage Arduino Uno R4 WiFi testing plate control system.

The vertical slice is intentionally small:

- Dockerized FastAPI web dashboard on Gimli
- MQTT commands through the existing broker on Sauron
- Arduino Uno R4 WiFi firmware for relay, SSR, PWM, and telemetry
- Mobile/tablet-first controls for bench work
- No database and no bundled MQTT broker

## Architecture

```text
phone/tablet browser
        |
        v
Docker FastAPI app on Gimli :8088
        |
        v
MQTT broker on Sauron
        |
        v
Arduino Uno R4 WiFi test plate
```

Default topics use `MQTT_BASE_TOPIC=lv-test-plate`:

- `lv-test-plate/cmd`: web app publishes non-retained output commands
- `lv-test-plate/state`: board publishes retained dynamic output state and timing metadata
- `lv-test-plate/capabilities`: board publishes retained static/semi-static board metadata
- `lv-test-plate/switch`: board publishes retained compact per-channel switch config/read/test events
- `lv-test-plate/telemetry`: board publishes periodic telemetry
- `lv-test-plate/status`: board publishes retained `online`; MQTT last will publishes retained `offline`

## Repository Layout

```text
.
├── app/                         # FastAPI app and static dashboard
├── firmware/lv-test-plate/      # Arduino CLI sketch
├── scripts/                     # Arduino CLI helper scripts
├── Dockerfile
├── compose.yaml
├── .env.example
└── README.md
```

## Host Setup

Copy the environment example and edit the MQTT settings for Gimli/Sauron:

```sh
cp .env.example .env
```

Required app environment variables:

```sh
MQTT_HOST=sauron.local
MQTT_PORT=1883
MQTT_USERNAME=
MQTT_PASSWORD=
MQTT_BASE_TOPIC=lv-test-plate
APP_PORT=8088
```

## Run the Web App

From the repo root:

```sh
docker compose up --build
```

Open:

```text
http://gimli:8088/
```

or, from the host itself:

```text
http://localhost:8088/
```

The app subscribes to the board `state`, `capabilities`, `switch`, `telemetry`, and `status` topics, keeps the latest known values in memory, and serves live updates to the browser with a WebSocket.

## Dashboard Controls

The dashboard provides large touch targets for:

- `relay_1` on/off
- `relay_2` on/off
- `ssr_1` on/off
- PWM enable on/off
- PWM value `0` through `255`
- prominent `All Outputs Off`

Each relay also has an expandable switch-test section. Switch testing is optional per relay; channels with no switch input configured continue to work as normal relay outputs.

Per-relay switch-test settings:

- enabled/disabled
- input pin selected from the firmware-published pin options
- test type: `passive_read`, `relay_follow`, `timed_observation`, or `output_feedback`
- switch mode: `NO` or `NC`
- pull mode: `pullup`, `pulldown`, or `external`
- debounce time in milliseconds
- observation duration in seconds for timed observation
- settle delay in milliseconds for output-driven feedback tests
- read-now, timed observation, and output feedback actions

It displays:

- app MQTT connection status
- board online/offline status
- latest output state
- uptime
- WiFi RSSI
- analog potentiometer input
- last command received by the board
- current switch open/closed state and raw digital state for configured relay switch tests
- relay-follow ownership/status, timed observation summaries, and last output feedback result

Switch test types:

- `passive_read`: reads and interprets the configured switch input without controlling the relay. Manual relay control remains available.
- `relay_follow`: the interpreted switch closed/active state drives the associated relay continuously. Manual relay control is disabled while this mode owns the relay.
- `timed_observation`: watches the input for a human-facing duration, counts transitions, and records starting/ending state without requiring relay output.
- `output_feedback`: preserves the output-driven feedback test: read before, turn the relay on, wait `settle_ms`, read after, then turn the relay off and report pass/fail.

For current bench validation, prefer `relay_2` with D6, `NO`, and `pullup`: D6 open reads HIGH/open/off, and D6 grounded reads LOW/closed/on. Relay 1 has known hardware issues right now.

## MQTT Payloads

Command messages published by the web app:

```json
{
  "outputs": {
    "relay_1": true,
    "relay_2": false,
    "ssr_1": true,
    "pwm_enabled": true,
    "pwm_value": 128
  },
  "source": "web",
  "ts": 1716500000.0
}
```

Switch-test configuration and actions are additive top-level command fields. Existing output-only commands remain valid.

```json
{
  "switch_tests": {
    "relay_2": {
      "enabled": true,
      "pin": 6,
      "test_mode": "relay_follow",
      "mode": "NO",
      "pull_mode": "pullup",
      "debounce_ms": 30,
      "settle_ms": 150,
      "observation_duration_s": 30
    }
  },
  "read_switch": "relay_2",
  "observe_switch": "relay_2",
  "test_switch": "relay_2",
  "source": "web",
  "ts": 1716500000.0
}
```

Use `read_switch` in any test type. Use `observe_switch` with `timed_observation`. Use `test_switch` with `output_feedback`; it is intentionally not the default basic switch workflow.

Board capabilities messages are retained static/semi-static board metadata. The sketch is authoritative for pin assignments and switch input options; docs may lag behind the `.ino`.

```json
{
  "sketch": "lv-test-plate",
  "firmware_version": "2026-05-29-switch-modes-1",
  "supported_switch_modes": ["NO", "NC"],
  "supported_pull_modes": ["pullup", "pulldown", "external"],
  "supported_switch_test_modes": ["passive_read", "relay_follow", "timed_observation", "output_feedback"],
  "output_pins": {
    "relay_1": {"pin": 8, "label": "D8"},
    "relay_2": {"pin": 13, "label": "D13"},
    "ssr_1": {"pin": 3, "label": "D3"},
    "pwm": {"pin": 5, "label": "D5"}
  },
  "input_pins": {
    "pot": {"pin": 14, "label": "A0"}
  },
  "switch_input_pin_options": [
    {"pin": 2, "label": "D2"},
    {"pin": 4, "label": "D4"}
  ]
}
```

Board state messages are retained dynamic output state. Static board metadata such as `switch_input_pin_options` belongs on `lv-test-plate/capabilities`; compact switch configuration/read/test results belong on `lv-test-plate/switch`. The app still accepts `switch_input_pin_options` and `switch_tests` in state for older firmware.

```json
{
  "outputs": {
    "relay_1": true,
    "relay_2": false,
    "ssr_1": false,
    "pwm_enabled": true,
    "pwm_value": 128
  },
  "uptime_ms": 12000,
  "last_command": "{\"outputs\":{\"relay_1\":true}}",
  "switch_tests_included": false
}
```

Switch event messages are retained compact per-channel updates. They echo `command_id` and monotonic `command_seq` so the app can ignore stale channel updates.

```json
{
  "event": "switch_read",
  "channel": "relay_2",
  "command_id": "6c9418a7-9d3f-4a8e-9284-882b8dfb10d2",
  "command_seq": 42,
  "switch_tests": {
    "relay_2": {
      "enabled": true,
      "pin": 6,
      "pin_label": "D6",
      "test_mode": "relay_follow",
      "mode": "NO",
      "pull_mode": "pullup",
      "effective_pull_mode": "pullup",
      "debounce_ms": 30,
      "settle_ms": 150,
      "observation_duration_s": 30,
      "configured": true,
      "relay_follow_enabled": true,
      "commanded_by": "switch",
      "status": "relay_following",
      "current": {
        "configured": true,
        "raw": 1,
        "raw_state": "HIGH",
        "closed": false,
        "state": "open",
        "bounce_count": 0,
        "sampled_at_ms": 12000
      },
      "observation": {
        "active": false,
        "available": true,
        "status": "activity_observed",
        "duration_s": 30,
        "remaining_ms": 0,
        "transition_count": 5,
        "open_to_closed_count": 3,
        "closed_to_open_count": 2
      },
      "last_test": {
        "available": false,
        "status": "not_run",
        "pass": false,
        "changed": false,
        "settle_ms": 150,
        "before": {"state": "unconfigured", "raw_state": "n/a", "closed": false},
        "after": {"state": "unconfigured", "raw_state": "n/a", "closed": false}
      }
    }
  },
  "uptime_ms": 12000
}
```

Telemetry messages:

```json
{
  "uptime_ms": 12000,
  "wifi_rssi": -54,
  "pot_value": 512,
  "wifi_connected": true,
  "mqtt_connected": true
}
```

## Firmware Setup

Install Arduino CLI first, then run:

```sh
scripts/arduino-setup.sh
```

Copy the firmware config example:

```sh
cp firmware/lv-test-plate/config.example.h firmware/lv-test-plate/config.h
```

Edit `firmware/lv-test-plate/config.h` with WiFi and MQTT values:

```cpp
const char WIFI_SSID[] = "your-wifi-ssid";
const char WIFI_PASSWORD[] = "your-wifi-password";

const char MQTT_HOST[] = "sauron.local";
const int MQTT_PORT = 1883;
const char MQTT_USERNAME[] = "";
const char MQTT_PASSWORD[] = "";
const char MQTT_BASE_TOPIC[] = "lv-test-plate";
const char MQTT_CLIENT_ID[] = "lv-test-plate-uno-r4";
```

`config.h` is gitignored so credentials are not committed. The sketch can compile with `config.example.h`, but create `config.h` with real values before uploading to hardware.

## Firmware Pin Mapping

The active pin assignments at the top of `firmware/lv-test-plate/lv-test-plate.ino` are authoritative. Check the sketch before changing wiring or docs.

```cpp
const int RELAY_1_PIN = 8;      // D8, digital relay output
const int RELAY_2_PIN = 13;     // D13, digital relay output
const int SSR_1_PIN = 3;        // D3, SSR output
const int PWM_PIN = 5;          // D5, PWM-capable output
const int POT_PIN = A0;         // A0, analog potentiometer input
```

Switch input options are also defined in the sketch and published in retained board capabilities as `switch_input_pin_options`. The current default options exclude the active output and potentiometer pins: `D2`, `D4`, `D6`, `D7`, `D9`, `D10`, `D11`, `D12`, `A1`, `A2`, `A3`, `A4`, and `A5`.

Relay module and SSR polarity are configured separately. If either device is active-low, change only that device's pair:

```cpp
const int RELAY_ON_LEVEL = HIGH;
const int RELAY_OFF_LEVEL = LOW;
const int SSR_ON_LEVEL = HIGH;
const int SSR_OFF_LEVEL = LOW;
```

## Compile Firmware

The default FQBN is `arduino:renesas_uno:unor4wifi`.

```sh
scripts/compile-firmware.sh
```

The firmware includes temporary stability isolation flags for MQTT payload debugging. By default, retained capabilities publishing and full `switch_tests` payloads inside `lv-test-plate/state` are disabled in the sketch; compact per-channel switch updates still publish on `lv-test-plate/switch`.

```cpp
#define LVTP_ENABLE_CAPABILITIES_PUBLISH 0
#define LVTP_ENABLE_FULL_SWITCH_TEST_STATE 0
```

Set either flag to `1` at compile time or in the sketch when re-enabling that payload after idle stability is confirmed.

You can override it:

```sh
FQBN=arduino:renesas_uno:unor4wifi scripts/compile-firmware.sh
```

## Upload Firmware

Default upload port is `/dev/ttyACM0`:

```sh
scripts/upload-firmware.sh
```

Override the port when needed:

```sh
ARDUINO_PORT=/dev/ttyACM1 scripts/upload-firmware.sh
```

The Uno R4 WiFi port may change during upload. If upload fails, check:

```sh
arduino-cli board list
```

## Safety Notes

- All outputs are forced off on boot before network setup.
- Commands are not retained, which avoids replaying stale output commands after reconnect.
- Board `state`, `capabilities`, and `status` are retained so the dashboard can show the latest known dynamic state and static board metadata on page load.
- If MQTT disconnects after previously being connected, the firmware forces outputs off before trying to reconnect.
- This is for low-voltage bench testing, not a finished enclosure or production safety system.

## Local MQTT Smoke Test

With the app running, you can simulate board messages from another shell:

```sh
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/status -r -m online
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/capabilities -r -m '{"sketch":"lv-test-plate","firmware_version":"smoke-test","supported_switch_modes":["NO","NC"],"supported_pull_modes":["pullup","pulldown","external"],"supported_switch_test_modes":["passive_read","relay_follow","timed_observation","output_feedback"],"switch_input_pin_options":[{"pin":2,"label":"D2"},{"pin":4,"label":"D4"},{"pin":6,"label":"D6"}]}'
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/telemetry -m '{"uptime_ms":5000,"wifi_rssi":-55,"pot_value":321,"wifi_connected":true,"mqtt_connected":true}'
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/state -r -m '{"outputs":{"relay_1":false,"relay_2":false,"ssr_1":false,"pwm_enabled":false,"pwm_value":0},"uptime_ms":5000,"last_command":"manual smoke test"}'
```

Watch commands from the dashboard:

```sh
mosquitto_sub -h "$MQTT_HOST" -t lv-test-plate/cmd -v
```
