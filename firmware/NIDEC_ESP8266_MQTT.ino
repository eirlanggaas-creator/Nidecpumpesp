#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Isi kredensial lokal; jangan commit password ke repository publik.
const char* ssid = "NAMA_WIFI";
const char* wifiPassword = "PASSWORD_WIFI";

const char* mqttServer = "b8a4f4a32b0d46ba9356920ec2825cca.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqttUser = "MQTT_USERNAME";
const char* mqttPassword = "MQTT_PASSWORD";

const char* topicPumpCommand = "nidec/command/speed";
const char* topicPumpStatus = "nidec/status/speed";
const char* topicSolenoidCommand = "nidec/command/solenoid";
const char* topicSolenoidStatus = "nidec/status/solenoid";
const char* topicPumpTimerStatus = "nidec/status/timer/pump";
const char* topicSolenoidTimerStatus = "nidec/status/timer/solenoid";
const char* topicOnline = "nidec/status";

const uint8_t PUMP_PIN = D5;
const uint8_t SOLENOID_PIN = D6;
const int PWM_FREQ = 1000;
const int PWM_RANGE = 1023;

// false jika relay solenoid aktif-LOW.
const bool SOLENOID_ACTIVE_HIGH = true;

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastTimerPublish = 0;
const unsigned long MQTT_RETRY_MS = 5000;
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long TIMER_PUBLISH_MS = 1000;

int currentSpeed = 0;
bool pumpTimerActive = false;
unsigned long pumpDeadline = 0;

bool solenoidOn = false;
bool solenoidTimerActive = false;
unsigned long solenoidDeadline = 0;

void setPumpPWM(int speed) {
  speed = constrain(speed, 0, 100);
  currentSpeed = speed;
  int pwmValue = map(speed, 0, 100, 0, PWM_RANGE);
  analogWrite(PUMP_PIN, pwmValue);
  Serial.printf("Pump: %d%%, PWM: %d\n", speed, pwmValue);
}

void setSolenoid(bool on) {
  solenoidOn = on;
  const bool pinHigh = on ? SOLENOID_ACTIVE_HIGH : !SOLENOID_ACTIVE_HIGH;
  digitalWrite(SOLENOID_PIN, pinHigh ? HIGH : LOW);
  Serial.printf("Solenoid: %s\n", on ? "ON" : "OFF");
}

void publishText(const char* topic, const char* value) {
  if (mqtt.connected()) mqtt.publish(topic, value, true);
}

unsigned long remainingSeconds(bool active, unsigned long deadline) {
  if (!active) return 0;
  const long remaining = (long)(deadline - millis());
  if (remaining <= 0) return 0;
  return ((unsigned long)remaining + 999UL) / 1000UL;
}

void publishPumpStatus() {
  char value[8];
  snprintf(value, sizeof(value), "%d", currentSpeed);
  publishText(topicPumpStatus, value);
}

void publishSolenoidStatus() {
  publishText(topicSolenoidStatus, solenoidOn ? "ON" : "OFF");
}

void publishTimerStatus() {
  char value[20];
  snprintf(value, sizeof(value), "%lu",
           remainingSeconds(pumpTimerActive, pumpDeadline));
  publishText(topicPumpTimerStatus, value);
  snprintf(value, sizeof(value), "%lu",
           remainingSeconds(solenoidTimerActive, solenoidDeadline));
  publishText(topicSolenoidTimerStatus, value);
}

void publishAllStatus() {
  publishPumpStatus();
  publishSolenoidStatus();
  publishTimerStatus();
}

void stopPumpTimer() {
  pumpTimerActive = false;
  pumpDeadline = 0;
}

void pumpManual(int speed) {
  stopPumpTimer();
  setPumpPWM(speed);
  publishPumpStatus();
  publishTimerStatus();
}

void pumpTimed(int speed, unsigned long seconds) {
  if (seconds == 0 || seconds > 86400UL) return;
  setPumpPWM(speed);
  if (speed == 0) {
    stopPumpTimer();
  } else {
    pumpDeadline = millis() + seconds * 1000UL;
    pumpTimerActive = true;
  }
  publishPumpStatus();
  publishTimerStatus();
}

void stopSolenoidTimer() {
  solenoidTimerActive = false;
  solenoidDeadline = 0;
}

void solenoidManual(bool on) {
  stopSolenoidTimer();
  setSolenoid(on);
  publishSolenoidStatus();
  publishTimerStatus();
}

void solenoidTimed(unsigned long seconds) {
  if (seconds == 0 || seconds > 86400UL) return;
  setSolenoid(true);
  solenoidDeadline = millis() + seconds * 1000UL;
  solenoidTimerActive = true;
  publishSolenoidStatus();
  publishTimerStatus();
}

bool parseUnsignedLong(const char* s, unsigned long &result) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p) {
    if (!isdigit((unsigned char)*p)) return false;
  }
  char* endPtr = nullptr;
  const unsigned long value = strtoul(s, &endPtr, 10);
  if (!endPtr || *endPtr != '\0') return false;
  result = value;
  return true;
}

