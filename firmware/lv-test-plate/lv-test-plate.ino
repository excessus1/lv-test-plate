#include <ArduinoJson.h>
#include <ArduinoMqttClient.h>
#include <WiFiS3.h>

#if __has_include("config.h")
#include "config.h"
#else
#warning "Using config.example.h. Copy it to config.h and fill real WiFi/MQTT settings before uploading."
#include "config.example.h"
#endif

// Pin mapping. Edit these constants to match the test plate wiring.
const int RELAY_1_PIN = 8;      // D8, digital relay output
const int RELAY_2_PIN = 13;      // D13, digital relay output
const int SSR_1_PIN = 3;         // D3, SSR output
const int PWM_PIN = 5;           // D5, PWM-capable output
const int POT_PIN = A0;          // A0, analog potentiometer input

// Output polarity. Relay modules and SSRs often use different active levels.
const int RELAY_ON_LEVEL = LOW;
const int RELAY_OFF_LEVEL = HIGH;
const int SSR_ON_LEVEL = HIGH;
const int SSR_OFF_LEVEL = LOW;

// Switch-test input options are defined here with the active pin map above.
// The web UI reads these from firmware state instead of relying on README pin notes.
const int NO_SWITCH_PIN = -1;
const int SWITCH_INPUT_PIN_OPTIONS[] = {2, 4, 6, 7, 9, 10, 11, 12, A1, A2, A3, A4, A5};
const char* SWITCH_INPUT_PIN_LABELS[] = {"D2", "D4", "D6", "D7", "D9", "D10", "D11", "D12", "A1", "A2", "A3", "A4", "A5"};
const size_t SWITCH_INPUT_PIN_COUNT = sizeof(SWITCH_INPUT_PIN_OPTIONS) / sizeof(SWITCH_INPUT_PIN_OPTIONS[0]);
const size_t SWITCH_CHANNEL_COUNT = 2;
const char* SWITCH_CHANNEL_KEYS[] = {"relay_1", "relay_2"};
const unsigned long DEFAULT_SWITCH_DEBOUNCE_MS = 30;
const unsigned long DEFAULT_SWITCH_SETTLE_MS = 150;

const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const unsigned long STATE_PUBLISH_INTERVAL_MS = 5000;
const unsigned long TELEMETRY_PUBLISH_INTERVAL_MS = 2000;
const size_t STATE_JSON_CAPACITY = 4096;

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

String cmdTopic;
String stateTopic;
String telemetryTopic;
String statusTopic;

struct Outputs {
  bool relay1 = false;
  bool relay2 = false;
  bool ssr1 = false;
  bool pwmEnabled = false;
  int pwmValue = 0;
};

enum SwitchContactMode {
  SWITCH_MODE_NO,
  SWITCH_MODE_NC
};

enum SwitchPullMode {
  SWITCH_PULL_EXTERNAL,
  SWITCH_PULL_UP,
  SWITCH_PULL_DOWN
};

struct SwitchConfig {
  bool enabled = false;
  int pin = NO_SWITCH_PIN;
  SwitchContactMode mode = SWITCH_MODE_NO;
  SwitchPullMode pullMode = SWITCH_PULL_UP;
  unsigned long debounceMs = DEFAULT_SWITCH_DEBOUNCE_MS;
  unsigned long settleMs = DEFAULT_SWITCH_SETTLE_MS;
};

struct SwitchReading {
  bool configured = false;
  int raw = -1;
  bool closed = false;
  unsigned int bounceCount = 0;
  unsigned long sampledAt = 0;
};

struct SwitchTestResult {
  bool available = false;
  bool pass = false;
  bool changed = false;
  SwitchReading before;
  SwitchReading after;
  unsigned long settleMs = DEFAULT_SWITCH_SETTLE_MS;
  String status = "not_run";
};

