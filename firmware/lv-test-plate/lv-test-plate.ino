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
const unsigned long DEFAULT_OBSERVATION_DURATION_S = 30;
const unsigned long SWITCH_SERVICE_INTERVAL_MS = 50;

const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const unsigned long STATE_PUBLISH_INTERVAL_MS = 5000;
const unsigned long TELEMETRY_PUBLISH_INTERVAL_MS = 2000;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
const unsigned long MQTT_POLL_DIAGNOSTIC_INTERVAL_MS = 5000;
const unsigned long PUBLISH_WARN_MS = 100;
const unsigned long LOOP_WARN_MS = 100;
const char* SKETCH_ID = "lv-test-plate";
const char* FIRMWARE_VERSION = "2026-05-29-mqtt-responsive-switch-events-1";
const size_t STATE_JSON_CAPACITY = 6144;
const size_t CAPABILITIES_JSON_CAPACITY = 2048;
const size_t SWITCH_EVENT_JSON_CAPACITY = 2048;

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
String switchTopic;

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

enum SwitchTestMode {
  SWITCH_TEST_PASSIVE_READ,
  SWITCH_TEST_RELAY_FOLLOW,
  SWITCH_TEST_TIMED_OBSERVATION,
  SWITCH_TEST_OUTPUT_FEEDBACK
};