// Pump payload: "0".."100", "OFF", or "T:<speed>:<seconds>".
// Solenoid payload: "ON", "OFF", or "T:<seconds>".
void handlePumpCommand(char* msg) {
  if (strcmp(msg, "CANCEL_TIMER") == 0) {
    stopPumpTimer();
    publishTimerStatus();
    return;
  }
  if (strcmp(msg, "OFF") == 0) {
    pumpManual(0);
    return;
  }

  if (msg[0] == 'T' && msg[1] == ':') {
    char* speedText = msg + 2;
    char* separator = strchr(speedText, ':');
    if (!separator) return;
    *separator = '\0';

    unsigned long speed = 0, seconds = 0;
    if (!parseUnsignedLong(speedText, speed) ||
        !parseUnsignedLong(separator + 1, seconds)) return;
    if (speed < 1 || speed > 100 || seconds == 0 || seconds > 86400UL) return;
    pumpTimed((int)speed, seconds);
    return;
  }

  unsigned long speed = 0;
  if (parseUnsignedLong(msg, speed) && speed <= 100) {
    pumpManual((int)speed);
  }
}

void handleSolenoidCommand(char* msg) {
  if (strcmp(msg, "CANCEL_TIMER") == 0) {
    stopSolenoidTimer();
    publishTimerStatus();
    return;
  }
  if (strcmp(msg, "ON") == 0) {
    solenoidManual(true);
    return;
  }
  if (strcmp(msg, "OFF") == 0) {
    solenoidManual(false);
    return;
  }
  if (msg[0] == 'T' && msg[1] == ':') {
    unsigned long seconds = 0;
    if (!parseUnsignedLong(msg + 2, seconds)) return;
    if (seconds == 0 || seconds > 86400UL) return;
    solenoidTimed(seconds);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length == 0 || length >= 48) return;
  char message[48];
  memcpy(message, payload, length);
  message[length] = '\0';
  Serial.printf("MQTT [%s] %s\n", topic, message);

  if (strcmp(topic, topicPumpCommand) == 0) {
    handlePumpCommand(message);
  } else if (strcmp(topic, topicSolenoidCommand) == 0) {
    handleSolenoidCommand(message);
  }
}

void serviceTimers() {
  const unsigned long now = millis();

  if (pumpTimerActive && (long)(now - pumpDeadline) >= 0) {
    stopPumpTimer();
    setPumpPWM(0);
    publishPumpStatus();
    publishTimerStatus();
    Serial.println("Pump timer expired: OFF");
  }

  if (solenoidTimerActive && (long)(now - solenoidDeadline) >= 0) {
    stopSolenoidTimer();
    setSolenoid(false);
    publishSolenoidStatus();
    publishTimerStatus();
    Serial.println("Solenoid timer expired: OFF");
  }

  if (now - lastTimerPublish >= TIMER_PUBLISH_MS) {
    lastTimerPublish = now;
    publishTimerStatus();
  }
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  const unsigned long now = millis();
  if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = now;
    Serial.println("Reconnecting WiFi...");
    WiFi.begin(ssid, wifiPassword);
  }
}

void serviceMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  const unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_MS) return;
  lastMqttAttempt = now;

  String clientID = "NIDEC-ESP8266-";
  clientID += String(ESP.getChipId(), HEX);

  Serial.println("Connecting to HiveMQ...");
  if (mqtt.connect(clientID.c_str(), mqttUser, mqttPassword)) {
    Serial.println("MQTT connected");
    mqtt.subscribe(topicPumpCommand);
    mqtt.subscribe(topicSolenoidCommand);
    publishText(topicOnline, "ONLINE");
    publishAllStatus();
  } else {
    Serial.printf("MQTT failed, state=%d\n", mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(SOLENOID_PIN, OUTPUT);
  analogWriteRange(PWM_RANGE);
  analogWriteFreq(PWM_FREQ);

  // Safe initial outputs. A hardware pull-down/pull-up and fail-safe
  // relay/driver are still needed during reset and before setup().
  analogWrite(PUMP_PIN, 0);
  digitalWrite(SOLENOID_PIN, SOLENOID_ACTIVE_HIGH ? LOW : HIGH);
  currentSpeed = 0;
  solenoidOn = false;
  pumpTimerActive = false;
  solenoidTimerActive = false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, wifiPassword);

  // TLS encryption is enabled, but server certificate validation is not.
  // For production, replace setInsecure() with the correct HiveMQ CA certificate.
  espClient.setInsecure();

  mqtt.setServer(mqttServer, mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(256);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);

  Serial.println("NIDEC controller ready; outputs OFF.");
}

void loop() {
  // Local timers run independently of the web page and MQTT availability.
  serviceTimers();
  serviceWiFi();
  serviceMQTT();
  if (mqtt.connected()) mqtt.loop();
}