Outputs outputs;
SwitchConfig switchConfigs[SWITCH_CHANNEL_COUNT];
SwitchReading switchReadings[SWITCH_CHANNEL_COUNT];
SwitchTestResult switchResults[SWITCH_CHANNEL_COUNT];
String lastCommandSummary = "";
unsigned long lastMqttAttempt = 0;
unsigned long lastStatePublish = 0;
unsigned long lastTelemetryPublish = 0;
bool mqttWasConnected = false;

void applyOutputs() {
  digitalWrite(RELAY_1_PIN, outputs.relay1 ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  digitalWrite(RELAY_2_PIN, outputs.relay2 ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  digitalWrite(SSR_1_PIN, outputs.ssr1 ? SSR_ON_LEVEL : SSR_OFF_LEVEL);
  analogWrite(PWM_PIN, outputs.pwmEnabled ? constrain(outputs.pwmValue, 0, 255) : 0);
}

void allOutputsOff() {
  outputs.relay1 = false;
  outputs.relay2 = false;
  outputs.ssr1 = false;
  outputs.pwmEnabled = false;
  outputs.pwmValue = 0;
  applyOutputs();
}

bool isSwitchPinAllowed(int pin) {
  for (size_t i = 0; i < SWITCH_INPUT_PIN_COUNT; i++) {
    if (SWITCH_INPUT_PIN_OPTIONS[i] == pin) {
      return true;
    }
  }
  return false;
}

const char* switchPinLabel(int pin) {
  for (size_t i = 0; i < SWITCH_INPUT_PIN_COUNT; i++) {
    if (SWITCH_INPUT_PIN_OPTIONS[i] == pin) {
      return SWITCH_INPUT_PIN_LABELS[i];
    }
  }
  return "";
}

int switchChannelIndex(const char* channel) {
  if (channel == nullptr) {
    return -1;
  }
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    if (strcmp(channel, SWITCH_CHANNEL_KEYS[i]) == 0) {
      return (int)i;
    }
  }
  return -1;
}

const char* switchModeName(SwitchContactMode mode) {
  return mode == SWITCH_MODE_NC ? "NC" : "NO";
}

SwitchContactMode parseSwitchMode(const char* raw, SwitchContactMode fallback) {
  if (raw == nullptr) {
    return fallback;
  }
  String value = raw;
  value.trim();
  value.toUpperCase();
  if (value == "NC") {
    return SWITCH_MODE_NC;
  }
  if (value == "NO") {
    return SWITCH_MODE_NO;
  }
  return fallback;
}

const char* switchPullModeName(SwitchPullMode mode) {
  if (mode == SWITCH_PULL_UP) {
    return "pullup";
  }
  if (mode == SWITCH_PULL_DOWN) {
    return "pulldown";
  }
  return "external";
}

SwitchPullMode parseSwitchPullMode(const char* raw, SwitchPullMode fallback) {
  if (raw == nullptr) {
    return fallback;
  }
  String value = raw;
  value.trim();
  value.toLowerCase();
  if (value == "pullup" || value == "internal_pullup" || value == "internal_pull-up") {
    return SWITCH_PULL_UP;
  }
  if (value == "pulldown" || value == "internal_pulldown" || value == "internal_pull-down") {
    return SWITCH_PULL_DOWN;
  }
  if (value == "external" || value == "none" || value == "no_pull") {
    return SWITCH_PULL_EXTERNAL;
  }
  return fallback;
}

const char* effectiveSwitchPullModeName(SwitchPullMode mode) {
#if defined(INPUT_PULLDOWN)
  return switchPullModeName(mode);
#else
  return mode == SWITCH_PULL_DOWN ? "external" : switchPullModeName(mode);
#endif
}

bool switchConfigured(size_t index) {
  return index < SWITCH_CHANNEL_COUNT && switchConfigs[index].enabled && isSwitchPinAllowed(switchConfigs[index].pin);
}

void configureSwitchPin(size_t index) {
  if (index >= SWITCH_CHANNEL_COUNT || !switchConfigured(index)) {
    return;
  }

  SwitchConfig& config = switchConfigs[index];
  if (config.pullMode == SWITCH_PULL_UP) {
    pinMode(config.pin, INPUT_PULLUP);
  } else if (config.pullMode == SWITCH_PULL_DOWN) {
#if defined(INPUT_PULLDOWN)
    pinMode(config.pin, INPUT_PULLDOWN);
#else
    pinMode(config.pin, INPUT);
#endif
  } else {
    pinMode(config.pin, INPUT);
  }
}

void configureSwitchPins() {
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    configureSwitchPin(i);
  }
}