struct SwitchConfig {
  bool enabled = false;
  int pin = NO_SWITCH_PIN;
  SwitchContactMode mode = SWITCH_MODE_NO;
  SwitchPullMode pullMode = SWITCH_PULL_UP;
  SwitchTestMode testMode = SWITCH_TEST_PASSIVE_READ;
  unsigned long debounceMs = DEFAULT_SWITCH_DEBOUNCE_MS;
  unsigned long settleMs = DEFAULT_SWITCH_SETTLE_MS;
  unsigned long observationDurationS = DEFAULT_OBSERVATION_DURATION_S;
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

struct SwitchObservation {
  bool active = false;
  bool available = false;
  unsigned long durationMs = DEFAULT_OBSERVATION_DURATION_S * 1000UL;
  unsigned long startedAt = 0;
  unsigned long endedAt = 0;
  SwitchReading start;
  SwitchReading current;
  SwitchReading end;
  bool hasLast = false;
  bool lastClosed = false;
  unsigned int transitionCount = 0;
  unsigned int openToClosedCount = 0;
  unsigned int closedToOpenCount = 0;
  const char* status = "not_started";
};

Outputs outputs;
SwitchConfig switchConfigs[SWITCH_CHANNEL_COUNT];
SwitchReading switchReadings[SWITCH_CHANNEL_COUNT];
SwitchTestResult switchResults[SWITCH_CHANNEL_COUNT];
SwitchObservation switchObservations[SWITCH_CHANNEL_COUNT];
const char* relayCommandedBy[SWITCH_CHANNEL_COUNT] = {"manual", "manual"};
const char* switchChannelStatus[SWITCH_CHANNEL_COUNT] = {"not_configured", "not_configured"};
char lastCommandSummaryBuffer[96] = "";
char lastCommandId[48] = "";
unsigned long lastCommandSeq = 0;
unsigned long switchChannelSeq[SWITCH_CHANNEL_COUNT] = {0, 0};
unsigned long lastCommandReceivedMs = 0;
unsigned long lastSwitchSampledMs = 0;
unsigned long lastStatePublishStartedMs = 0;
unsigned long lastSwitchEventPublishStartedMs = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastStatePublish = 0;
unsigned long lastTelemetryPublish = 0;
unsigned long lastPollDiagnostic = 0;
unsigned long lastSwitchService = 0;
unsigned long lastLoopDurationMs = 0;
bool switchEventDirty[SWITCH_CHANNEL_COUNT] = {false, false};
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

void setLastCommandId(const char* value) {
  snprintf(lastCommandId, sizeof(lastCommandId), "%s", value == nullptr ? "" : value);
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

void allOutputsOff(const char* source = "safety") {
  outputs.relay1 = false;
  outputs.relay2 = false;
  outputs.ssr1 = false;
  outputs.pwmEnabled = false;
  outputs.pwmValue = 0;
  relayCommandedBy[0] = source;
  relayCommandedBy[1] = source;
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

const char* switchTestModeName(SwitchTestMode mode) {
  if (mode == SWITCH_TEST_RELAY_FOLLOW) {
    return "relay_follow";
  }
  if (mode == SWITCH_TEST_TIMED_OBSERVATION) {
    return "timed_observation";
  }
  if (mode == SWITCH_TEST_OUTPUT_FEEDBACK) {
    return "output_feedback";
  }
  return "passive_read";
}

SwitchTestMode parseSwitchTestMode(const char* raw, SwitchTestMode fallback) {
  if (raw == nullptr) {
    return fallback;
  }
  String value = raw;
  value.trim();
  value.toLowerCase();
  if (value == "relay_follow" || value == "follow") {
    return SWITCH_TEST_RELAY_FOLLOW;
  }
  if (value == "timed_observation" || value == "observe") {
    return SWITCH_TEST_TIMED_OBSERVATION;
  }
  if (value == "output_feedback" || value == "feedback" || value == "output_driven") {
    return SWITCH_TEST_OUTPUT_FEEDBACK;
  }
  if (value == "passive_read" || value == "passive" || value == "read") {
    return SWITCH_TEST_PASSIVE_READ;
  }
  return fallback;
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
    if (index < SWITCH_CHANNEL_COUNT) {
      switchReadings[index] = reading;
    }
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

void setSwitchRelayOutput(size_t index, bool on, const char* source) {
  if (index == 0) {
    outputs.relay1 = on;
    relayCommandedBy[0] = source;
  } else if (index == 1) {
    outputs.relay2 = on;
    relayCommandedBy[1] = source;
  }
  applyOutputs();
}

bool switchRelayOutput(size_t index) {
  if (index == 0) {
    return outputs.relay1;
  }
  if (index == 1) {
    return outputs.relay2;
  }
  return false;
}

bool switchReadingChanged(const SwitchReading& before, const SwitchReading& after) {
  return before.configured != after.configured || before.raw != after.raw || before.closed != after.closed;
}

void startSwitchObservation(size_t index) {
  if (index >= SWITCH_CHANNEL_COUNT) {
    return;
  }

  SwitchObservation observation;
  observation.available = true;
  observation.active = false;
  observation.durationMs = constrain(switchConfigs[index].observationDurationS, 1, 3600) * 1000UL;
  observation.startedAt = millis();
  observation.status = "not_configured";

  if (switchConfigured(index)) {
    observation.start = readSwitch(index);
    observation.current = observation.start;
    observation.end = observation.start;
    observation.lastClosed = observation.start.closed;
    observation.hasLast = observation.start.configured;
    observation.active = true;
    observation.status = "observing";
    switchChannelStatus[index] = "observing";
  } else {
    switchChannelStatus[index] = "not_configured";
  }

  switchObservations[index] = observation;
  stateDirty = true;
  switchEventDirty[index] = true;
}

void finishSwitchObservation(size_t index, const char* status) {
  if (index >= SWITCH_CHANNEL_COUNT) {
    return;
  }
  SwitchObservation& observation = switchObservations[index];
  observation.active = false;
  observation.available = true;
  observation.endedAt = millis();
  observation.end = observation.current;
  observation.status = status;
  switchChannelStatus[index] = status;
  stateDirty = true;
  switchEventDirty[index] = true;
}

void serviceSwitchObservation(size_t index) {
  SwitchObservation& observation = switchObservations[index];
  if (!observation.active) {
    return;
  }
  if (!switchConfigured(index)) {
    finishSwitchObservation(index, "not_configured");
    return;
  }

  SwitchReading reading = readSwitch(index);
  observation.current = reading;
  if (reading.configured && observation.hasLast && reading.closed != observation.lastClosed) {
    observation.transitionCount++;
    if (!observation.lastClosed && reading.closed) {
      observation.openToClosedCount++;
    } else if (observation.lastClosed && !reading.closed) {
      observation.closedToOpenCount++;
    }
    observation.lastClosed = reading.closed;
    stateDirty = true;
    switchEventDirty[index] = true;
  } else if (reading.configured && !observation.hasLast) {
    observation.hasLast = true;
    observation.lastClosed = reading.closed;
    stateDirty = true;
    switchEventDirty[index] = true;
  }

  unsigned long elapsed = millis() - observation.startedAt;
  if (elapsed >= observation.durationMs) {
    finishSwitchObservation(index, observation.transitionCount > 0 ? "activity_observed" : "no_activity");
  }
}

void serviceSwitchModes() {
  unsigned long now = millis();
  if (now - lastSwitchService < SWITCH_SERVICE_INTERVAL_MS) {
    return;
  }
  lastSwitchService = now;

  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    SwitchConfig& config = switchConfigs[i];
    if (!config.enabled) {
      switchChannelStatus[i] = "disabled";
      continue;
    }

    if (config.testMode == SWITCH_TEST_RELAY_FOLLOW) {
      if (!switchConfigured(i)) {
        if (switchRelayOutput(i)) {
          setSwitchRelayOutput(i, false, "safety");
          stateDirty = true;
          switchEventDirty[i] = true;
        }
        relayCommandedBy[i] = "safety";
        switchChannelStatus[i] = "relay_follow_not_configured";
        continue;
      }
      SwitchReading before = switchReadings[i];
      SwitchReading reading = readSwitch(i);
      bool wasOn = switchRelayOutput(i);
      if (wasOn != reading.closed) {
        setSwitchRelayOutput(i, reading.closed, "switch");
        stateDirty = true;
        switchEventDirty[i] = true;
      }
      if (switchReadingChanged(before, reading)) {
        stateDirty = true;
        switchEventDirty[i] = true;
      }
      switchChannelStatus[i] = "relay_following";
      continue;
    }

    if (config.testMode == SWITCH_TEST_TIMED_OBSERVATION) {
      serviceSwitchObservation(i);
      if (!switchObservations[i].active && switchConfigured(i)) {
        SwitchReading before = switchReadings[i];
        SwitchReading reading = readSwitch(i);
        if (switchReadingChanged(before, reading)) {
          stateDirty = true;
          switchEventDirty[i] = true;
        }
      }
      if (!switchObservations[i].active) {
        switchChannelStatus[i] = switchConfigured(i) ? "observation_ready" : "not_configured";
      }
      continue;
    }

    if (config.testMode == SWITCH_TEST_PASSIVE_READ) {
      if (switchConfigured(i)) {
        SwitchReading before = switchReadings[i];
        SwitchReading reading = readSwitch(i);
        if (switchReadingChanged(before, reading)) {
          stateDirty = true;
          switchEventDirty[i] = true;
        }
        switchChannelStatus[i] = "passive_read";
      } else {
        switchChannelStatus[i] = "not_configured";
      }
      continue;
    }

    switchChannelStatus[i] = "output_feedback_ready";
  }
}

void runSwitchTest(size_t index) {
  if (index >= SWITCH_CHANNEL_COUNT) {
    return;
  }

  SwitchTestResult result;
  result.available = true;
  result.settleMs = switchConfigs[index].settleMs;

  if (switchConfigs[index].testMode != SWITCH_TEST_OUTPUT_FEEDBACK) {
    result.status = "wrong_mode";
    switchResults[index] = result;
    return;
  }

  if (!switchConfigured(index)) {
    result.status = "not_configured";
    switchResults[index] = result;
    return;
  }

  setSwitchRelayOutput(index, false, "feedback_test");
  result.before = readSwitch(index);
  setSwitchRelayOutput(index, true, "feedback_test");
  delay(switchConfigs[index].settleMs > 5000 ? 5000 : switchConfigs[index].settleMs);
  result.after = readSwitch(index);
  result.changed = result.before.configured && result.after.configured && result.before.closed != result.after.closed;

  bool expectBeforeClosed = switchConfigs[index].mode == SWITCH_MODE_NC;
  bool expectAfterClosed = switchConfigs[index].mode == SWITCH_MODE_NO;
  result.pass = result.before.configured && result.after.configured && result.before.closed == expectBeforeClosed && result.after.closed == expectAfterClosed && result.changed;
  result.status = result.pass ? "pass" : "fail";

  setSwitchRelayOutput(index, false, "feedback_test");
  switchResults[index] = result;
}

bool publishText(const char* label, const String& topic, const char* text, bool retained) {
  const size_t payloadSize = strlen(text);
  Serial.print(label);
  Serial.print(" payload_len=");
  Serial.print(payloadSize);

  mqttClient.poll();
  const unsigned long publishStarted = millis();
  const int beginOk = mqttClient.beginMessage(topic.c_str(), (unsigned long)payloadSize, retained);
  size_t mqttBytesWritten = 0;
  int endOk = 0;
  if (beginOk) {
    mqttBytesWritten = mqttClient.print(text);
    endOk = mqttClient.endMessage();
  }
  const unsigned long publishDuration = millis() - publishStarted;
  mqttClient.poll();

  Serial.print(" mqtt_begin=");
  Serial.print(beginOk);
  Serial.print(" mqtt_written=");
  Serial.print(mqttBytesWritten);
  Serial.print(" mqtt_end=");
  Serial.print(endOk);
  Serial.print(" duration_ms=");
  Serial.println(publishDuration);
  if (publishDuration > PUBLISH_WARN_MS) {
    Serial.print(label);
    Serial.print(" WARNING: publish duration_ms=");
    Serial.println(publishDuration);
  }
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

  mqttClient.poll();
  const unsigned long publishStarted = millis();
  const int beginOk = mqttClient.beginMessage(topic.c_str(), (unsigned long)measuredJsonSize, retained);
  size_t mqttBytesWritten = 0;
  int endOk = 0;
  if (beginOk) {
    mqttBytesWritten = serializeJson(doc, mqttClient);
    endOk = mqttClient.endMessage();
  }
  const unsigned long publishDuration = millis() - publishStarted;
  mqttClient.poll();

  Serial.print(label);
  Serial.print(" mqtt_begin=");
  Serial.print(beginOk);
  Serial.print(" mqtt_written=");
  Serial.print(mqttBytesWritten);
  Serial.print(" mqtt_end=");
  Serial.print(endOk);
  Serial.print(" duration_ms=");
  Serial.println(publishDuration);
  if (publishDuration > PUBLISH_WARN_MS) {
    Serial.print(label);
    Serial.print(" WARNING: publish duration_ms=");
    Serial.println(publishDuration);
  }
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

void writeSwitchObservation(JsonObject target, const SwitchObservation& observation) {
  target["active"] = observation.active;
  target["available"] = observation.available;
  target["status"] = observation.status;
  target["duration_s"] = observation.durationMs / 1000UL;
  target["started_at_ms"] = observation.startedAt;
  target["ended_at_ms"] = observation.endedAt;
  unsigned long elapsed = observation.startedAt > 0 ? millis() - observation.startedAt : 0;
  target["remaining_ms"] = observation.active && elapsed < observation.durationMs ? observation.durationMs - elapsed : 0;
  target["transition_count"] = observation.transitionCount;
  target["open_to_closed_count"] = observation.openToClosedCount;
  target["closed_to_open_count"] = observation.closedToOpenCount;
  JsonObject start = target.createNestedObject("start");
  writeSwitchReading(start, observation.start);
  JsonObject current = target.createNestedObject("current");
  writeSwitchReading(current, observation.current);
  JsonObject end = target.createNestedObject("end");
  writeSwitchReading(end, observation.end);
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

  JsonArray testModes = doc.createNestedArray("supported_switch_test_modes");
  testModes.add("passive_read");
  testModes.add("relay_follow");
  testModes.add("timed_observation");
  testModes.add("output_feedback");

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
  target["test_mode"] = switchTestModeName(config.testMode);
  target["effective_pull_mode"] = effectiveSwitchPullModeName(config.pullMode);
  target["debounce_ms"] = config.debounceMs;
  target["settle_ms"] = config.settleMs;
  target["observation_duration_s"] = config.observationDurationS;
  target["configured"] = switchConfigured(index);
  target["relay_follow_enabled"] = config.testMode == SWITCH_TEST_RELAY_FOLLOW;
  target["commanded_by"] = relayCommandedBy[index];
  target["status"] = switchChannelStatus[index];
  target["command_id"] = lastCommandId;
  target["command_seq"] = switchChannelSeq[index];

  JsonObject current = target.createNestedObject("current");
  writeSwitchReading(current, switchReadings[index]);

  JsonObject observation = target.createNestedObject("observation");
  writeSwitchObservation(observation, switchObservations[index]);

  JsonObject lastTest = target.createNestedObject("last_test");
  writeSwitchTestResult(lastTest, switchResults[index]);
}

void writeSwitchChannelCompact(JsonObject target, size_t index, bool includeObservation, bool includeLastTest) {
  SwitchConfig& config = switchConfigs[index];
  target["enabled"] = config.enabled;
  target["pin"] = config.pin;
  target["pin_label"] = switchPinLabel(config.pin);
  target["mode"] = switchModeName(config.mode);
  target["pull_mode"] = switchPullModeName(config.pullMode);
  target["test_mode"] = switchTestModeName(config.testMode);
  target["effective_pull_mode"] = effectiveSwitchPullModeName(config.pullMode);
  target["debounce_ms"] = config.debounceMs;
  target["settle_ms"] = config.settleMs;
  target["observation_duration_s"] = config.observationDurationS;
  target["configured"] = switchConfigured(index);
  target["relay_follow_enabled"] = config.testMode == SWITCH_TEST_RELAY_FOLLOW;
  target["commanded_by"] = relayCommandedBy[index];
  target["status"] = switchChannelStatus[index];
  target["command_id"] = lastCommandId;
  target["command_seq"] = switchChannelSeq[index];

  JsonObject current = target.createNestedObject("current");
  writeSwitchReading(current, switchReadings[index]);

  if (includeObservation || switchObservations[index].active || switchObservations[index].available) {
    JsonObject observation = target.createNestedObject("observation");
    writeSwitchObservation(observation, switchObservations[index]);
  }

  if (includeLastTest || switchResults[index].available) {
    JsonObject lastTest = target.createNestedObject("last_test");
    writeSwitchTestResult(lastTest, switchResults[index]);
  }
}

bool publishSwitchEvent(const char* eventName, int channelIndex, bool retained, bool includeObservation = false, bool includeLastTest = false) {
  if (channelIndex < 0 || channelIndex >= (int)SWITCH_CHANNEL_COUNT) {
    return false;
  }

  lastSwitchEventPublishStartedMs = millis();
  StaticJsonDocument<SWITCH_EVENT_JSON_CAPACITY> doc;
  doc["event"] = eventName;
  doc["channel"] = SWITCH_CHANNEL_KEYS[channelIndex];
  doc["uptime_ms"] = millis();
  doc["last_command"] = lastCommandSummaryBuffer;
  doc["command_id"] = lastCommandId;
  doc["command_seq"] = lastCommandSeq;
  JsonObject timing = doc.createNestedObject("timing");
  timing["firmware_command_received_ms"] = lastCommandReceivedMs;
  timing["firmware_switch_sampled_ms"] = lastSwitchSampledMs;
  timing["firmware_state_publish_started_ms"] = lastStatePublishStartedMs;
  timing["firmware_switch_publish_started_ms"] = lastSwitchEventPublishStartedMs;

  JsonObject switchTests = doc.createNestedObject("switch_tests");
  JsonObject channel = switchTests.createNestedObject(SWITCH_CHANNEL_KEYS[channelIndex]);
  writeSwitchChannelCompact(channel, (size_t)channelIndex, includeObservation, includeLastTest);

  bool ok = publishJsonDocument("publishSwitchEvent", switchTopic, retained, doc, SWITCH_EVENT_JSON_CAPACITY);
  if (ok) {
    switchEventDirty[channelIndex] = false;
  }
  return ok;
}

bool shouldIncludeSwitchTests(bool forceSwitchTests) {
#if LVTP_ENABLE_FULL_SWITCH_TEST_STATE
  (void)forceSwitchTests;
  return true;
#else
  return forceSwitchTests;
#endif
}

void publishState(bool retained, bool forceSwitchTests = false, int onlySwitchChannel = -1) {
  lastStatePublishStartedMs = millis();
  Serial.print("publishState start command_id=");
  Serial.print(lastCommandId);
  Serial.print(" started_ms=");
  Serial.print(lastStatePublishStartedMs);
  Serial.print(" force_switch_tests=");
  Serial.print(forceSwitchTests ? "true" : "false");
  Serial.print(" only_switch_channel=");
  Serial.println(onlySwitchChannel);
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
    if (onlySwitchChannel >= 0 && onlySwitchChannel < (int)SWITCH_CHANNEL_COUNT) {
      JsonObject channel = switchTests.createNestedObject(SWITCH_CHANNEL_KEYS[onlySwitchChannel]);
      writeSwitchChannel(channel, (size_t)onlySwitchChannel);
    } else {
      for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
        JsonObject channel = switchTests.createNestedObject(SWITCH_CHANNEL_KEYS[i]);
        writeSwitchChannel(channel, i);
      }
    }
  }

  doc["uptime_ms"] = millis();
  doc["last_command"] = lastCommandSummaryBuffer;
  doc["command_id"] = lastCommandId;
  doc["command_seq"] = lastCommandSeq;
  doc["switch_tests_included"] = includeSwitchTests;
  JsonObject timing = doc.createNestedObject("timing");
  timing["firmware_command_received_ms"] = lastCommandReceivedMs;
  timing["firmware_switch_sampled_ms"] = lastSwitchSampledMs;
  timing["firmware_state_publish_started_ms"] = lastStatePublishStartedMs;

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

bool updateSwitchConfig(size_t index, JsonObject configDoc, unsigned long commandSeq) {
  if (index >= SWITCH_CHANNEL_COUNT || configDoc.isNull()) {
    return false;
  }

  SwitchConfig& config = switchConfigs[index];
  bool changed = false;
  bool requestedEnabled = config.enabled;

  if (configDoc.containsKey("enabled")) {
    requestedEnabled = configDoc["enabled"].as<bool>();
    config.enabled = requestedEnabled;
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
  if (configDoc.containsKey("test_mode")) {
    config.testMode = parseSwitchTestMode(configDoc["test_mode"].as<const char*>(), config.testMode);
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
  if (configDoc.containsKey("observation_duration_s")) {
    config.observationDurationS = constrain(configDoc["observation_duration_s"].as<int>(), 1, 3600);
    changed = true;
  }

  if (changed) {
    switchChannelSeq[index] = commandSeq;
    if (config.enabled && !isSwitchPinAllowed(config.pin)) {
      config.enabled = false;
      config.pin = NO_SWITCH_PIN;
      switchChannelStatus[index] = requestedEnabled ? "invalid_config" : "disabled";
    }
    configureSwitchPin(index);
    if (switchObservations[index].active && config.testMode != SWITCH_TEST_TIMED_OBSERVATION) {
      finishSwitchObservation(index, "cancelled");
    }
    if (config.testMode == SWITCH_TEST_RELAY_FOLLOW && !switchConfigured(index)) {
      setSwitchRelayOutput(index, false, "safety");
      switchChannelStatus[index] = "relay_follow_not_configured";
    }
  }
  return changed;
}

bool relayFollowOwnsOutput(size_t index) {
  return index < SWITCH_CHANNEL_COUNT && switchConfigs[index].enabled && switchConfigs[index].testMode == SWITCH_TEST_RELAY_FOLLOW;
}

bool updateSwitchConfigs(JsonObject switchTests, unsigned long commandSeq, int& touchedSwitchIndex) {
  if (switchTests.isNull()) {
    return false;
  }

  bool changed = false;
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    JsonObject configDoc = switchTests[SWITCH_CHANNEL_KEYS[i]];
    if (!configDoc.isNull()) {
      touchedSwitchIndex = touchedSwitchIndex == -1 ? (int)i : -2;
      changed = updateSwitchConfig(i, configDoc, commandSeq) || changed;
    }
  }
  return changed;
}

void handleCommand(int messageSize) {
  lastCommandReceivedMs = millis();
  Serial.print("handleCommand message_size=");
  Serial.print(messageSize);
  Serial.print(" received_ms=");
  Serial.println(lastCommandReceivedMs);

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
    publishState(true);
    return;
  }

  setLastCommandId(doc["command_id"].as<const char*>());
  lastCommandSeq = doc["command_seq"].as<unsigned long>();
  lastSwitchSampledMs = 0;
  Serial.print("handleCommand command_id=");
  Serial.print(lastCommandId);
  Serial.print(" command_seq=");
  Serial.print(lastCommandSeq);
  Serial.print(" client_click_ts_ms=");
  if (doc.containsKey("client_click_ts_ms")) {
    Serial.print(doc["client_click_ts_ms"].as<double>(), 0);
  } else {
    Serial.print("n/a");
  }
  Serial.print(" server_api_received_ts_ms=");
  if (doc.containsKey("server_api_received_ts_ms")) {
    Serial.print(doc["server_api_received_ts_ms"].as<double>(), 0);
  } else {
    Serial.print("n/a");
  }
  Serial.println();

  bool handled = false;
  int touchedSwitchIndex = -1;
  bool touchedSwitchChannels[SWITCH_CHANNEL_COUNT] = {false, false};
  bool includeSwitchObservation[SWITCH_CHANNEL_COUNT] = {false, false};
  bool includeSwitchLastTest[SWITCH_CHANNEL_COUNT] = {false, false};
  const char* switchEventNames[SWITCH_CHANNEL_COUNT] = {"switch_config", "switch_config"};
  JsonObject switchTests = doc["switch_tests"];
  bool hasSwitchConfigCommand = false;
  if (!switchTests.isNull()) {
    for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
      JsonObject configDoc = switchTests[SWITCH_CHANNEL_KEYS[i]];
      if (!configDoc.isNull()) {
        hasSwitchConfigCommand = true;
        touchedSwitchChannels[i] = true;
        switchEventDirty[i] = true;
        switchEventNames[i] = "switch_config";
      }
    }
  }
  if (updateSwitchConfigs(switchTests, lastCommandSeq, touchedSwitchIndex) || hasSwitchConfigCommand) {
    setLastCommandSummary("switch_config");
    handled = true;
  }

  JsonObject out = doc["outputs"];
  if (!out.isNull()) {
    if (out.containsKey("relay_1")) {
      if (relayFollowOwnsOutput(0)) {
        switchChannelStatus[0] = "manual_ignored_relay_follow";
      } else {
        outputs.relay1 = out["relay_1"].as<bool>();
        relayCommandedBy[0] = "manual";
      }
    }
    if (out.containsKey("relay_2")) {
      if (relayFollowOwnsOutput(1)) {
        switchChannelStatus[1] = "manual_ignored_relay_follow";
      } else {
        outputs.relay2 = out["relay_2"].as<bool>();
        relayCommandedBy[1] = "manual";
      }
    }
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
      switchChannelSeq[index] = lastCommandSeq;
      touchedSwitchIndex = (int)index;
      touchedSwitchChannels[index] = true;
      switchEventDirty[index] = true;
      switchEventNames[index] = "switch_read";
      SwitchReading reading = readSwitch((size_t)index);
      lastSwitchSampledMs = reading.sampledAt;
      Serial.print("read_switch command_id=");
      Serial.print(lastCommandId);
      Serial.print(" channel=");
      Serial.print(SWITCH_CHANNEL_KEYS[index]);
      Serial.print(" sampled_ms=");
      Serial.print(lastSwitchSampledMs);
      Serial.print(" raw=");
      Serial.print(reading.raw);
      Serial.print(" state=");
      Serial.println(reading.configured ? (reading.closed ? "closed" : "open") : "unconfigured");
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
      switchChannelSeq[index] = lastCommandSeq;
      touchedSwitchIndex = (int)index;
      touchedSwitchChannels[index] = true;
      switchEventDirty[index] = true;
      includeSwitchLastTest[index] = true;
      switchEventNames[index] = "switch_test";
      runSwitchTest((size_t)index);
      setLastCommandSummary2("switch_test:", SWITCH_CHANNEL_KEYS[index]);
    } else {
      setLastCommandSummary("switch_test:invalid_channel");
    }
    handled = true;
  }

  if (doc.containsKey("observe_switch")) {
    const char* channel = doc["observe_switch"].as<const char*>();
    int index = switchChannelIndex(channel);
    if (index >= 0) {
      switchChannelSeq[index] = lastCommandSeq;
      touchedSwitchIndex = (int)index;
      touchedSwitchChannels[index] = true;
      switchEventDirty[index] = true;
      includeSwitchObservation[index] = true;
      switchEventNames[index] = "switch_observe";
      if (switchConfigs[index].testMode == SWITCH_TEST_TIMED_OBSERVATION) {
        startSwitchObservation((size_t)index);
        setLastCommandSummary2("switch_observe:", SWITCH_CHANNEL_KEYS[index]);
      } else {
        setLastCommandSummary2("switch_observe_wrong_mode:", SWITCH_CHANNEL_KEYS[index]);
      }
    } else {
      setLastCommandSummary("switch_observe:invalid_channel");
    }
    handled = true;
  }

  if (doc.containsKey("stop_observation")) {
    const char* channel = doc["stop_observation"].as<const char*>();
    int index = switchChannelIndex(channel);
    if (index >= 0) {
      switchChannelSeq[index] = lastCommandSeq;
      touchedSwitchIndex = (int)index;
      touchedSwitchChannels[index] = true;
      switchEventDirty[index] = true;
      includeSwitchObservation[index] = true;
      switchEventNames[index] = "switch_observe_cancel";
      finishSwitchObservation((size_t)index, "cancelled");
      setLastCommandSummary2("switch_observe_cancel:", SWITCH_CHANNEL_KEYS[index]);
    } else {
      setLastCommandSummary("switch_observe_cancel:invalid_channel");
    }
    handled = true;
  }

  if (!handled) {
    setLastCommandSummary("no_command");
  }

  applyOutputs();
  bool publishedSwitchEvent = false;
  for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
    if (touchedSwitchChannels[i]) {
      publishSwitchEvent(switchEventNames[i], (int)i, true, includeSwitchObservation[i], includeSwitchLastTest[i]);
      publishedSwitchEvent = true;
    }
  }

