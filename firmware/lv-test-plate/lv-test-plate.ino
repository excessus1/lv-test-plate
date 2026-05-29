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
const char* RELAY_1_PIN_LABEL = "D8";
const char* RELAY_2_PIN_LABEL = "D13";
const char* SSR_1_PIN_LABEL = "D3";
const char* PWM_PIN_LABEL = "D5";
const char* POT_PIN_LABEL = "A0";

// Output polarity. Relay modules and SSRs often use different active levels.
const int RELAY_ON_LEVEL = LOW;
const int RELAY_OFF_LEVEL = HIGH;
const int SSR_ON_LEVEL = HIGH;
const int SSR_OFF_LEVEL = LOW;

// Switch-test input options are defined here with the active pin map above.
// The web UI reads these from firmware capabilities instead of relying on README pin notes.
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
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
const unsigned long MQTT_POLL_DIAGNOSTIC_INTERVAL_MS = 5000;
const char* SKETCH_ID = "lv-test-plate";
const char* FIRMWARE_VERSION = "2026-05-29-stability-1";
const size_t STATE_JSON_CAPACITY = 3072;
const size_t CAPABILITIES_JSON_CAPACITY = 1536;

#ifndef LVTP_ENABLE_CAPABILITIES_PUBLISH
#define LVTP_ENABLE_CAPABILITIES_PUBLISH 0
#endif

#ifndef LVTP_ENABLE_FULL_SWITCH_TEST_STATE
#define LVTP_ENABLE_FULL_SWITCH_TEST_STATE 0
#endif

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

String cmdTopic;
String stateTopic;
String capabilitiesTopic;
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
  const char* status = "not_run";
};

Outputs outputs;
SwitchConfig switchConfigs[SWITCH_CHANNEL_COUNT];
SwitchReading switchReadings[SWITCH_CHANNEL_COUNT];
SwitchTestResult switchResults[SWITCH_CHANNEL_COUNT];
char lastCommandSummaryBuffer[96] = "";
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastStatePublish = 0;
unsigned long lastTelemetryPublish = 0;
unsigned long lastPollDiagnostic = 0;
bool mqttWasConnected = false;
bool wifiWasConnected = false;
bool stateDirty = true;

void setLastCommandSummary(const char* value) {
  snprintf(lastCommandSummaryBuffer, sizeof(lastCommandSummaryBuffer), "%s", value);
  stateDirty = true;
}

void setLastCommandSummary2(const char* prefix, const char* value) {
  snprintf(lastCommandSummaryBuffer, sizeof(lastCommandSummaryBuffer), "%s%s", prefix, value);
  stateDirty = true;
}

void printResetDiagnostics() {
  Serial.print("setup reset_status");
#if defined(R_SYSTEM)
  Serial.print(" RSTSR0=0x");
  Serial.print((uint8_t)R_SYSTEM->RSTSR0, HEX);
  Serial.print(" RSTSR1=0x");
  Serial.print((uint16_t)R_SYSTEM->RSTSR1, HEX);
  Serial.print(" RSTSR2=0x");
  Serial.print((uint8_t)R_SYSTEM->RSTSR2, HEX);
#else
  Serial.print(" unavailable");
#endif
  Serial.println();
}

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

bool publishText(const char* label, const String& topic, const char* text, bool retained) {
  const size_t payloadSize = strlen(text);
  Serial.print(label);
  Serial.print(" payload_len=");
  Serial.print(payloadSize);

  const int beginOk = mqttClient.beginMessage(topic.c_str(), (unsigned long)payloadSize, retained);
  size_t mqttBytesWritten = 0;
  int endOk = 0;
  if (beginOk) {
    mqttBytesWritten = mqttClient.print(text);
    endOk = mqttClient.endMessage();
  }

  Serial.print(" mqtt_begin=");
  Serial.print(beginOk);
  Serial.print(" mqtt_written=");
  Serial.print(mqttBytesWritten);
  Serial.print(" mqtt_end=");
  Serial.println(endOk);
  if (!beginOk || !endOk || mqttBytesWritten != payloadSize) {
    Serial.print(label);
    Serial.println(" ERROR: MQTT publish did not write the full payload");
    return false;
  }
  return true;
}

