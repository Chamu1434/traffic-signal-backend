// This file runs in your REAL browser - completely separate from the
// Wokwi simulation. It talks to YOUR backend (backend/server.js), which
// the ESP32 (running in Wokwi) posts status updates to over HTTP.
//
// If you're opening this page as a static file served BY the backend
// itself (http://localhost:3000 or your deployed URL), leave BACKEND_URL
// as "" - it will automatically use the page's own origin.
// If you're opening index.html separately (e.g. double-clicking the
// file), set BACKEND_URL to your backend's full origin, e.g.
// "https://your-app.onrender.com".
const BACKEND_URL = "";
const MAX_HISTORY_ROWS = 20;

const statusEl = document.getElementById("connectionStatus");
const roadGrid = document.getElementById("roadGrid");
const currentGreenEl = document.getElementById("currentGreen");
const currentPhaseEl = document.getElementById("currentPhase");
const lastUpdateEl = document.getElementById("lastUpdate");
const historyBody = document.querySelector("#historyTable tbody");

function origin() {
  return BACKEND_URL || window.location.origin;
}

function wsUrl() {
  const base = origin().replace(/^http/, "ws");
  return `${base}/ws`;
}

// Load whatever the backend already has, so the page isn't empty
// while waiting for the next ESP32 publish.
async function loadInitial() {
  try {
    const res = await fetch(`${origin()}/api/status`);
    const data = await res.json();
    if (data && data.roads) render(data);
  } catch (e) {
    console.warn("Could not load initial status", e);
  }
}

function connect() {
  const ws = new WebSocket(wsUrl());

  ws.onopen = () => {
    statusEl.textContent = "Connected to backend";
    statusEl.className = "status connected";
  };

  ws.onclose = () => {
    statusEl.textContent = "Disconnected - retrying...";
    statusEl.className = "status disconnected";
    setTimeout(connect, 2000);
  };

  ws.onerror = () => ws.close();

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === "status" && msg.data) {
        render(msg.data);
        addHistoryRow(msg.data);
      }
    } catch (e) {
      console.error("Bad message", e);
    }
  };
}

function render(data) {
  currentGreenEl.textContent = data.currentGreen ?? "-";
  currentPhaseEl.textContent = data.phase ?? "-";
  lastUpdateEl.textContent = new Date().toLocaleTimeString();

  roadGrid.innerHTML = "";
  (data.roads || []).forEach((r) => {
    const card = document.createElement("div");
    card.className = `road-card signal-${r.signal}`;
    card.innerHTML = `
      <h3>${r.name}</h3>
      <div class="row"><span>Vehicles</span><span>${r.vehicles}</span></div>
      <div class="row"><span>Waiting</span><span>${r.waiting} sec</span></div>
      <div class="row"><span>Priority</span><span>${Number(r.priority).toFixed(1)}</span></div>
      <div class="row"><span>Signal</span><span class="pill ${r.signal}">${r.signal}</span></div>
      <div class="row"><span>Sensor</span><span class="pill ${r.sensor}">${r.sensor}</span></div>
    `;
    roadGrid.appendChild(card);
  });
}

function addHistoryRow(data) {
  const roads = data.roads || [];
  const tr = document.createElement("tr");
  tr.innerHTML = `
    <td>${new Date().toLocaleTimeString()}</td>
    <td>${roads[0]?.vehicles ?? "-"}</td>
    <td>${roads[1]?.vehicles ?? "-"}</td>
    <td>${roads[2]?.vehicles ?? "-"}</td>
    <td>${roads[3]?.vehicles ?? "-"}</td>
    <td>${data.currentGreen ?? "-"}</td>
  `;
  historyBody.prepend(tr);
  while (historyBody.rows.length > MAX_HISTORY_ROWS) {
    historyBody.deleteRow(historyBody.rows.length - 1);
  }
}

loadInitial();
connect();
