/*
  Traffic Signal Backend
  ----------------------
  Receives status updates POSTed by the ESP32 (running in Wokwi) over
  plain HTTP, keeps the latest snapshot + a rolling history in memory,
  and pushes every update to connected dashboards in real time over
  WebSocket. Also serves the dashboard's static files so the whole
  thing can be deployed as one service if you want.

  Run locally:
    npm install
    npm start
    -> listens on http://localhost:3000

  Deploy (so the simulated ESP32 in Wokwi can actually reach it):
    - Render.com / Railway.app / Fly.io: point them at this folder,
      start command "npm start". They give you a public https:// URL.
    - Quick demo without deploying: run locally + `ngrok http 3000`,
      then use the ngrok https URL as BACKEND_URL in sketch.ino.
    Wokwi's simulated network can reach the public internet but NOT
    "localhost" on your machine directly - that's why a tunnel or a
    real deployment is required, not optional.
*/

const express = require("express");
const cors = require("cors");
const http = require("http");
const path = require("path");
const { WebSocketServer } = require("ws");

const PORT = process.env.PORT || 3000;
const MAX_HISTORY = 200;

const app = express();
app.use(cors());
app.use(express.json({ limit: "64kb" }));

// Serve the dashboard (index.html / style.css / script.js) from the
// same server, so you can just open http://localhost:3000 in a browser.
app.use(express.static(path.join(__dirname, "..", "dashboard")));

let latestStatus = null;
const history = [];

// ---------------------------------------------------------------------
// POST /api/status  - called by the ESP32 every few seconds
// ---------------------------------------------------------------------
app.post("/api/status", (req, res) => {
  const body = req.body;

  if (!body || !Array.isArray(body.roads)) {
    return res.status(400).json({ error: "expected { roads: [...], currentGreen, phase }" });
  }

  const entry = {
    receivedAt: new Date().toISOString(),
    roads: body.roads,
    currentGreen: body.currentGreen ?? null,
    phase: body.phase ?? null,
  };

  latestStatus = entry;
  history.push(entry);
  if (history.length > MAX_HISTORY) history.shift();

  broadcast(entry);

  res.json({ ok: true });
});

// GET /api/status - latest snapshot (dashboard can also just poll this)
app.get("/api/status", (req, res) => {
  res.json(latestStatus || { message: "no data received yet" });
});

// GET /api/history - rolling history for charts / tables
app.get("/api/history", (req, res) => {
  res.json(history);
});

app.get("/api/health", (req, res) => {
  res.json({ ok: true, hasData: latestStatus !== null, historyLength: history.length });
});

// ---------------------------------------------------------------------
// WebSocket - push new status to every connected dashboard instantly
// ---------------------------------------------------------------------
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: "/ws" });

function broadcast(entry) {
  const payload = JSON.stringify({ type: "status", data: entry });
  wss.clients.forEach((client) => {
    if (client.readyState === 1 /* OPEN */) {
      client.send(payload);
    }
  });
}

wss.on("connection", (ws) => {
  // send the current snapshot immediately so a new dashboard tab
  // doesn't have to wait for the next ESP32 publish
  if (latestStatus) {
    ws.send(JSON.stringify({ type: "status", data: latestStatus }));
  }
});

server.listen(PORT, () => {
  console.log(`Traffic signal backend listening on port ${PORT}`);
  console.log(`  POST http://localhost:${PORT}/api/status`);
  console.log(`  GET  http://localhost:${PORT}/api/status`);
  console.log(`  GET  http://localhost:${PORT}/api/history`);
  console.log(`  WS   ws://localhost:${PORT}/ws`);
});