template <typename TDocument>
bool publishJsonDocument(const char* label, const String& topic, bool retained, TDocument& doc, size_t capacity) {
  const size_t measuredJsonSize = measureJson(doc);
  const bool jsonOverflowed = doc.overflowed();
  Serial.print(label);
  Serial.print(" json_capacity=");
  Serial.print(capacity);
  Serial.print(" measured=");
  Serial.print(measuredJsonSize);
  Serial.print(" overflowed=");
  Serial.println(jsonOverflowed ? "true" : "false");
  if (jsonOverflowed) {
    Serial.print(label);
    Serial.println(" ERROR: ArduinoJson overflowed; payload is incomplete");
  }

  const int beginOk = mqttClient.beginMessage(topic.c_str(), (unsigned long)measuredJsonSize, retained);
  size_t mqttBytesWritten = 0;
  int endOk = 0;
  if (beginOk) {
    mqttBytesWritten = serializeJson(doc, mqttClient);
    endOk = mqttClient.endMessage();
  }

  Serial.print(label);
  Serial.print(" mqtt_begin=");
  Serial.print(beginOk);
  Serial.print(" mqtt_written=");
  Serial.print(mqttBytesWritten);
  Serial.print(" mqtt_end=");
  Serial.println(endOk);
  if (!beginOk || !endOk || mqttBytesWritten != measuredJsonSize) {
    Serial.print(label);
    Serial.println(" ERROR: MQTT publish did not write the full payload");
    return false;
  }
  return !jsonOverflowed;
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

void writePinDescriptor(JsonObject target, int pin, const char* label) {
  target["pin"] = pin;
  target["label"] = label;
}

void writeSwitchInputPinOptions(JsonArray pinOptions) {
  for (size_t i = 0; i < SWITCH_INPUT_PIN_COUNT; i++) {
    JsonObject option = pinOptions.createNestedObject();
    writePinDescriptor(option, SWITCH_INPUT_PIN_OPTIONS[i], SWITCH_INPUT_PIN_LABELS[i]);
  }
}

void publishCapabilities() {
  StaticJsonDocument<CAPABILITIES_JSON_CAPACITY> doc;
  doc["sketch"] = SKETCH_ID;
  doc["firmware_version"] = FIRMWARE_VERSION;

  JsonArray switchModes = doc.createNestedArray("supported_switch_modes");
  switchModes.add("NO");
  switchModes.add("NC");

  JsonArray pullModes = doc.createNestedArray("supported_pull_modes");
  pullModes.add("pullup");
  pullModes.add("pulldown");
  pullModes.add("external");

  JsonObject outputPins = doc.createNestedObject("output_pins");
  writePinDescriptor(outputPins.createNestedObject("relay_1"), RELAY_1_PIN, RELAY_1_PIN_LABEL);
  writePinDescriptor(outputPins.createNestedObject("relay_2"), RELAY_2_PIN, RELAY_2_PIN_LABEL);
  writePinDescriptor(outputPins.createNestedObject("ssr_1"), SSR_1_PIN, SSR_1_PIN_LABEL);
  writePinDescriptor(outputPins.createNestedObject("pwm"), PWM_PIN, PWM_PIN_LABEL);

  JsonObject inputPins = doc.createNestedObject("input_pins");
  writePinDescriptor(inputPins.createNestedObject("pot"), POT_PIN, POT_PIN_LABEL);

  JsonArray pinOptions = doc.createNestedArray("switch_input_pin_options");
  writeSwitchInputPinOptions(pinOptions);

  publishJsonDocument("publishCapabilities", capabilitiesTopic, true, doc, CAPABILITIES_JSON_CAPACITY);
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

bool shouldIncludeSwitchTests(bool forceSwitchTests) {
#if LVTP_ENABLE_FULL_SWITCH_TEST_STATE
  (void)forceSwitchTests;
  return true;
#else
  if (forceSwitchTests) {
    return true;
  }
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    if (switchConfigs[i].enabled || switchReadings[i].configured || switchResults[i].available) {
      return true;
    }
  }
  return false;
#endif
}

void publishState(bool retained, bool forceSwitchTests = false) {
  StaticJsonDocument<STATE_JSON_CAPACITY> doc;
  JsonObject out = doc.createNestedObject("outputs");
  out["relay_1"] = outputs.relay1;
  out["relay_2"] = outputs.relay2;
  out["ssr_1"] = outputs.ssr1;
  out["pwm_enabled"] = outputs.pwmEnabled;
  out["pwm_value"] = outputs.pwmValue;

  const bool includeSwitchTests = shouldIncludeSwitchTests(forceSwitchTests);
  if (includeSwitchTests) {
    JsonObject switchTests = doc.createNestedObject("switch_tests");
    for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
      JsonObject channel = switchTests.createNestedObject(SWITCH_CHANNEL_KEYS[i]);
      writeSwitchChannel(channel, i);
    }
  }

  doc["uptime_ms"] = millis();
  doc["last_command"] = lastCommandSummaryBuffer;
  doc["switch_tests_included"] = includeSwitchTests;

  if (publishJsonDocument("publishState", stateTopic, retained, doc, STATE_JSON_CAPACITY)) {
    stateDirty = false;
  }
}