bool rawSwitchClosed(const SwitchConfig& config, int raw) {
  if (config.pullMode == SWITCH_PULL_UP) {
    return raw == LOW;
  }
  return raw == HIGH;
}

const char* rawStateName(int raw) {
  if (raw == HIGH) {
    return "HIGH";
  }
  if (raw == LOW) {
    return "LOW";
  }
  return "n/a";
}

SwitchReading readSwitch(size_t index) {
  SwitchReading reading;
  reading.sampledAt = millis();
  if (!switchConfigured(index)) {
    return reading;
  }

  SwitchConfig& config = switchConfigs[index];
  unsigned long debounceMs = config.debounceMs > 1000 ? 1000 : config.debounceMs;
  int stableRaw = digitalRead(config.pin);
  unsigned long stableSince = millis();
  unsigned long startedAt = stableSince;

  while (debounceMs > 0 && millis() - stableSince < debounceMs && millis() - startedAt < debounceMs + 250) {
    int raw = digitalRead(config.pin);
    if (raw != stableRaw) {
      stableRaw = raw;
      stableSince = millis();
      reading.bounceCount++;
    }
    delay(1);
  }

  reading.configured = true;
  reading.raw = stableRaw;
  reading.closed = rawSwitchClosed(config, stableRaw);
  reading.sampledAt = millis();
  switchReadings[index] = reading;
  return reading;
}

void setSwitchRelayOutput(size_t index, bool on) {
  if (index == 0) {
    outputs.relay1 = on;
  } else if (index == 1) {
    outputs.relay2 = on;
  }
  applyOutputs();
}

void runSwitchTest(size_t index) {
  if (index >= SWITCH_CHANNEL_COUNT) {
    return;
  }

  SwitchTestResult result;
  result.available = true;
  result.settleMs = switchConfigs[index].settleMs;

  if (!switchConfigured(index)) {
    result.status = "not_configured";
    switchResults[index] = result;
    return;
  }

  setSwitchRelayOutput(index, false);
  result.before = readSwitch(index);
  setSwitchRelayOutput(index, true);
  delay(switchConfigs[index].settleMs > 5000 ? 5000 : switchConfigs[index].settleMs);
  result.after = readSwitch(index);
  result.changed = result.before.configured && result.after.configured && result.before.closed != result.after.closed;

  bool expectBeforeClosed = switchConfigs[index].mode == SWITCH_MODE_NC;
  bool expectAfterClosed = switchConfigs[index].mode == SWITCH_MODE_NO;
  result.pass = result.before.configured && result.after.configured && result.before.closed == expectBeforeClosed && result.after.closed == expectAfterClosed && result.changed;
  result.status = result.pass ? "pass" : "fail";

  setSwitchRelayOutput(index, false);
  switchResults[index] = result;
}

void publishText(const String& topic, const char* text, bool retained) {
  mqttClient.beginMessage(topic.c_str(), retained);
  mqttClient.print(text);
  mqttClient.endMessage();
}

void writeSwitchReading(JsonObject target, const SwitchReading& reading) {
  target["configured"] = reading.configured;
  target["raw"] = reading.raw;
  target["raw_state"] = rawStateName(reading.raw);
  target["closed"] = reading.closed;
  target["state"] = reading.configured ? (reading.closed ? "closed" : "open") : "unconfigured";
  target["bounce_count"] = reading.bounceCount;
  target["sampled_at_ms"] = reading.sampledAt;
}

