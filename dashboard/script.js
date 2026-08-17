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

  renderIntersection(data);

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

function renderIntersection(data) {
  (data.roads || []).forEach((r, i) => {
    const group = document.getElementById(`light-road-${i}`);
    if (!group) return;
    group.querySelectorAll(".bulb").forEach((bulb) => bulb.classList.remove("on"));
    const activeClass = (r.signal || "").toLowerCase();
    const activeBulb = group.querySelector(`.bulb.${activeClass}`);
    if (activeBulb) activeBulb.classList.add("on");

    const countEl = document.getElementById(`count-${i}`);
    if (countEl) {
      countEl.textContent = `${r.vehicles} cars, ${r.waiting}s wait`;
    }
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
