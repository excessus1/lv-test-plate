from __future__ import annotations

import asyncio
import json
import logging
import os
import socket
import time
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from threading import Lock
from typing import Any

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, Response
from fastapi.staticfiles import StaticFiles

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger("lv-test-plate")

APP_DIR = Path(__file__).resolve().parent
STATIC_DIR = APP_DIR / "static"
ASSET_VERSION = os.getenv("ASSET_VERSION", str(int(time.time())))
SWITCH_CHANNELS = ("relay_1", "relay_2")
SWITCH_MODES = {"NO", "NC"}
SWITCH_PULL_MODES = {"pullup", "pulldown", "external"}
SWITCH_TEST_MODES = {"passive_read", "relay_follow", "timed_observation", "output_feedback"}
DEFAULT_SWITCH_DEBOUNCE_MS = 30
DEFAULT_SWITCH_SETTLE_MS = 150
DEFAULT_OBSERVATION_DURATION_S = 30


@dataclass(frozen=True)
class Settings:
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str
    mqtt_password: str
    mqtt_base_topic: str

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(
            mqtt_host=os.getenv("MQTT_HOST", "sauron.local"),
            mqtt_port=int(os.getenv("MQTT_PORT", "1883")),
            mqtt_username=os.getenv("MQTT_USERNAME", ""),
            mqtt_password=os.getenv("MQTT_PASSWORD", ""),
            mqtt_base_topic=os.getenv("MQTT_BASE_TOPIC", "lv-test-plate").strip("/"),
        )

    @property
    def topics(self) -> dict[str, str]:
        base = self.mqtt_base_topic
        return {
            "cmd": f"{base}/cmd",
            "state": f"{base}/state",
            "capabilities": f"{base}/capabilities",
            "telemetry": f"{base}/telemetry",
            "status": f"{base}/status",
        }