void writeSwitchTestResult(JsonObject target, const SwitchTestResult& result) {
  target["available"] = result.available;
  target["status"] = result.status;
  target["pass"] = result.pass;
  target["changed"] = result.changed;
  target["settle_ms"] = result.settleMs;
  JsonObject before = target.createNestedObject("before");
  writeSwitchReading(before, result.before);
  JsonObject after = target.createNestedObject("after");
  writeSwitchReading(after, result.after);
}

void writeSwitchChannel(JsonObject target, size_t index) {
  SwitchConfig& config = switchConfigs[index];
  target["enabled"] = config.enabled;
  target["pin"] = config.pin;
  target["pin_label"] = switchPinLabel(config.pin);
  target["mode"] = switchModeName(config.mode);
  target["pull_mode"] = switchPullModeName(config.pullMode);
  target["effective_pull_mode"] = effectiveSwitchPullModeName(config.pullMode);
  target["debounce_ms"] = config.debounceMs;
  target["settle_ms"] = config.settleMs;
  target["configured"] = switchConfigured(index);

  JsonObject current = target.createNestedObject("current");
  writeSwitchReading(current, switchConfigured(index) ? readSwitch(index) : switchReadings[index]);

  JsonObject lastTest = target.createNestedObject("last_test");
  writeSwitchTestResult(lastTest, switchResults[index]);
}

void publishState(bool retained) {
  StaticJsonDocument<STATE_JSON_CAPACITY> doc;
  JsonObject out = doc.createNestedObject("outputs");
  out["relay_1"] = outputs.relay1;
  out["relay_2"] = outputs.relay2;
  out["ssr_1"] = outputs.ssr1;
  out["pwm_enabled"] = outputs.pwmEnabled;
  out["pwm_value"] = outputs.pwmValue;

  JsonObject switchTests = doc.createNestedObject("switch_tests");
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    JsonObject channel = switchTests.createNestedObject(SWITCH_CHANNEL_KEYS[i]);
    writeSwitchChannel(channel, i);
  }

  JsonArray pinOptions = doc.createNestedArray("switch_input_pin_options");
  for (size_t i = 0; i < SWITCH_INPUT_PIN_COUNT; i++) {
    JsonObject option = pinOptions.createNestedObject();
    option["pin"] = SWITCH_INPUT_PIN_OPTIONS[i];
    option["label"] = SWITCH_INPUT_PIN_LABELS[i];
  }

  doc["uptime_ms"] = millis();
  doc["last_command"] = lastCommandSummary;

  String payload;
  const size_t measuredJsonSize = measureJson(doc);
  const size_t serializedJsonSize = serializeJson(doc, payload);
  const bool jsonOverflowed = doc.overflowed();

  Serial.print("publishState json_capacity=");
  Serial.print(STATE_JSON_CAPACITY);
  Serial.print(" measured=");
  Serial.print(measuredJsonSize);
  Serial.print(" serialized=");
  Serial.print(serializedJsonSize);
  Serial.print(" payload_len=");
  Serial.print(payload.length());
  Serial.print(" overflowed=");
  Serial.println(jsonOverflowed ? "true" : "false");
  if (jsonOverflowed) {
    Serial.println("publishState ERROR: ArduinoJson overflowed; state payload is incomplete");
  }

  const int beginOk = mqttClient.beginMessage(stateTopic.c_str(), (unsigned long)payload.length(), retained);
  size_t mqttBytesWritten = 0;
  int endOk = 0;
  if (beginOk) {
    mqttBytesWritten = mqttClient.print(payload);
    endOk = mqttClient.endMessage();
  }

  Serial.print("publishState mqtt_begin=");
  Serial.print(beginOk);
  Serial.print(" mqtt_written=");
  Serial.print(mqttBytesWritten);
  Serial.print(" mqtt_end=");
  Serial.println(endOk);
  if (!beginOk || !endOk || mqttBytesWritten != payload.length()) {
    Serial.println("publishState ERROR: MQTT state publish did not write the full payload");
  }
}

