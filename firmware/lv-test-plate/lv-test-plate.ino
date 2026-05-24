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
const int RELAY_1_PIN = 12;      // D12, digital relay output
const int RELAY_2_PIN = 13;      // D13, digital relay output
const int SSR_1_PIN = 3;         // D3, SSR output
const int PWM_PIN = 5;           // D5, PWM-capable output
const int POT_PIN = A0;          // A0, analog potentiometer input

// Output polarity. Relay modules and SSRs often use different active levels.
const int RELAY_ON_LEVEL = LOW;
const int RELAY_OFF_LEVEL = HIGH;
const int SSR_ON_LEVEL = HIGH;
const int SSR_OFF_LEVEL = LOW;

const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const unsigned long STATE_PUBLISH_INTERVAL_MS = 5000;
const unsigned long TELEMETRY_PUBLISH_INTERVAL_MS = 2000;

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

Outputs outputs;
String lastCommand = "";
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

void publishText(const String& topic, const char* text, bool retained) {
  mqttClient.beginMessage(topic.c_str(), retained);
  mqttClient.print(text);
  mqttClient.endMessage();
}

void publishState(bool retained) {
  StaticJsonDocument<1024> doc;
  JsonObject out = doc.createNestedObject("outputs");
  out["relay_1"] = outputs.relay1;
  out["relay_2"] = outputs.relay2;
  out["ssr_1"] = outputs.ssr1;
  out["pwm_enabled"] = outputs.pwmEnabled;
  out["pwm_value"] = outputs.pwmValue;
  doc["uptime_ms"] = millis();
  String stateLastCommand = lastCommand;
  if (stateLastCommand.length() > 320) {
    stateLastCommand = stateLastCommand.substring(0, 320);
  }
  doc["last_command"] = stateLastCommand;

  String payload;
  serializeJson(doc, payload);
  mqttClient.beginMessage(stateTopic.c_str(), retained);
  mqttClient.print(payload);
  mqttClient.endMessage();
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

void handleCommand(int messageSize) {
  String payload;
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    lastCommand = "invalid_json";
    publishState(true);
    return;
  }

  JsonObject out = doc["outputs"];
  if (!out.isNull()) {
    if (out.containsKey("relay_1")) outputs.relay1 = out["relay_1"].as<bool>();
    if (out.containsKey("relay_2")) outputs.relay2 = out["relay_2"].as<bool>();
    if (out.containsKey("ssr_1")) outputs.ssr1 = out["ssr_1"].as<bool>();
    if (out.containsKey("pwm_enabled")) outputs.pwmEnabled = out["pwm_enabled"].as<bool>();
    if (out.containsKey("pwm_value")) outputs.pwmValue = constrain(out["pwm_value"].as<int>(), 0, 255);
  }

  lastCommand = payload;
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