class DashboardState:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self._lock = Lock()
        self._state: dict[str, Any] = {
            "app": {
                "mqtt_connected": False,
                "mqtt_host": settings.mqtt_host,
                "mqtt_port": settings.mqtt_port,
                "base_topic": settings.mqtt_base_topic,
                "last_error": None,
            },
            "board": {
                "online": False,
                "status": "unknown",
                "last_status_at": None,
            },
            "outputs": {
                "relay_1": False,
                "relay_2": False,
                "ssr_1": False,
                "pwm_enabled": False,
                "pwm_value": 0,
            },
            "switch_tests": _default_switch_tests(),
            "capabilities": _default_capabilities(),
            "switch_input_pin_options": [],
            "telemetry": {
                "uptime_ms": None,
                "wifi_rssi": None,
                "pot_value": None,
                "wifi_connected": None,
                "mqtt_connected": None,
                "last_received_at": None,
            },
            "last_command": None,
            "messages": {
                "state": None,
                "capabilities": None,
                "telemetry": None,
                "status": None,
            },
        }

    def _snapshot_locked(self) -> dict[str, Any]:
        snapshot = json.loads(json.dumps(self._state))
        snapshot["server_time"] = time.time()
        return snapshot

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return self._snapshot_locked()

    def update_app(self, **values: Any) -> dict[str, Any]:
        with self._lock:
            self._state["app"].update(values)
            return self._snapshot_locked()

    def apply_outputs(self, outputs: dict[str, Any], last_command: Any | None = None) -> dict[str, Any]:
        with self._lock:
            self._state["outputs"].update(_coerce_outputs(outputs))
            if last_command is not None:
                self._state["last_command"] = last_command
            return self._snapshot_locked()

    def apply_switch_tests(self, switch_tests: dict[str, Any], last_command: Any | None = None) -> dict[str, Any]:
        with self._lock:
            _merge_switch_tests(self._state["switch_tests"], switch_tests)
            if last_command is not None:
                self._state["last_command"] = last_command
            return self._snapshot_locked()

    def apply_command_snapshot(
        self,
        outputs: dict[str, Any] | None = None,
        switch_tests: dict[str, Any] | None = None,
        last_command: Any | None = None,
    ) -> dict[str, Any]:
        with self._lock:
            if outputs is not None:
                self._state["outputs"].update(_coerce_outputs(outputs))
            if switch_tests is not None:
                _merge_switch_tests(self._state["switch_tests"], switch_tests)
            if last_command is not None:
                self._state["last_command"] = last_command
            return self._snapshot_locked()

    def apply_mqtt_message(self, topic_key: str, payload: Any) -> dict[str, Any]:
        now = time.time()
        with self._lock:
            self._state["messages"][topic_key] = payload
            if topic_key == "status":
                status = str(payload).strip().lower() if not isinstance(payload, dict) else str(payload.get("status", "unknown")).lower()
                self._state["board"].update(
                    {
                        "online": status == "online",
                        "status": status,
                        "last_status_at": now,
                    }
                )
            elif topic_key == "state" and isinstance(payload, dict):
                outputs = payload.get("outputs")
                if isinstance(outputs, dict):
                    self._state["outputs"].update(_coerce_outputs(outputs))
                switch_tests = payload.get("switch_tests")
                if isinstance(switch_tests, dict):
                    _merge_switch_tests(self._state["switch_tests"], switch_tests)
                pin_options = payload.get("switch_input_pin_options")
                if isinstance(pin_options, list):
                    coerced_pin_options = _coerce_pin_options(pin_options)
                    if not self._state["capabilities"].get("switch_input_pin_options"):
                        self._state["switch_input_pin_options"] = coerced_pin_options
                        self._state["capabilities"]["switch_input_pin_options"] = coerced_pin_options
                        self._state["capabilities"]["source"] = "state"
                if "last_command" in payload:
                    self._state["last_command"] = payload["last_command"]
                elif "last_command_received" in payload:
                    self._state["last_command"] = payload["last_command_received"]
                self._state["telemetry"]["uptime_ms"] = payload.get("uptime_ms", self._state["telemetry"]["uptime_ms"])
            elif topic_key == "capabilities" and isinstance(payload, dict):
                capabilities = _coerce_capabilities(payload, now)
                self._state["capabilities"].update(capabilities)
                pin_options = capabilities.get("switch_input_pin_options")
                if isinstance(pin_options, list):
                    self._state["switch_input_pin_options"] = pin_options
            elif topic_key == "telemetry" and isinstance(payload, dict):
                self._state["telemetry"].update(
                    {
                        "uptime_ms": payload.get("uptime_ms", self._state["telemetry"]["uptime_ms"]),
                        "wifi_rssi": payload.get("wifi_rssi", self._state["telemetry"]["wifi_rssi"]),
                        "pot_value": payload.get("pot_value", self._state["telemetry"]["pot_value"]),
                        "wifi_connected": payload.get("wifi_connected", self._state["telemetry"]["wifi_connected"]),
                        "mqtt_connected": payload.get("mqtt_connected", self._state["telemetry"]["mqtt_connected"]),
                        "last_received_at": now,
                    }
                )
            return self._snapshot_locked()


class WebSocketHub:
    def __init__(self) -> None:
        self._clients: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        async with self._lock:
            self._clients.add(websocket)

    async def disconnect(self, websocket: WebSocket) -> None:
        async with self._lock:
            self._clients.discard(websocket)

    async def broadcast(self, message: dict[str, Any]) -> None:
        async with self._lock:
            clients = list(self._clients)
        for client in clients:
            try:
                await client.send_json(message)
            except Exception:
                await self.disconnect(client)


def _coerce_outputs(raw: dict[str, Any]) -> dict[str, Any]:
    coerced: dict[str, Any] = {}
    for key in ("relay_1", "relay_2", "ssr_1", "pwm_enabled"):
        if key in raw:
            coerced[key] = bool(raw[key])
    if "pwm_value" in raw:
        try:
            coerced["pwm_value"] = max(0, min(255, int(raw["pwm_value"])))
        except (TypeError, ValueError):
            pass
    return coerced


def _default_switch_reading() -> dict[str, Any]:
    return {
        "configured": False,
        "raw": -1,
        "raw_state": "n/a",
        "closed": False,
        "state": "unconfigured",
        "bounce_count": 0,
        "sampled_at_ms": None,
    }


def _default_switch_result() -> dict[str, Any]:
    return {
        "available": False,
        "status": "not_run",
        "pass": False,
        "changed": False,
        "settle_ms": DEFAULT_SWITCH_SETTLE_MS,
        "before": _default_switch_reading(),
        "after": _default_switch_reading(),
    }


