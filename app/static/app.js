const APP_JS_VERSION = "latest-snapshot-2026-05-24-1";

const DEFAULT_OUTPUTS = {
  relay_1: false,
  relay_2: false,
  ssr_1: false,
  pwm_enabled: false,
  pwm_value: 0,
};

let latest = null;

const pwmSync = {
  allowServerSync: true,
  pendingValue: null,
};

const els = {
  appMqtt: document.querySelector("#app-mqtt"),
  boardStatus: document.querySelector("#board-status"),
  message: document.querySelector("#message"),
  pwmValue: document.querySelector("#pwm-value"),
  pwmOutput: document.querySelector("#pwm-output"),
  uptime: document.querySelector("#uptime"),
  wifiRssi: document.querySelector("#wifi-rssi"),
  potValue: document.querySelector("#pot-value"),
  lastCommand: document.querySelector("#last-command"),
  outRelay1: document.querySelector("#out-relay-1"),
  outRelay2: document.querySelector("#out-relay-2"),
  outSsr1: document.querySelector("#out-ssr-1"),
  outPwm: document.querySelector("#out-pwm"),
  wsDebug: document.querySelector("#ws-debug"),
};

function setDebug(text) {
  console.debug(`[lv-test-plate] ${text}`);
  els.wsDebug.textContent = text;
}

function setPill(el, text, status) {
  el.textContent = text;
  el.classList.remove("pill-good", "pill-warn", "pill-danger");
  el.classList.add(status === "good" ? "pill-good" : status === "danger" ? "pill-danger" : "pill-warn");
}

