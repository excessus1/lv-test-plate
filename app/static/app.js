const outputs = {
  relay_1: false,
  relay_2: false,
  ssr_1: false,
  pwm_enabled: false,
  pwm_value: 0,
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
};

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

function render(snapshot) {
  if (!snapshot) return;

  Object.assign(outputs, snapshot.outputs || {});
  outputs.pwm_value = Math.max(0, Math.min(255, Number(outputs.pwm_value || 0)));

  setPill(
    els.appMqtt,
    `MQTT: ${snapshot.app?.mqtt_connected ? "connected" : "offline"}`,
    snapshot.app?.mqtt_connected ? "good" : "danger",
  );
  setPill(
    els.boardStatus,
    `Board: ${snapshot.board?.online ? "online" : snapshot.board?.status || "unknown"}`,
    snapshot.board?.online ? "good" : "warn",
  );

  document.querySelectorAll(".toggle").forEach((button) => {
    const key = button.dataset.output;
    const isOn = Boolean(outputs[key]);
    button.classList.toggle("is-on", isOn);
    button.querySelector("strong").textContent = isOn ? "ON" : "OFF";
  });

  els.pwmValue.value = outputs.pwm_value;
  els.pwmOutput.value = outputs.pwm_value;

  const telemetry = snapshot.telemetry || {};
  els.uptime.textContent = formatUptime(telemetry.uptime_ms);
  els.wifiRssi.textContent = telemetry.wifi_rssi === null || telemetry.wifi_rssi === undefined ? "-" : `${telemetry.wifi_rssi} dBm`;
  els.potValue.textContent = telemetry.pot_value === null || telemetry.pot_value === undefined ? "-" : telemetry.pot_value;
  els.lastCommand.textContent = snapshot.last_command ? JSON.stringify(snapshot.last_command) : "-";

  els.outRelay1.textContent = `relay_1: ${outputs.relay_1 ? "on" : "off"}`;
  els.outRelay2.textContent = `relay_2: ${outputs.relay_2 ? "on" : "off"}`;
  els.outSsr1.textContent = `ssr_1: ${outputs.ssr_1 ? "on" : "off"}`;
  els.outPwm.textContent = `pwm: ${outputs.pwm_enabled ? "on" : "off"} / ${outputs.pwm_value}`;
}

async function sendCommand(nextOutputs) {
  els.message.textContent = "Sending command...";
  const response = await fetch("/api/command", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ outputs: nextOutputs }),
  });
  if (!response.ok) {
    const error = await response.text();
    throw new Error(error || `HTTP ${response.status}`);
  }
  els.message.textContent = "Command published.";
}

function commandWith(changes) {
  const next = { ...outputs, ...changes };
  sendCommand(next).catch((error) => {
    els.message.textContent = `Command failed: ${error.message}`;
  });
}

document.querySelectorAll(".toggle").forEach((button) => {
  button.addEventListener("click", () => {
    const key = button.dataset.output;
    commandWith({ [key]: !outputs[key] });
  });
});

els.pwmValue.addEventListener("input", () => {
  els.pwmOutput.value = els.pwmValue.value;
});

els.pwmValue.addEventListener("change", () => {
  commandWith({ pwm_value: Number(els.pwmValue.value) });
});

document.querySelector("#all-off").addEventListener("click", () => {
  commandWith({
    relay_1: false,
    relay_2: false,
    ssr_1: false,
    pwm_enabled: false,
    pwm_value: 0,
  });
});

async function loadInitialState() {
  const response = await fetch("/api/state");
  render(await response.json());
}

function connectWebSocket() {
  const scheme = window.location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${scheme}://${window.location.host}/ws`);

  ws.addEventListener("open", () => {
    els.message.textContent = "Dashboard live updates connected.";
  });

  ws.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "snapshot") render(message.data);
  });

  ws.addEventListener("close", () => {
    els.message.textContent = "Dashboard live updates disconnected. Reconnecting...";
    setTimeout(connectWebSocket, 1500);
  });
}

loadInitialState().catch((error) => {
  els.message.textContent = `Initial state failed: ${error.message}`;
});
connectWebSocket();