def _default_switch_observation() -> dict[str, Any]:
    return {
        "active": False,
        "available": False,
        "status": "not_started",
        "duration_s": DEFAULT_OBSERVATION_DURATION_S,
        "started_at_ms": None,
        "ended_at_ms": None,
        "remaining_ms": 0,
        "transition_count": 0,
        "open_to_closed_count": 0,
        "closed_to_open_count": 0,
        "start": _default_switch_reading(),
        "current": _default_switch_reading(),
        "end": _default_switch_reading(),
    }


def _default_switch_tests() -> dict[str, dict[str, Any]]:
    return {
        channel: {
            "enabled": False,
            "pin": None,
            "pin_label": "",
            "mode": "NO",
            "pull_mode": "pullup",
            "effective_pull_mode": "pullup",
            "test_mode": "passive_read",
            "debounce_ms": DEFAULT_SWITCH_DEBOUNCE_MS,
            "settle_ms": DEFAULT_SWITCH_SETTLE_MS,
            "observation_duration_s": DEFAULT_OBSERVATION_DURATION_S,
            "configured": False,
            "relay_follow_enabled": False,
            "commanded_by": "manual",
            "status": "not_configured",
            "current": _default_switch_reading(),
            "observation": _default_switch_observation(),
            "last_test": _default_switch_result(),
        }
        for channel in SWITCH_CHANNELS
    }


def _default_capabilities() -> dict[str, Any]:
    return {
        "sketch": None,
        "firmware_version": None,
        "supported_switch_modes": [],
        "supported_pull_modes": [],
        "supported_switch_test_modes": [],
        "output_pins": {},
        "input_pins": {},
        "switch_input_pin_options": [],
        "source": None,
        "last_received_at": None,
    }