void publishTelemetry() {
  StaticJsonDocument<256> doc;
  doc["uptime_ms"] = millis();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["pot_value"] = analogRead(POT_PIN);
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["mqtt_connected"] = mqttClient.connected();

  String payload;
  serializeJson(doc, payload);
  mqttClient.beginMessage(telemetryTopic.c_str(), false);
  mqttClient.print(payload);
  mqttClient.endMessage();
}

void updateLastCommandSummary(const JsonObject& out) {
  String summary = "";
  if (out.containsKey("relay_1")) {
    summary += "relay_1:";
    summary += outputs.relay1 ? "on" : "off";
  }
  if (out.containsKey("relay_2")) {
    if (summary.length() > 0) summary += " ";
    summary += "relay_2:";
    summary += outputs.relay2 ? "on" : "off";
  }
  if (out.containsKey("ssr_1")) {
    if (summary.length() > 0) summary += " ";
    summary += "ssr_1:";
    summary += outputs.ssr1 ? "on" : "off";
  }
  if (out.containsKey("pwm_enabled")) {
    if (summary.length() > 0) summary += " ";
    summary += "pwm_enabled:";
    summary += outputs.pwmEnabled ? "on" : "off";
  }
  if (out.containsKey("pwm_value")) {
    if (summary.length() > 0) summary += " ";
    summary += "pwm:";
    summary += outputs.pwmValue;
  }
  lastCommandSummary = summary.length() > 0 ? summary : "outputs";
}

bool updateSwitchConfig(size_t index, JsonObject configDoc) {
  if (index >= SWITCH_CHANNEL_COUNT || configDoc.isNull()) {
    return false;
  }

  SwitchConfig& config = switchConfigs[index];
  bool changed = false;

  if (configDoc.containsKey("enabled")) {
    config.enabled = configDoc["enabled"].as<bool>();
    changed = true;
  }
  if (configDoc.containsKey("pin")) {
    if (configDoc["pin"].isNull()) {
      config.pin = NO_SWITCH_PIN;
    } else {
      int requestedPin = configDoc["pin"].as<int>();
      config.pin = isSwitchPinAllowed(requestedPin) ? requestedPin : NO_SWITCH_PIN;
    }
    changed = true;
  }
  if (configDoc.containsKey("mode")) {
    config.mode = parseSwitchMode(configDoc["mode"].as<const char*>(), config.mode);
    changed = true;
  }
  if (configDoc.containsKey("pull_mode")) {
    config.pullMode = parseSwitchPullMode(configDoc["pull_mode"].as<const char*>(), config.pullMode);
    changed = true;
  }
  if (configDoc.containsKey("debounce_ms")) {
    config.debounceMs = constrain(configDoc["debounce_ms"].as<int>(), 0, 1000);
    changed = true;
  }
  if (configDoc.containsKey("settle_ms")) {
    config.settleMs = constrain(configDoc["settle_ms"].as<int>(), 0, 5000);
    changed = true;
  }

  if (changed) {
    configureSwitchPin(index);
  }
  return changed;
}

bool updateSwitchConfigs(JsonObject switchTests) {
  if (switchTests.isNull()) {
    return false;
  }

  bool changed = false;
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    JsonObject configDoc = switchTests[SWITCH_CHANNEL_KEYS[i]];
    if (!configDoc.isNull()) {
      changed = updateSwitchConfig(i, configDoc) || changed;
    }
  }
  return changed;
}