function formatUptime(ms) {
  if (ms === null || ms === undefined) return "-";
  const seconds = Math.floor(Number(ms) / 1000);
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainingSeconds = seconds % 60;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m ${remainingSeconds}s`;
  return `${remainingSeconds}s`;
}

function normalizeOutputs(raw) {
  const normalized = {};
  if (!raw || typeof raw !== "object") return normalized;

  ["relay_1", "relay_2", "ssr_1", "pwm_enabled"].forEach((key) => {
    if (Object.hasOwn(raw, key)) normalized[key] = Boolean(raw[key]);
  });

  if (Object.hasOwn(raw, "pwm_value")) {
    const value = Number.parseInt(raw.pwm_value, 10);
    if (Number.isFinite(value)) normalized.pwm_value = Math.max(0, Math.min(255, value));
  }

  return normalized;
}

function latestOutputs() {
  return { ...DEFAULT_OUTPUTS, ...normalizeOutputs(latest?.outputs) };
}

function formatValue(value) {
  if (value === null || value === undefined || value === "") return "-";
  if (typeof value === "string") return value;
  return JSON.stringify(value);
}

function syncPwmControl(outputs) {
  const pwmValue = Number(outputs.pwm_value || 0);

  if (pwmSync.pendingValue !== null) {
    if (pwmValue === pwmSync.pendingValue) {
      pwmSync.pendingValue = null;
      pwmSync.allowServerSync = true;
    } else {
      els.pwmOutput.value = els.pwmValue.value;
      return;
    }
  }

  if (pwmSync.allowServerSync) {
    els.pwmValue.value = pwmValue;
    els.pwmOutput.value = pwmValue;
  } else {
    els.pwmOutput.value = els.pwmValue.value;
  }
}

function render(snapshot) {
  if (!snapshot) return;
  latest = snapshot;

  const outputs = latestOutputs();
  setPill(
    els.appMqtt,
    `MQTT: ${latest.app?.mqtt_connected ? "connected" : "offline"}`,
    latest.app?.mqtt_connected ? "good" : "danger",
  );
  setPill(
    els.boardStatus,
    `Board: ${latest.board?.online ? "online" : latest.board?.status || "unknown"}`,
    latest.board?.online ? "good" : latest.board?.status === "offline" ? "danger" : "warn",
  );

  document.querySelectorAll(".toggle").forEach((button) => {
    const key = button.dataset.output;
    const isOn = Boolean(outputs[key]);
    button.classList.toggle("is-on", isOn);
    button.querySelector("strong").textContent = isOn ? "ON" : "OFF";
  });

  syncPwmControl(outputs);

  const telemetry = latest.telemetry || {};
  els.uptime.textContent = formatUptime(telemetry.uptime_ms);
  els.wifiRssi.textContent = telemetry.wifi_rssi === null || telemetry.wifi_rssi === undefined ? "-" : `${telemetry.wifi_rssi} dBm`;
  els.potValue.textContent = telemetry.pot_value === null || telemetry.pot_value === undefined ? "-" : telemetry.pot_value;
  els.lastCommand.textContent = formatValue(latest.last_command);

  els.outRelay1.textContent = `relay_1: ${outputs.relay_1 ? "on" : "off"}`;
  els.outRelay2.textContent = `relay_2: ${outputs.relay_2 ? "on" : "off"}`;
  els.outSsr1.textContent = `ssr_1: ${outputs.ssr_1 ? "on" : "off"}`;
  els.outPwm.textContent = `pwm: ${outputs.pwm_enabled ? "on" : "off"} / ${outputs.pwm_value}`;
}

function send(command) {
  console.debug("[lv-test-plate] command payload being sent", command);
  els.message.textContent = "Sending command...";
  return fetch("/api/command", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(command),
  }).then(async (response) => {
    if (!response.ok) {
      const error = await response.text();
      throw new Error(error || `HTTP ${response.status}`);
    }
    return response.json();
  }).then((result) => {
    if (result.snapshot) render(result.snapshot);
    els.message.textContent = "Command published.";
    return result;
  });
}

function sendSetOutputs(changes) {
  console.debug("[lv-test-plate] latest outputs before click", latestOutputs());
  return send({ set_outputs: normalizeOutputs(changes) }).catch((error) => {
    els.message.textContent = `Command failed: ${error.message}`;
  });
}

document.querySelectorAll(".toggle").forEach((button) => {
  button.addEventListener("click", () => {
    const key = button.dataset.output;
    console.debug("[lv-test-plate] latest outputs before click", latestOutputs());
    send({ toggle: key }).catch((error) => {
      els.message.textContent = `Command failed: ${error.message}`;
    });
  });
});

els.pwmValue.addEventListener("input", () => {
  pwmSync.allowServerSync = false;
  els.pwmOutput.value = els.pwmValue.value;
});

els.pwmValue.addEventListener("focus", () => {
  pwmSync.allowServerSync = false;
});

els.pwmValue.addEventListener("change", () => {
  const pwmValue = Math.max(0, Math.min(255, Number(els.pwmValue.value)));
  pwmSync.pendingValue = pwmValue;
  pwmSync.allowServerSync = false;
  sendSetOutputs({ pwm_value: pwmValue });
});

document.querySelector("#all-off").addEventListener("click", () => {
  pwmSync.pendingValue = 0;
  pwmSync.allowServerSync = true;
  sendSetOutputs({
    relay_1: false,
    relay_2: false,
    ssr_1: false,
    pwm_enabled: false,
    pwm_value: 0,
  });
});

fetch("/api/state")
  .then((response) => response.json())
  .then((snapshot) => {
    setDebug(`Initial state loaded (${APP_JS_VERSION})`);
    render(snapshot);
  })
  .catch((error) => {
    els.message.textContent = `Initial state failed: ${error.message}`;
  });

function connectWebSocket() {
  const scheme = window.location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${scheme}://${window.location.host}/ws`);

  ws.addEventListener("open", () => {
    els.message.textContent = "Dashboard live updates connected.";
    setDebug(`WebSocket open (${APP_JS_VERSION})`);
  });

  ws.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    console.debug("[lv-test-plate] incoming WebSocket message", message.type, message);
    setDebug(`WebSocket message: ${message.type}${message.topic_key ? ` (${message.topic_key})` : ""}`);
    if (message.type === "snapshot" || message.type === "mqtt_message" || message.type === "command_sent") {
      render(message.data);
    }
  });

  ws.addEventListener("close", () => {
    els.message.textContent = "Dashboard live updates disconnected. Reconnecting...";
    setDebug("WebSocket closed");
    setTimeout(connectWebSocket, 1500);
  });
}

connectWebSocket();
