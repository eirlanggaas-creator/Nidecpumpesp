const express = require("express");
const mqtt = require("mqtt");

const app = express();
app.use(express.json());

app.use((req, res, next) => {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow-Headers", "Content-Type");
  res.header("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  if (req.method === "OPTIONS") return res.sendStatus(204);
  next();
});

const PORT = process.env.PORT || 3000;
const MQTT_HOST = process.env.MQTT_HOST;
const MQTT_USERNAME = process.env.MQTT_USERNAME;
const MQTT_PASSWORD = process.env.MQTT_PASSWORD;

const TOPICS = {
  pumpCommand: "nidec/command/speed",
  pumpStatus: "nidec/status/speed",
  solenoidCommand: "nidec/command/solenoid",
  solenoidStatus: "nidec/status/solenoid",
  pumpTimer: "nidec/status/timer/pump",
  solenoidTimer: "nidec/status/timer/solenoid"
};

let currentSpeed = 0;
let currentSolenoid = false;
let pumpTimer = 0;
let solenoidTimer = 0;
let mqttConnected = false;

const mqttClient = mqtt.connect(`mqtts://${MQTT_HOST}:8883`, {
  username: MQTT_USERNAME,
  password: MQTT_PASSWORD,
  reconnectPeriod: 3000
});

mqttClient.on("connect", () => {
  console.log("Connected to HiveMQ");
  mqttConnected = true;
  mqttClient.subscribe([
    TOPICS.pumpStatus,
    TOPICS.solenoidStatus,
    TOPICS.pumpTimer,
    TOPICS.solenoidTimer
  ], (err) => {
    if (err) console.error("Subscribe error:", err.message);
    else console.log("Subscribed to NIDEC status topics");
  });
});

mqttClient.on("message", (topic, message) => {
  const value = message.toString();
  if (topic === TOPICS.pumpStatus) {
    const number = Number(value);
    if (Number.isFinite(number)) currentSpeed = Math.max(0, Math.min(100, number));
  } else if (topic === TOPICS.solenoidStatus) {
    if (value === "ON") currentSolenoid = true;
    if (value === "OFF") currentSolenoid = false;
  } else if (topic === TOPICS.pumpTimer) {
    const number = Number(value);
    if (Number.isFinite(number)) pumpTimer = Math.max(0, Math.min(86400, Math.floor(number)));
  } else if (topic === TOPICS.solenoidTimer) {
    const number = Number(value);
    if (Number.isFinite(number)) solenoidTimer = Math.max(0, Math.min(86400, Math.floor(number)));
  }
});

mqttClient.on("close", () => {
  mqttConnected = false;
  console.log("MQTT disconnected");
});
mqttClient.on("error", (err) => {
  mqttConnected = false;
  console.error("MQTT error:", err.message);
});

function publish(topic, payload, res) {
  if (!mqttClient.connected) return res.status(503).json({ error: "MQTT not connected" });
  mqttClient.publish(topic, String(payload), { qos: 0, retain: false }, (err) => {
    if (err) return res.status(500).json({ error: "MQTT publish failed" });
    return res.json({ success: true });
  });
}

app.get("/", (req, res) => res.json({ status: "NIDEC backend online", mqtt: mqttConnected }));

app.post("/api/speed", (req, res) => {
  const value = Number(req.body.speed);
  if (!Number.isFinite(value)) return res.status(400).json({ error: "Invalid speed" });
  const speed = Math.max(0, Math.min(100, Math.round(value)));
  if (!mqttClient.connected) return res.status(503).json({ error: "MQTT not connected" });
  mqttClient.publish(TOPICS.pumpCommand, String(speed), { qos: 0, retain: false }, (err) => {
    if (err) return res.status(500).json({ error: "MQTT publish failed" });
    res.json({ success: true, speed });
  });
});

app.post("/api/pump/timer", (req, res) => {
  const speed = Number(req.body.speed);
  const seconds = Number(req.body.seconds);
  if (!Number.isInteger(speed) || speed < 1 || speed > 100 ||
      !Number.isInteger(seconds) || seconds < 1 || seconds > 86400) {
    return res.status(400).json({ error: "Speed must be 1-100 and seconds 1-86400" });
  }
  return publish(TOPICS.pumpCommand, `T:${speed}:${seconds}`, res);
});

app.post("/api/pump/timer/cancel", (req, res) => {
  return publish(TOPICS.pumpCommand, String(currentSpeed), res);
});

app.post("/api/solenoid", (req, res) => {
  if (typeof req.body.on !== "boolean") {
    return res.status(400).json({ error: "Field 'on' must be true or false" });
  }
  return publish(TOPICS.solenoidCommand, req.body.on ? "ON" : "OFF", res);
});

app.post("/api/solenoid/timer", (req, res) => {
  const seconds = Number(req.body.seconds);
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 86400) {
    return res.status(400).json({ error: "Seconds must be 1-86400" });
  }
  return publish(TOPICS.solenoidCommand, `T:${seconds}`, res);
});

app.post("/api/solenoid/timer/cancel", (req, res) => {
  return publish(TOPICS.solenoidCommand, currentSolenoid ? "ON" : "OFF", res);
});

app.get("/api/status", (req, res) => {
  res.set("Cache-Control", "no-store");
  res.json({ mqtt: mqttConnected, speed: currentSpeed, solenoid: currentSolenoid, pumpTimer, solenoidTimer });
});

app.listen(PORT, () => console.log(`Server running on port ${PORT}`));
