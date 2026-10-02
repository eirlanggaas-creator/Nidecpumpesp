const express = require("express");
const mqtt = require("mqtt");

const app = express();
app.use(express.json());

// Izinkan dashboard GitHub Pages mengakses API
app.use((req, res, next) => {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow-Headers", "Content-Type");
  res.header("Access-Control-Allow-Methods", "GET,POST,OPTIONS");

  if (req.method === "OPTIONS") {
    return res.sendStatus(204);
  }

  next();
});

const PORT = process.env.PORT || 3000;

const MQTT_HOST = process.env.MQTT_HOST;
const MQTT_USERNAME = process.env.MQTT_USERNAME;
const MQTT_PASSWORD = process.env.MQTT_PASSWORD;

const COMMAND_TOPIC = "nidec/command/speed";
const STATUS_TOPIC = "nidec/status/speed";

let currentSpeed = 0;
let mqttConnected = false;

const mqttClient = mqtt.connect(`mqtts://${MQTT_HOST}:8883`, {
  username: MQTT_USERNAME,
  password: MQTT_PASSWORD
});

mqttClient.on("connect", () => {
  console.log("Connected to HiveMQ");

  mqttConnected = true;

  mqttClient.subscribe(STATUS_TOPIC, (err) => {
    if (err) {
      console.error("Subscribe error:", err.message);
    } else {
      console.log("Subscribed:", STATUS_TOPIC);
    }
  });
});

mqttClient.on("message", (topic, message) => {
  if (topic === STATUS_TOPIC) {
    const value = Number(message.toString());

    if (Number.isFinite(value)) {
      currentSpeed = Math.max(0, Math.min(100, value));
      console.log("Speed feedback:", currentSpeed + "%");
    }
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

// Test backend
app.get("/", (req, res) => {
  res.json({
    status: "NIDEC backend online"
  });
});

// Kirim perintah speed ke ESP8266
app.post("/api/speed", (req, res) => {
  let speed = Number(req.body.speed);

  if (!Number.isFinite(speed)) {
    return res.status(400).json({
      error: "Invalid speed"
    });
  }

  speed = Math.max(0, Math.min(100, Math.round(speed)));

  if (!mqttClient.connected) {
    return res.status(503).json({
      error: "MQTT not connected"
    });
  }

  mqttClient.publish(
    COMMAND_TOPIC,
    String(speed),
    { qos: 0 },
    (err) => {
      if (err) {
        return res.status(500).json({
          error: "MQTT publish failed"
        });
      }

      res.json({
        success: true,
        speed: speed
      });
    }
  );
});

// Ambil status speed dari ESP8266
app.get("/api/status", (req, res) => {
  res.json({
    mqtt: mqttConnected,
    speed: currentSpeed
  });
});

app.listen(PORT, () => {
  console.log(`Server running on port ${PORT}`);
});
