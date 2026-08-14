# Traffic Signal Backend

A tiny Express + WebSocket server that:
- accepts `POST /api/status` from the ESP32 (running in Wokwi)
- keeps the latest snapshot + a rolling history in memory
- pushes every update live to connected browser dashboards over WebSocket
- also serves `dashboard/index.html` directly, so you can open one URL
  and see everything

## Run it locally

```
cd backend
npm install
npm start
```

Then open **http://localhost:3000** in your browser - that's your live
dashboard. `GET http://localhost:3000/api/status` returns the latest
snapshot as JSON; `GET /api/history` returns the last 200 updates.

## Why "localhost" alone isn't enough

Wokwi runs your ESP32 in a browser-based simulator. Its simulated WiFi
(`Wokwi-GUEST`) can reach the real public internet, but it **cannot**
reach `localhost` on your computer - there's no route between the
sandboxed simulator and your machine's loopback address. So for the
ESP32 to actually POST data to this backend, the backend needs a real,
public URL. Two ways to get one:

### Option A - quick demo: ngrok tunnel
1. `npm start` locally (backend now on `http://localhost:3000`)
2. In another terminal: `ngrok http 3000`
3. Copy the `https://xxxx.ngrok-free.app` URL ngrok gives you
4. In `sketch.ino`, set:
   ```cpp
   const char* BACKEND_URL = "https://xxxx.ngrok-free.app/api/status";
   ```
5. Re-run the Wokwi simulation

Free ngrok URLs change every time you restart the tunnel, so you'll
need to update `BACKEND_URL` and re-run the simulation each session.

### Option B - real deployment (stays up, stable URL)
Any Node-friendly host works. For example, on Render.com:
1. Push this `backend/` folder to a GitHub repo
2. Render -> New -> Web Service -> connect the repo
3. Build command: `npm install`   Start command: `npm start`
4. Render gives you a stable `https://your-app.onrender.com` URL
5. Set that as `BACKEND_URL` (with `/api/status` appended) in `sketch.ino`

Railway.app and Fly.io work the same way. Any of these give you a
permanent URL, so you only update `sketch.ino` once.

## Endpoints

| Method | Path           | Purpose                                   |
|--------|----------------|--------------------------------------------|
| POST   | /api/status    | ESP32 sends a status snapshot (JSON)       |
| GET    | /api/status    | Latest snapshot                            |
| GET    | /api/history   | Last 200 snapshots                         |
| GET    | /api/health    | Basic liveness check                       |
| WS     | /ws            | Live push of every new snapshot            |

## Note on data persistence

This backend keeps data **in memory only** - restarting it clears the
history. That's intentional for a student demo (zero database setup).
If you want it to survive restarts, swap the `history` array for a
small SQLite or JSON-file-backed store; the rest of the code doesn't
need to change.