  if (!publishedSwitchEvent || !out.isNull()) {
    publishState(true);
  }
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
    for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
      publishSwitchEvent("switch_snapshot", (int)i, true);
    }
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
  switchTopic = base + "/switch";
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
  const unsigned long loopStarted = millis();
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
  serviceSwitchModes();

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
    Serial.print(stateDirty);
    Serial.print(" last_loop_duration_ms=");
    Serial.println(lastLoopDurationMs);
  }
  if (mqttClient.connected() && now - lastTelemetryPublish >= TELEMETRY_PUBLISH_INTERVAL_MS) {
    lastTelemetryPublish = now;
    publishTelemetry();
  }
  if (mqttClient.connected()) {
    for (size_t i = 0; i < SWITCH_CHANNEL_COUNT; i++) {
      if (switchEventDirty[i]) {
        publishSwitchEvent("switch_update", (int)i, true, switchObservations[i].active || switchObservations[i].available, switchResults[i].available);
      }
    }
  }
  if (mqttClient.connected() && (stateDirty || now - lastStatePublish >= STATE_PUBLISH_INTERVAL_MS)) {
    lastStatePublish = now;
    publishState(true);
  }
  lastLoopDurationMs = millis() - loopStarted;
  if (lastLoopDurationMs > LOOP_WARN_MS) {
    Serial.print("loop WARNING duration_ms=");
    Serial.print(lastLoopDurationMs);
    Serial.print(" mqtt_connected=");
    Serial.print(mqttClient.connected());
    Serial.print(" state_dirty=");
    Serial.println(stateDirty);
  }
}