def _coerce_int(value: Any, default: int, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return default
    return max(minimum, min(maximum, parsed))


def _coerce_switch_config(raw: dict[str, Any], existing: dict[str, Any] | None = None) -> dict[str, Any]:
    base = dict(existing or {})
    coerced: dict[str, Any] = {}

    if "enabled" in raw:
        coerced["enabled"] = bool(raw["enabled"])
    if "pin" in raw:
        coerced["pin"] = None if raw["pin"] in (None, "") else _coerce_int(raw["pin"], -1, -1, 100)
    if "pin_label" in raw:
        coerced["pin_label"] = str(raw.get("pin_label") or "")
    if "mode" in raw:
        mode = str(raw.get("mode", "")).upper()
        if mode in SWITCH_MODES:
            coerced["mode"] = mode
    if "pull_mode" in raw:
        pull_mode = str(raw.get("pull_mode", "")).lower()
        if pull_mode in SWITCH_PULL_MODES:
            coerced["pull_mode"] = pull_mode
    if "test_mode" in raw:
        test_mode = str(raw.get("test_mode", "")).lower()
        if test_mode in SWITCH_TEST_MODES:
            coerced["test_mode"] = test_mode
    if "effective_pull_mode" in raw:
        coerced["effective_pull_mode"] = str(raw.get("effective_pull_mode") or "")
    if "debounce_ms" in raw:
        coerced["debounce_ms"] = _coerce_int(raw["debounce_ms"], int(base.get("debounce_ms", DEFAULT_SWITCH_DEBOUNCE_MS)), 0, 1000)
    if "settle_ms" in raw:
        coerced["settle_ms"] = _coerce_int(raw["settle_ms"], int(base.get("settle_ms", DEFAULT_SWITCH_SETTLE_MS)), 0, 5000)
    if "observation_duration_s" in raw:
        coerced["observation_duration_s"] = _coerce_int(
            raw["observation_duration_s"],
            int(base.get("observation_duration_s", DEFAULT_OBSERVATION_DURATION_S)),
            1,
            3600,
        )
    if "configured" in raw:
        coerced["configured"] = bool(raw["configured"])
    if "relay_follow_enabled" in raw:
        coerced["relay_follow_enabled"] = bool(raw["relay_follow_enabled"])
    if "commanded_by" in raw:
        coerced["commanded_by"] = str(raw.get("commanded_by") or "")
    if "status" in raw:
        coerced["status"] = str(raw.get("status") or "")
    for key in ("current", "observation", "last_test"):
        if isinstance(raw.get(key), dict):
            coerced[key] = raw[key]

    return coerced


def _merge_switch_tests(target: dict[str, Any], raw: dict[str, Any]) -> None:
    for channel in SWITCH_CHANNELS:
        incoming = raw.get(channel)
        if not isinstance(incoming, dict):
            continue
        current = target.setdefault(channel, _default_switch_tests()[channel])
        current.update(_coerce_switch_config(incoming, current))


def _coerce_pin_options(raw: list[Any]) -> list[dict[str, Any]]:
    options: list[dict[str, Any]] = []
    for item in raw:
        if not isinstance(item, dict):
            continue
        if "pin" not in item:
            continue
        pin = _coerce_int(item["pin"], -1, -1, 100)
        if pin < 0:
            continue
        options.append({"pin": pin, "label": str(item.get("label") or f"D{pin}")})
    return options


def _coerce_pin_descriptor(raw: Any) -> dict[str, Any] | None:
    if not isinstance(raw, dict) or "pin" not in raw:
        return None
    pin = _coerce_int(raw["pin"], -1, -1, 100)
    if pin < 0:
        return None
    return {"pin": pin, "label": str(raw.get("label") or f"D{pin}")}


def _coerce_pin_map(raw: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(raw, dict):
        return {}
    pins: dict[str, dict[str, Any]] = {}
    for key, value in raw.items():
        descriptor = _coerce_pin_descriptor(value)
        if descriptor is not None:
            pins[str(key)] = descriptor
    return pins


def _coerce_capability_modes(raw: Any, allowed: set[str]) -> list[str]:
    if not isinstance(raw, list):
        return []
    modes: list[str] = []
    for item in raw:
        value = str(item)
        if value in allowed and value not in modes:
            modes.append(value)
    return modes


def _coerce_capabilities(raw: dict[str, Any], received_at: float) -> dict[str, Any]:
    capabilities: dict[str, Any] = {
        "source": "capabilities",
        "last_received_at": received_at,
    }
    if "sketch" in raw:
        capabilities["sketch"] = str(raw["sketch"])
    if "firmware_version" in raw:
        capabilities["firmware_version"] = str(raw["firmware_version"])
    if isinstance(raw.get("switch_input_pin_options"), list):
        capabilities["switch_input_pin_options"] = _coerce_pin_options(raw["switch_input_pin_options"])
    if isinstance(raw.get("supported_switch_modes"), list):
        capabilities["supported_switch_modes"] = _coerce_capability_modes(raw["supported_switch_modes"], SWITCH_MODES)
    if isinstance(raw.get("supported_pull_modes"), list):
        capabilities["supported_pull_modes"] = _coerce_capability_modes(raw["supported_pull_modes"], SWITCH_PULL_MODES)
    if isinstance(raw.get("supported_switch_test_modes"), list):
        capabilities["supported_switch_test_modes"] = _coerce_capability_modes(raw["supported_switch_test_modes"], SWITCH_TEST_MODES)
    if isinstance(raw.get("output_pins"), dict):
        capabilities["output_pins"] = _coerce_pin_map(raw["output_pins"])
    if isinstance(raw.get("input_pins"), dict):
        capabilities["input_pins"] = _coerce_pin_map(raw["input_pins"])
    return capabilities


def _parse_payload(payload: bytes) -> Any:
    text = payload.decode("utf-8", errors="replace").strip()
    if not text:
        return ""
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def _decode_json_value_after_key(text: str, key: str) -> Any:
    marker = f'"{key}"'
    marker_index = text.find(marker)
    if marker_index < 0:
        raise ValueError(f"Missing {key}")
    colon_index = text.find(":", marker_index + len(marker))
    if colon_index < 0:
        raise ValueError(f"Missing colon after {key}")
    value_text = text[colon_index + 1 :].lstrip()
    value, _end = json.JSONDecoder().raw_decode(value_text)
    return value


def _recover_state_payload(text: str) -> dict[str, Any] | None:
    try:
        outputs = _decode_json_value_after_key(text, "outputs")
    except (json.JSONDecodeError, ValueError):
        return None

    recovered: dict[str, Any] = {"outputs": outputs}
    try:
        recovered["uptime_ms"] = _decode_json_value_after_key(text, "uptime_ms")
    except (json.JSONDecodeError, ValueError):
        pass
    try:
        recovered["last_command"] = _decode_json_value_after_key(text, "last_command")
    except (json.JSONDecodeError, ValueError):
        recovered["last_command"] = None
    return recovered


settings = Settings.from_env()
state = DashboardState(settings)
hub = WebSocketHub()
mqtt_client: mqtt.Client | None = None
event_loop: asyncio.AbstractEventLoop | None = None


def _schedule_broadcast(
    snapshot: dict[str, Any],
    message_type: str = "snapshot",
    **extra: Any,
) -> None:
    if event_loop and event_loop.is_running():
        message = {"type": message_type, "data": snapshot}
        message.update(extra)
        asyncio.run_coroutine_threadsafe(hub.broadcast(message), event_loop)


def _mqtt_connected(reason_code: Any) -> bool:
    is_failure = getattr(reason_code, "is_failure", None)
    if is_failure is not None:
        return not bool(is_failure)
    try:
        return int(reason_code) == 0
    except (TypeError, ValueError):
        return str(reason_code).lower() in {"0", "success", "normal disconnection"}


def _make_mqtt_client() -> mqtt.Client:
    client_id = f"lv-test-plate-web-{socket.gethostname()}-{uuid.uuid4().hex[:8]}"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    if settings.mqtt_username:
        client.username_pw_set(settings.mqtt_username, settings.mqtt_password or None)

    topic_lookup = {topic: key for key, topic in settings.topics.items()}

    def on_connect(client: mqtt.Client, _userdata: Any, _flags: mqtt.ConnectFlags, reason_code: mqtt.ReasonCode, _properties: mqtt.Properties | None) -> None:
        connected = _mqtt_connected(reason_code)
        logger.info("MQTT connect result: %s", reason_code)
        snapshot = state.update_app(mqtt_connected=connected, last_error=None if connected else str(reason_code))
        if connected:
            for topic_key in ("state", "capabilities", "telemetry", "status"):
                topic = settings.topics[topic_key]
                client.subscribe(topic)
                logger.info("Subscribed to MQTT topic %s", topic)
        _schedule_broadcast(snapshot)

    def on_disconnect(client: mqtt.Client, _userdata: Any, _disconnect_flags: mqtt.DisconnectFlags, reason_code: mqtt.ReasonCode, _properties: mqtt.Properties | None) -> None:
        logger.warning("MQTT disconnected: %s", reason_code)
        snapshot = state.update_app(mqtt_connected=False, last_error=str(reason_code))
        _schedule_broadcast(snapshot)

    def on_message(_client: mqtt.Client, _userdata: Any, message: mqtt.MQTTMessage) -> None:
        topic_key = topic_lookup.get(message.topic)
        if not topic_key:
            return
        payload = _parse_payload(message.payload)
        if topic_key == "state" and isinstance(payload, str):
            recovered_payload = _recover_state_payload(payload)
            if recovered_payload is not None:
                logger.debug("Recovered outputs from malformed MQTT state payload")
                payload = recovered_payload
        logger.info("MQTT %s payload received", topic_key)
        snapshot = state.apply_mqtt_message(topic_key, payload)
        _schedule_broadcast(
            snapshot,
            message_type="mqtt_message",
            topic_key=topic_key,
            payload=payload,
        )

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    return client


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global event_loop, mqtt_client
    event_loop = asyncio.get_running_loop()
    mqtt_client = _make_mqtt_client()
    try:
        mqtt_client.connect_async(settings.mqtt_host, settings.mqtt_port, keepalive=30)
        mqtt_client.loop_start()
        logger.info("MQTT client starting for %s:%s", settings.mqtt_host, settings.mqtt_port)
    except Exception as exc:
        logger.exception("Failed to start MQTT client")
        state.update_app(mqtt_connected=False, last_error=str(exc))
    yield
    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()


app = FastAPI(title="lv-test-plate", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.middleware("http")
async def add_development_cache_headers(request: Request, call_next):
    response = await call_next(request)
    if request.url.path.startswith("/static/"):
        response.headers["Cache-Control"] = "no-store"
    return response


@app.get("/")
async def index() -> HTMLResponse:
    html = (STATIC_DIR / "index.html").read_text(encoding="utf-8")
    html = html.replace("{{APP_JS_VERSION}}", ASSET_VERSION)
    html = html.replace("{{STYLES_VERSION}}", ASSET_VERSION)
    return HTMLResponse(
        html,
        headers={"Cache-Control": "no-store"},
    )


@app.get("/favicon.ico", include_in_schema=False)
async def favicon() -> Response:
    return Response(status_code=204)


@app.get("/api/state")
async def get_state() -> dict[str, Any]:
    return state.snapshot()


@app.post("/api/command")
async def post_command(command: dict[str, Any]) -> dict[str, Any]:
    if mqtt_client is None or not mqtt_client.is_connected():
        raise HTTPException(status_code=503, detail="MQTT is not connected")

    current_snapshot = state.snapshot()
    current_outputs = current_snapshot.get("outputs", {})
    next_outputs = _coerce_outputs(current_outputs)
    output_command = False
    modified_outputs: set[str] = set()
    switch_test_updates: dict[str, Any] | None = None

    toggle_key = command.get("toggle")
    if toggle_key is not None:
        if toggle_key not in {"relay_1", "relay_2", "ssr_1", "pwm_enabled"}:
            raise HTTPException(status_code=400, detail="Unsupported toggle output")
        next_outputs[toggle_key] = not bool(next_outputs.get(toggle_key, False))
        output_command = True
        modified_outputs.add(toggle_key)
    elif isinstance(command.get("set_outputs"), dict):
        normalized_outputs = _coerce_outputs(command["set_outputs"])
        next_outputs.update(normalized_outputs)
        modified_outputs.update(normalized_outputs)
        output_command = True
    elif isinstance(command.get("outputs"), dict):
        normalized_outputs = _coerce_outputs(command["outputs"])
        next_outputs.update(normalized_outputs)
        modified_outputs.update(normalized_outputs)
        output_command = True

    if isinstance(command.get("switch_tests"), dict):
        switch_test_updates = {}
        for channel in SWITCH_CHANNELS:
            config = command["switch_tests"].get(channel)
            if isinstance(config, dict):
                switch_test_updates[channel] = _coerce_switch_config(config)

    read_switch = command.get("read_switch")
    test_switch = command.get("test_switch")
    observe_switch = command.get("observe_switch")
    stop_observation = command.get("stop_observation")
    if read_switch is not None and read_switch not in SWITCH_CHANNELS:
        raise HTTPException(status_code=400, detail="Unsupported switch read channel")
    if test_switch is not None and test_switch not in SWITCH_CHANNELS:
        raise HTTPException(status_code=400, detail="Unsupported switch test channel")
    if observe_switch is not None and observe_switch not in SWITCH_CHANNELS:
        raise HTTPException(status_code=400, detail="Unsupported switch observation channel")
    if stop_observation is not None and stop_observation not in SWITCH_CHANNELS:
        raise HTTPException(status_code=400, detail="Unsupported switch observation channel")

    effective_switch_tests = json.loads(json.dumps(current_snapshot.get("switch_tests", _default_switch_tests())))
    if switch_test_updates:
        _merge_switch_tests(effective_switch_tests, switch_test_updates)

    for relay in ("relay_1", "relay_2"):
        config = effective_switch_tests.get(relay, {})
        if relay in modified_outputs and config.get("enabled") and config.get("test_mode") == "relay_follow":
            raise HTTPException(status_code=409, detail=f"{relay} is controlled by relay-follow mode")

    if not output_command and not switch_test_updates and read_switch is None and test_switch is None and observe_switch is None and stop_observation is None:
        raise HTTPException(status_code=400, detail="Command must include outputs, switch_tests, read_switch, test_switch, or observe_switch")

    payload = {
        "source": "web",
        "ts": time.time(),
    }
    if output_command:
        payload["outputs"] = next_outputs
    if switch_test_updates:
        payload["switch_tests"] = switch_test_updates
    if read_switch is not None:
        payload["read_switch"] = read_switch
    if test_switch is not None:
        payload["test_switch"] = test_switch
    if observe_switch is not None:
        payload["observe_switch"] = observe_switch
    if stop_observation is not None:
        payload["stop_observation"] = stop_observation

    info = mqtt_client.publish(settings.topics["cmd"], json.dumps(payload, separators=(",", ":")), qos=0, retain=False)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        raise HTTPException(status_code=503, detail=f"MQTT publish failed: {mqtt.error_string(info.rc)}")

    snapshot = state.apply_command_snapshot(
        outputs=next_outputs if output_command else None,
        switch_tests=switch_test_updates,
        last_command=payload,
    )
    await hub.broadcast({"type": "command_sent", "data": snapshot, "payload": payload})
    return {"ok": True, "topic": settings.topics["cmd"], "payload": payload, "snapshot": snapshot}


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    await hub.connect(websocket)
    try:
        await websocket.send_json({"type": "snapshot", "data": state.snapshot()})
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        await hub.disconnect(websocket)
