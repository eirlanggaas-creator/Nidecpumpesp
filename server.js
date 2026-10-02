const express = require("express");
const mqtt = require("mqtt");

const app = express();
app.use(express.json());

const PORT = process.env.PORT || 3000;

const mqttClient = mqtt.connect(
  `mqtts://${process.env.MQTT_HOST}:8883`,
  {
    username: process.env.MQTT_USERNAME,
    password: process.env.MQTT_PASSWORD
  }
);

mqttClient.on("connect", () => {
  console.log("Connected to HiveMQ");
});

mqttClient.on("error", (err) => {
  console.error("MQTT error:", err.message);
});

app.get("/", (req, res) => {
  res.json({
    status: "NIDEC backend online"
  });
});

app.post("/api/speed", (req, res) => {
  let speed = Number(req.body.speed);

  if (!Number.isFinite(speed)) {
    return res.status(400).json({ error: "Invalid speed" });
  }

  speed = Math.max(0, Math.min(100, Math.round(speed)));

  if (!mqttClient.connected) {
    return res.status(503).json({ error: "MQTT not connected" });
  }

  mqttClient.publish(
    "nidec/command/speed",
    String(speed),
    { qos: 0 },
    (err) => {
      if (err) {
        return res.status(500).json({ error: "MQTT publish failed" });
      }

      res.json({
        success: true,
        speed: speed
      });
    }
  );
});

app.listen(PORT, () => {
  console.log(`Server running on port ${PORT}`);
});