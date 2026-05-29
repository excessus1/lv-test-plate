const APP_JS_VERSION = "switch-modes-2026-05-29-1";

const DEFAULT_OUTPUTS = {
  relay_1: false,
  relay_2: false,
  ssr_1: false,
  pwm_enabled: false,
  pwm_value: 0,
};

const SWITCH_CHANNELS = ["relay_1", "relay_2"];
const DEFAULT_SWITCH_TEST = {
  enabled: false,
  pin: null,
  pin_label: "",
  mode: "NO",
  pull_mode: "pullup",
  effective_pull_mode: "pullup",
  test_mode: "passive_read",
  debounce_ms: 30,
  settle_ms: 150,
  observation_duration_s: 30,
  configured: false,
  relay_follow_enabled: false,
  commanded_by: "manual",
  status: "not_configured",
  current: {
    configured: false,
    raw: -1,
    raw_state: "n/a",
    closed: false,
    state: "unconfigured",
    bounce_count: 0,
  },
  observation: {
    active: false,
    available: false,
    status: "not_started",
    duration_s: 30,
    remaining_ms: 0,
    transition_count: 0,
    open_to_closed_count: 0,
    closed_to_open_count: 0,
  },
  last_test: {
    available: false,
    status: "not_run",
    pass: false,
    changed: false,
    before: { state: "unconfigured", raw_state: "n/a" },
    after: { state: "unconfigured", raw_state: "n/a" },
  },
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
  switchPanels: Array.from(document.querySelectorAll("[data-switch-channel]")),
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

function latestSwitchTests() {
  const switchTests = {};
  SWITCH_CHANNELS.forEach((channel) => {
    switchTests[channel] = { ...DEFAULT_SWITCH_TEST, ...(latest?.switch_tests?.[channel] || {}) };
  });
  return switchTests;
}

function pinOptions() {
  if (Array.isArray(latest?.capabilities?.switch_input_pin_options) && latest.capabilities.switch_input_pin_options.length > 0) {
    return latest.capabilities.switch_input_pin_options;
  }
  return Array.isArray(latest?.switch_input_pin_options) ? latest.switch_input_pin_options : [];
}

function formatValue(value) {
  if (value === null || value === undefined || value === "") return "-";
  if (typeof value === "string") return value;
  return JSON.stringify(value);
}

function formatSwitchState(reading) {
  if (!reading?.configured) return "unconfigured";
  return reading.state || (reading.closed ? "closed" : "open");
}

function formatSwitchResult(result) {
  if (!result?.available) return "not run";
  const before = formatSwitchState(result.before);
  const after = formatSwitchState(result.after);
  const changed = result.changed ? "changed" : "no change";
  return `${result.status || "unknown"} (${before} -> ${after}, ${changed})`;
}

function formatObservation(observation) {
  if (!observation?.available && !observation?.active) return "not started";
  const transitions = Number(observation.transition_count || 0);
  const remainingMs = Number(observation.remaining_ms || 0);
  const remaining = observation.active ? `, ${Math.ceil(remainingMs / 1000)}s left` : "";
  return `${observation.status || "unknown"} (${transitions} transitions${remaining})`;
}

function formatModeResult(config) {
  const mode = config.test_mode || "passive_read";
  if (mode === "timed_observation") {
    return `Observation: ${formatObservation(config.observation)}`;
  }
  if (mode === "relay_follow") {
    return `Relay follow: ${config.configured ? config.status || "following" : "not configured"}`;
  }
  if (mode === "output_feedback") {
    return `Feedback: ${formatSwitchResult(config.last_test)}`;
  }
  return `Read: ${config.status || "passive read"}`;
}

function outputKeyToSwitchChannel(key) {
  return key === "relay_1" || key === "relay_2" ? key : null;
}

function relayFollowOverridesOutput(key) {
  const channel = outputKeyToSwitchChannel(key);
  if (!channel) return false;
  const config = latestSwitchTests()[channel];
  return Boolean(config.enabled && config.test_mode === "relay_follow");
}

function syncPinSelect(select, config) {
  const options = pinOptions();
  const configuredPin = config.pin === null || config.pin === undefined || config.pin < 0 ? "" : String(config.pin);
  const hasBoardPinMap = options.length > 0;
  select.textContent = "";

  const empty = document.createElement("option");
  empty.value = "";
  empty.textContent = hasBoardPinMap ? "No switch input" : "no board pin map received";
  select.append(empty);

  options.forEach((option) => {
    const item = document.createElement("option");
    item.value = String(option.pin);
    item.textContent = option.label || `D${option.pin}`;
    select.append(item);
  });

  if (configuredPin && !options.some((option) => String(option.pin) === configuredPin)) {
    const current = document.createElement("option");
    current.value = configuredPin;
    current.textContent = config.pin_label || `Pin ${configuredPin}`;
    select.append(current);
  }

  select.value = configuredPin;
  select.disabled = !hasBoardPinMap && !configuredPin;
  select.title = hasBoardPinMap ? "" : "no board pin map received";
}

function syncSwitchPanel(panel, config) {
  panel.querySelector('[data-switch-field="enabled"]').checked = Boolean(config.enabled);
  syncPinSelect(panel.querySelector('[data-switch-field="pin"]'), config);
  panel.querySelector('[data-switch-field="test_mode"]').value = config.test_mode || "passive_read";
  panel.querySelector('[data-switch-field="mode"]').value = config.mode || "NO";
  panel.querySelector('[data-switch-field="pull_mode"]').value = config.pull_mode || "pullup";
  panel.querySelector('[data-switch-field="debounce_ms"]').value = config.debounce_ms ?? 30;
  panel.querySelector('[data-switch-field="settle_ms"]').value = config.settle_ms ?? 150;
  panel.querySelector('[data-switch-field="observation_duration_s"]').value = config.observation_duration_s ?? 30;

  panel.querySelectorAll("[data-mode-section]").forEach((el) => {
    const sectionMode = el.dataset.modeSection;
    el.classList.toggle("is-hidden", sectionMode !== (config.test_mode || "passive_read"));
  });

  const current = config.current || {};
  const optionCount = pinOptions().length;
  const pinMapReadout = panel.querySelector('[data-switch-readout="pin-map"]');
  panel.querySelector('[data-switch-readout="current"]').textContent = `State: ${formatSwitchState(current)}`;
  panel.querySelector('[data-switch-readout="raw"]').textContent = `Raw: ${current.raw_state || "n/a"} (${current.raw ?? "-"})`;
  panel.querySelector('[data-switch-readout="result"]').textContent = formatModeResult(config);
  pinMapReadout.textContent = optionCount > 0 ? `Pin map: ${optionCount} board options` : "Pin map: no board pin map received";
  pinMapReadout.classList.toggle("is-diagnostic", optionCount === 0);
  panel.classList.toggle("is-pass", Boolean(config.last_test?.available && config.last_test?.pass));
  panel.classList.toggle("is-fail", Boolean(config.last_test?.available && config.last_test?.status === "fail"));
}

function collectSwitchConfig(panel) {
  const pinValue = panel.querySelector('[data-switch-field="pin"]').value;
  return {
    enabled: panel.querySelector('[data-switch-field="enabled"]').checked,
    pin: pinValue === "" ? null : Number.parseInt(pinValue, 10),
    test_mode: panel.querySelector('[data-switch-field="test_mode"]').value,
    mode: panel.querySelector('[data-switch-field="mode"]').value,
    pull_mode: panel.querySelector('[data-switch-field="pull_mode"]').value,
    debounce_ms: Math.max(0, Math.min(1000, Number.parseInt(panel.querySelector('[data-switch-field="debounce_ms"]').value, 10) || 0)),
    settle_ms: Math.max(0, Math.min(5000, Number.parseInt(panel.querySelector('[data-switch-field="settle_ms"]').value, 10) || 0)),
    observation_duration_s: Math.max(1, Math.min(3600, Number.parseInt(panel.querySelector('[data-switch-field="observation_duration_s"]').value, 10) || 30)),
  };
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
    const overridden = relayFollowOverridesOutput(key);
    button.classList.toggle("is-on", isOn);
    button.classList.toggle("is-overridden", overridden);
    button.disabled = overridden;
    button.title = overridden ? "Relay follows switch state while follow mode is active" : "";
    button.querySelector("strong").textContent = overridden ? "FOLLOW" : isOn ? "ON" : "OFF";
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

  const switchTests = latestSwitchTests();
  els.switchPanels.forEach((panel) => {
    syncSwitchPanel(panel, switchTests[panel.dataset.switchChannel]);
  });
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

function sendSwitchConfig(panel) {
  const channel = panel.dataset.switchChannel;
  return send({ switch_tests: { [channel]: collectSwitchConfig(panel) } }).catch((error) => {
    els.message.textContent = `Switch config failed: ${error.message}`;
  });
}

document.querySelectorAll(".toggle").forEach((button) => {
  button.addEventListener("click", () => {
    const key = button.dataset.output;
    if (relayFollowOverridesOutput(key)) {
      els.message.textContent = `${key} is controlled by relay-follow mode.`;
      return;
    }
    console.debug("[lv-test-plate] latest outputs before click", latestOutputs());
    send({ toggle: key }).catch((error) => {
      els.message.textContent = `Command failed: ${error.message}`;
    });
  });
});

els.switchPanels.forEach((panel) => {
  panel.querySelectorAll("[data-switch-field]").forEach((field) => {
    field.addEventListener("change", () => {
      sendSwitchConfig(panel);
    });
  });

  panel.querySelector('[data-switch-action="read"]').addEventListener("click", () => {
    send({ read_switch: panel.dataset.switchChannel }).catch((error) => {
      els.message.textContent = `Switch read failed: ${error.message}`;
    });
  });

  panel.querySelector('[data-switch-action="test"]').addEventListener("click", () => {
    send({ test_switch: panel.dataset.switchChannel }).catch((error) => {
      els.message.textContent = `Switch test failed: ${error.message}`;
    });
  });

  panel.querySelector('[data-switch-action="observe"]').addEventListener("click", () => {
    send({ observe_switch: panel.dataset.switchChannel }).catch((error) => {
      els.message.textContent = `Switch observation failed: ${error.message}`;
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
  const switchTests = latestSwitchTests();
  const followUpdates = {};
  SWITCH_CHANNELS.forEach((channel) => {
    if (switchTests[channel]?.enabled && switchTests[channel]?.test_mode === "relay_follow") {
      followUpdates[channel] = { enabled: false };
    }
  });
  const command = {
    set_outputs: {
      relay_1: false,
      relay_2: false,
      ssr_1: false,
      pwm_enabled: false,
      pwm_value: 0,
    },
  };
  if (Object.keys(followUpdates).length > 0) {
    command.switch_tests = followUpdates;
  }
  send(command).catch((error) => {
    els.message.textContent = `Command failed: ${error.message}`;
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
