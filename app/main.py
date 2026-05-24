from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from threading import Lock
from typing import Any

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
logger = logging.getLogger("lv-test-plate")

APP_DIR = Path(__file__).resolve().parent
STATIC_DIR = APP_DIR / "static"


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
                "telemetry": None,
                "status": None,
            },
        }

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return json.loads(json.dumps(self._state))

    def update_app(self, **values: Any) -> dict[str, Any]:
        with self._lock:
            self._state["app"].update(values)
            return json.loads(json.dumps(self._state))

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
                if "last_command" in payload:
                    self._state["last_command"] = payload["last_command"]
                elif "last_command_received" in payload:
                    self._state["last_command"] = payload["last_command_received"]
                self._state["telemetry"]["uptime_ms"] = payload.get("uptime_ms", self._state["telemetry"]["uptime_ms"])
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
            return json.loads(json.dumps(self._state))


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


def _parse_payload(payload: bytes) -> Any:
    text = payload.decode("utf-8", errors="replace").strip()
    if not text:
        return ""
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


settings = Settings.from_env()
state = DashboardState(settings)
hub = WebSocketHub()
mqtt_client: mqtt.Client | None = None
event_loop: asyncio.AbstractEventLoop | None = None


def _schedule_broadcast(snapshot: dict[str, Any]) -> None:
    if event_loop and event_loop.is_running():
        asyncio.run_coroutine_threadsafe(hub.broadcast({"type": "snapshot", "data": snapshot}), event_loop)


def _make_mqtt_client() -> mqtt.Client:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"lv-test-plate-web-{os.getpid()}")
    if settings.mqtt_username:
        client.username_pw_set(settings.mqtt_username, settings.mqtt_password or None)

    topic_lookup = {topic: key for key, topic in settings.topics.items()}

    def on_connect(client: mqtt.Client, _userdata: Any, _flags: mqtt.ConnectFlags, reason_code: mqtt.ReasonCode, _properties: mqtt.Properties | None) -> None:
        connected = reason_code == 0
        logger.info("MQTT connect result: %s", reason_code)
        snapshot = state.update_app(mqtt_connected=connected, last_error=None if connected else str(reason_code))
        if connected:
            for topic_key in ("state", "telemetry", "status"):
                client.subscribe(settings.topics[topic_key])
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
        snapshot = state.apply_mqtt_message(topic_key, payload)
        _schedule_broadcast(snapshot)

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


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


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

    outputs = command.get("outputs")
    if not isinstance(outputs, dict):
        raise HTTPException(status_code=400, detail="Command must include an outputs object")

    payload = {
        "outputs": _coerce_outputs(outputs),
        "source": "web",
        "ts": time.time(),
    }
    info = mqtt_client.publish(settings.topics["cmd"], json.dumps(payload, separators=(",", ":")), qos=0, retain=False)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        raise HTTPException(status_code=503, detail=f"MQTT publish failed: {mqtt.error_string(info.rc)}")

    snapshot = state.snapshot()
    await hub.broadcast({"type": "command_sent", "data": payload})
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