void handleCommand(int messageSize) {
  String payload;
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  StaticJsonDocument<1536> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    lastCommandSummary = "invalid_json";
    publishState(true);
    return;
  }

  bool handled = false;
  JsonObject switchTests = doc["switch_tests"];
  if (updateSwitchConfigs(switchTests)) {
    lastCommandSummary = "switch_config";
    handled = true;
  }

  JsonObject out = doc["outputs"];
  if (!out.isNull()) {
    if (out.containsKey("relay_1")) outputs.relay1 = out["relay_1"].as<bool>();
    if (out.containsKey("relay_2")) outputs.relay2 = out["relay_2"].as<bool>();
    if (out.containsKey("ssr_1")) outputs.ssr1 = out["ssr_1"].as<bool>();
    if (out.containsKey("pwm_enabled")) outputs.pwmEnabled = out["pwm_enabled"].as<bool>();
    if (out.containsKey("pwm_value")) outputs.pwmValue = constrain(out["pwm_value"].as<int>(), 0, 255);
    updateLastCommandSummary(out);
    handled = true;
  }

  if (doc.containsKey("read_switch")) {
    const char* channel = doc["read_switch"].as<const char*>();
    int index = switchChannelIndex(channel);
    if (index >= 0) {
      readSwitch((size_t)index);
      lastCommandSummary = "switch_read:";
      lastCommandSummary += SWITCH_CHANNEL_KEYS[index];
    } else {
      lastCommandSummary = "switch_read:invalid_channel";
    }
    handled = true;
  }

  if (doc.containsKey("test_switch")) {
    const char* channel = doc["test_switch"].as<const char*>();
    int index = switchChannelIndex(channel);
    if (index >= 0) {
      runSwitchTest((size_t)index);
      lastCommandSummary = "switch_test:";
      lastCommandSummary += SWITCH_CHANNEL_KEYS[index];
    } else {
      lastCommandSummary = "switch_test:invalid_channel";
    }
    handled = true;
  }

  if (!handled) {
    lastCommandSummary = "no_command";
  }

  applyOutputs();
  publishState(true);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.print("Connecting to WiFi");
  while (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED) {
    Serial.print(".");
    delay(3000);
  }
  Serial.println(" connected");
}

void connectMqtt() {
  if (mqttClient.connected()) {
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }
  lastMqttAttempt = now;

  Serial.print("Connecting to MQTT ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  if (mqttClient.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.println("MQTT connected");
    mqttWasConnected = true;
    publishText(statusTopic, "online", true);
    mqttClient.subscribe(cmdTopic.c_str());
    publishState(true);
  } else {
    Serial.print("MQTT failed, error ");
    Serial.println(mqttClient.connectError());
  }
}

void setupTopics() {
  String base = MQTT_BASE_TOPIC;
  base.trim();
  if (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  cmdTopic = base + "/cmd";
  stateTopic = base + "/state";
  telemetryTopic = base + "/telemetry";
  statusTopic = base + "/status";
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_1_PIN, OUTPUT);
  pinMode(RELAY_2_PIN, OUTPUT);
  pinMode(SSR_1_PIN, OUTPUT);
  pinMode(PWM_PIN, OUTPUT);
  allOutputsOff();
  configureSwitchPins();

  setupTopics();
  connectWifi();

  mqttClient.setId(MQTT_CLIENT_ID);
  if (strlen(MQTT_USERNAME) > 0) {
    mqttClient.setUsernamePassword(MQTT_USERNAME, MQTT_PASSWORD);
  }
  mqttClient.setKeepAliveInterval(30000);
  mqttClient.beginWill(statusTopic.c_str(), 7, true, 1);
  mqttClient.print("offline");
  mqttClient.endWill();
  mqttClient.onMessage(handleCommand);

  connectMqtt();
}

void loop() {
  connectWifi();

  if (!mqttClient.connected()) {
    if (mqttWasConnected) {
      Serial.println("MQTT disconnected; forcing outputs off");
      allOutputsOff();
      mqttWasConnected = false;
    }
    connectMqtt();
  }

  mqttClient.poll();

  unsigned long now = millis();
  if (mqttClient.connected() && now - lastTelemetryPublish >= TELEMETRY_PUBLISH_INTERVAL_MS) {
    lastTelemetryPublish = now;
    publishTelemetry();
  }
  if (mqttClient.connected() && now - lastStatePublish >= STATE_PUBLISH_INTERVAL_MS) {
    lastStatePublish = now;
    publishState(true);
  }
}