void publishTelemetry() {
  StaticJsonDocument<256> doc;
  doc["uptime_ms"] = millis();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["pot_value"] = analogRead(POT_PIN);
  doc["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  doc["mqtt_connected"] = mqttClient.connected();

  publishJsonDocument("publishTelemetry", telemetryTopic, false, doc, 256);
}

void updateLastCommandSummary(const JsonObject& out) {
  char summary[96] = "";
  size_t used = 0;

#define APPEND_SUMMARY_PART(label, value) \
  do { \
    int written = snprintf(summary + used, sizeof(summary) - used, "%s%s%s", used > 0 ? " " : "", label, value); \
    if (written > 0) { \
      used += (size_t)written; \
      if (used >= sizeof(summary)) used = sizeof(summary) - 1; \
    } \
  } while (0)

  if (out.containsKey("relay_1")) {
    APPEND_SUMMARY_PART("relay_1:", outputs.relay1 ? "on" : "off");
  }
  if (out.containsKey("relay_2")) {
    APPEND_SUMMARY_PART("relay_2:", outputs.relay2 ? "on" : "off");
  }
  if (out.containsKey("ssr_1")) {
    APPEND_SUMMARY_PART("ssr_1:", outputs.ssr1 ? "on" : "off");
  }
  if (out.containsKey("pwm_enabled")) {
    APPEND_SUMMARY_PART("pwm_enabled:", outputs.pwmEnabled ? "on" : "off");
  }
  if (out.containsKey("pwm_value")) {
    char pwmValue[8];
    snprintf(pwmValue, sizeof(pwmValue), "%d", outputs.pwmValue);
    APPEND_SUMMARY_PART("pwm:", pwmValue);
  }
  setLastCommandSummary(used > 0 ? summary : "outputs");

#undef APPEND_SUMMARY_PART
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
  Serial.print("handleCommand message_size=");
  Serial.println(messageSize);

  char payload[1537];
  size_t payloadLength = 0;
  while (mqttClient.available() && payloadLength < sizeof(payload) - 1) {
    int next = mqttClient.read();
    if (next < 0) {
      break;
    }
    payload[payloadLength++] = (char)next;
  }
  payload[payloadLength] = '\0';
  if (messageSize >= (int)sizeof(payload)) {
    Serial.println("handleCommand WARNING: payload truncated to command buffer size");
  }
  while (mqttClient.available()) {
    mqttClient.read();
  }

  StaticJsonDocument<1536> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("handleCommand invalid_json error=");
    Serial.println(error.c_str());
    setLastCommandSummary("invalid_json");
    publishState(true, true);
    return;
  }

  bool handled = false;
  JsonObject switchTests = doc["switch_tests"];
  if (updateSwitchConfigs(switchTests)) {
    setLastCommandSummary("switch_config");
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
      setLastCommandSummary2("switch_read:", SWITCH_CHANNEL_KEYS[index]);
    } else {
      setLastCommandSummary("switch_read:invalid_channel");
    }
    handled = true;
  }

  if (doc.containsKey("test_switch")) {
    const char* channel = doc["test_switch"].as<const char*>();
    int index = switchChannelIndex(channel);
    if (index >= 0) {
      runSwitchTest((size_t)index);
      setLastCommandSummary2("switch_test:", SWITCH_CHANNEL_KEYS[index]);
    } else {
      setLastCommandSummary("switch_test:invalid_channel");
    }
    handled = true;
  }

  if (!handled) {
    setLastCommandSummary("no_command");
  }

  applyOutputs();
  publishState(true, true);
}

