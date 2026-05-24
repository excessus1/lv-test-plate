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
- `lv-test-plate/state`: board publishes retained output state
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

The app subscribes to the board `state`, `telemetry`, and `status` topics, keeps the latest known values in memory, and serves live updates to the browser with a WebSocket.

## Dashboard Controls

The dashboard provides large touch targets for:

- `relay_1` on/off
- `relay_2` on/off
- `ssr_1` on/off
- PWM enable on/off
- PWM value `0` through `255`
- prominent `All Outputs Off`

It displays:

- app MQTT connection status
- board online/offline status
- latest output state
- uptime
- WiFi RSSI
- analog potentiometer input
- last command received by the board

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

Board state messages:

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
  "last_command": "{\"outputs\":{\"relay_1\":true}}"
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

Default pins are at the top of `firmware/lv-test-plate/lv-test-plate.ino`:

```cpp
const int RELAY_1_PIN = 2;  // D2
const int RELAY_2_PIN = 3;  // D3
const int SSR_1_PIN = 4;    // D4
const int PWM_PIN = 5;      // D5
const int POT_PIN = A0;     // A0
```

If a relay or SSR board is active-low, change:

```cpp
const int OUTPUT_ON_LEVEL = HIGH;
const int OUTPUT_OFF_LEVEL = LOW;
```

## Compile Firmware

The default FQBN is `arduino:renesas_uno:unor4wifi`.

```sh
scripts/compile-firmware.sh
```

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
- Board `state` and `status` are retained so the dashboard can show the latest known state on page load.
- If MQTT disconnects after previously being connected, the firmware forces outputs off before trying to reconnect.
- This is for low-voltage bench testing, not a finished enclosure or production safety system.

## Local MQTT Smoke Test

With the app running, you can simulate board messages from another shell:

```sh
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/status -r -m online
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/telemetry -m '{"uptime_ms":5000,"wifi_rssi":-55,"pot_value":321,"wifi_connected":true,"mqtt_connected":true}'
mosquitto_pub -h "$MQTT_HOST" -t lv-test-plate/state -r -m '{"outputs":{"relay_1":false,"relay_2":false,"ssr_1":false,"pwm_enabled":false,"pwm_value":0},"uptime_ms":5000,"last_command":"manual smoke test"}'
```

Watch commands from the dashboard:

```sh
mosquitto_sub -h "$MQTT_HOST" -t lv-test-plate/cmd -v
```