void connectWifi() {
  int wifiStatus = WiFi.status();
  if (wifiStatus == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.print("WiFi connected ip=");
      Serial.print(WiFi.localIP());
      Serial.print(" rssi=");
      Serial.println(WiFi.RSSI());
      wifiWasConnected = true;
    }
    return;
  }

  if (wifiWasConnected) {
    Serial.print("WiFi disconnected status=");
    Serial.println(wifiStatus);
    wifiWasConnected = false;
  }

  unsigned long now = millis();
  if (now - lastWifiAttempt < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }
  lastWifiAttempt = now;

  Serial.print("Connecting to WiFi status=");
  Serial.print(wifiStatus);
  Serial.print(" uptime_ms=");
  Serial.println(now);
  int beginStatus = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi begin result=");
  Serial.println(beginStatus);
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

  int wifiStatus = WiFi.status();
  if (wifiStatus != WL_CONNECTED) {
    Serial.print("MQTT connect skipped; wifi_status=");
    Serial.print(wifiStatus);
    Serial.print(" uptime_ms=");
    Serial.println(now);
    return;
  }

  Serial.print("Connecting to MQTT ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print(" uptime_ms=");
  Serial.println(now);

  if (mqttClient.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.print("MQTT connected subscribed_cmd=");
    mqttWasConnected = true;
    int subscribeOk = mqttClient.subscribe(cmdTopic.c_str());
    Serial.println(subscribeOk);
    publishText("publishStatus", statusTopic, "online", true);
#if LVTP_ENABLE_CAPABILITIES_PUBLISH
    publishCapabilities();
#else
    Serial.println("publishCapabilities disabled by LVTP_ENABLE_CAPABILITIES_PUBLISH=0");
#endif
    publishState(true);
  } else {
    Serial.print("MQTT failed, error ");
    Serial.print(mqttClient.connectError());
    Serial.print(" wifi_status=");
    Serial.println(wifiStatus);
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
  capabilitiesTopic = base + "/capabilities";
  telemetryTopic = base + "/telemetry";
  statusTopic = base + "/status";
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.print("setup start sketch=");
  Serial.print(SKETCH_ID);
  Serial.print(" firmware=");
  Serial.print(FIRMWARE_VERSION);
  Serial.print(" uptime_ms=");
  Serial.println(millis());
  printResetDiagnostics();
  Serial.print("feature capabilities_publish=");
  Serial.print(LVTP_ENABLE_CAPABILITIES_PUBLISH);
  Serial.print(" full_switch_test_state=");
  Serial.println(LVTP_ENABLE_FULL_SWITCH_TEST_STATE);

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
  Serial.println("setup complete");
}

void loop() {
  connectWifi();

  if (!mqttClient.connected()) {
    if (mqttWasConnected) {
      Serial.print("MQTT disconnected; forcing outputs off uptime_ms=");
      Serial.print(millis());
      Serial.print(" wifi_status=");
      Serial.print(WiFi.status());
      Serial.print(" connect_error=");
      Serial.println(mqttClient.connectError());
      allOutputsOff();
      mqttWasConnected = false;
      stateDirty = true;
    }
    connectMqtt();
  }

  mqttClient.poll();

  unsigned long now = millis();
  if (now - lastPollDiagnostic >= MQTT_POLL_DIAGNOSTIC_INTERVAL_MS) {
    lastPollDiagnostic = now;
    Serial.print("mqttPoll uptime_ms=");
    Serial.print(now);
    Serial.print(" mqtt_connected=");
    Serial.print(mqttClient.connected());
    Serial.print(" wifi_status=");
    Serial.print(WiFi.status());
    Serial.print(" state_dirty=");
    Serial.println(stateDirty);
  }
  if (mqttClient.connected() && now - lastTelemetryPublish >= TELEMETRY_PUBLISH_INTERVAL_MS) {
    lastTelemetryPublish = now;
    publishTelemetry();
  }
  if (mqttClient.connected() && (stateDirty || now - lastStatePublish >= STATE_PUBLISH_INTERVAL_MS)) {
    lastStatePublish = now;
    publishState(true);
  }
}
