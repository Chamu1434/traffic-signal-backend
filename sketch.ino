/*
  ===========================================================================
  IoT-Based Intelligent Adaptive Traffic Signal System
  Wokwi Simulation - ESP32

  Core algorithm (vehicle counting, waiting-time tracking, priority-based
  adaptive signal control, max green time, starvation prevention, sensor
  health monitoring) runs 100% locally and keeps working even if WiFi/MQTT
  is unavailable. Cloud publishing is an optional add-on (see ENABLE_CLOUD).
  ===========================================================================
*/

#include <WiFi.h>

// ---------------------------------------------------------------------
// 0. CLOUD FEATURE TOGGLES
//    ENABLE_CLOUD        - master switch for WiFi. Set 0 to run the pure
//                           local simulation first (recommended for your
//                           first test), then turn WiFi on.
//    ENABLE_HTTP_BACKEND - POST status as JSON to your own backend
//                           (backend/server.js). This is the recommended
//                           path since you control the server.
//    ENABLE_MQTT         - alternative: publish to a public MQTT broker
//                           instead (no backend to deploy, but the data
//                           is visible to anyone on that broker/topic).
//    You can enable both at once if you want to compare them; each is
//    independent and neither one can block the local traffic algorithm.
// ---------------------------------------------------------------------
#define ENABLE_CLOUD        1
#define ENABLE_HTTP_BACKEND 1
#define ENABLE_MQTT         0

#if ENABLE_CLOUD
  #include <ArduinoJson.h>
  #if ENABLE_HTTP_BACKEND
    #include <HTTPClient.h>
  #endif
  #if ENABLE_MQTT
    #include <PubSubClient.h>
  #endif
#endif

// ---------------------------------------------------------------------
// 1. PIN CONFIGURATION  (must match diagram.json exactly)
// ---------------------------------------------------------------------
#define ROADS 4

const int RED_PIN[ROADS]      = { 2, 18, 22, 13 };
const int YELLOW_PIN[ROADS]   = { 4, 19, 23, 12 };
const int GREEN_PIN[ROADS]    = { 5, 21, 25, 14 };
const int SENSOR_PIN[ROADS]   = { 34, 32, 26, 36 };  // vehicle-detect button
const int FAULT_PIN[ROADS]    = { 35, 33, 27, 39 };  // fault-toggle button

const char* ROAD_NAME[ROADS] = { "Road 1", "Road 2", "Road 3", "Road 4" };

// ---------------------------------------------------------------------
// 2. ALGORITHM CONFIGURATION (tune freely)
// ---------------------------------------------------------------------
const float DENSITY_WEIGHT              = 1.0;   // weight per vehicle
const float WAITING_WEIGHT              = 0.5;   // weight per second waited
const unsigned long MIN_GREEN_TIME_MS   = 5000;  // don't switch too fast
const unsigned long MAX_GREEN_TIME_MS   = 20000; // hard cap per spec
const unsigned long YELLOW_TIME_MS      = 3000;
const unsigned long DASHBOARD_PERIOD_MS = 1000;  // serial print / tick period
const unsigned long CLOUD_PERIOD_MS     = 5000;
const unsigned long DEBOUNCE_MS         = 200;
const int VEHICLES_SERVED_PER_GREEN     = 4;      // "cars that pass" per green phase

// ---------------------------------------------------------------------
// 3. STATE
// ---------------------------------------------------------------------
int  vehicleCount[ROADS]   = { 0, 0, 0, 0 };
unsigned long waitingTime[ROADS] = { 0, 0, 0, 0 }; // seconds
bool sensorFault[ROADS]    = { false, false, false, false };

enum Phase { PHASE_GREEN, PHASE_YELLOW };
Phase phase = PHASE_GREEN;
int currentRoad = 0;
unsigned long phaseStartTime = 0;

unsigned long lastTickTime = 0;
unsigned long lastCloudTime = 0;

bool lastSensorState[ROADS] = { false, false, false, false };
bool lastFaultBtnState[ROADS] = { false, false, false, false };
unsigned long lastSensorDebounce[ROADS] = { 0, 0, 0, 0 };
unsigned long lastFaultDebounce[ROADS]  = { 0, 0, 0, 0 };

#if ENABLE_CLOUD
const char* WIFI_SSID   = "Wokwi-GUEST";  // Wokwi's built-in open network
const char* WIFI_PASS   = "";
bool wifiEverConnected  = false;

#if ENABLE_HTTP_BACKEND
  // *** CHANGE THIS to your deployed backend URL (see backend/README.md) ***
  // e.g. "https://your-app.onrender.com/api/status"
  // or, for a quick local demo via ngrok: "https://xxxx.ngrok-free.app/api/status"
  const char* BACKEND_URL = "https://YOUR-BACKEND-URL.example.com/api/status";
#endif

#if ENABLE_MQTT
  WiFiClient espClient;
  PubSubClient mqttClient(espClient);
  const char* MQTT_BROKER = "test.mosquitto.org";
  const int   MQTT_PORT   = 1883;
  const char* MQTT_TOPIC  = "wokwi/adaptive-traffic-demo/status";
#endif
#endif

// ---------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  for (int i = 0; i < ROADS; i++) {
    pinMode(RED_PIN[i], OUTPUT);
    pinMode(YELLOW_PIN[i], OUTPUT);
    pinMode(GREEN_PIN[i], OUTPUT);
    pinMode(SENSOR_PIN[i], INPUT);   // external pulldown in diagram.json
    pinMode(FAULT_PIN[i], INPUT);    // external pulldown in diagram.json
  }

  // Start with Road 1 GREEN, everyone else RED
  currentRoad = 0;
  phase = PHASE_GREEN;
  phaseStartTime = millis();
  applySignals();

  Serial.println();
  Serial.println("=======================================================");
  Serial.println(" INTELLIGENT ADAPTIVE TRAFFIC SIGNAL SYSTEM - BOOT OK");
  Serial.println("=======================================================");
  Serial.println("Press a ROAD SENSOR button in Wokwi to simulate a vehicle");
  Serial.println("crossing the IR beam (increments vehicle count).");
  Serial.println("Press a ROAD FAULT button to toggle that sensor's health.");
  Serial.println("=======================================================");

#if ENABLE_CLOUD
  setupWiFi();
  #if ENABLE_MQTT
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  #endif
#endif
}

// ---------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  readSensorButtons(now);
  readFaultButtons(now);

  runSignalStateMachine(now);

  if (now - lastTickTime >= DASHBOARD_PERIOD_MS) {
    lastTickTime = now;
    tickWaitingTimes();
    printDashboard();
  }

#if ENABLE_CLOUD
  maintainCloudConnection();
  if (now - lastCloudTime >= CLOUD_PERIOD_MS) {
    lastCloudTime = now;
    publishStatus();
  }
#endif
}

// ---------------------------------------------------------------------
// 4. VEHICLE DETECTION  (button press = one vehicle crossing the beam)
// ---------------------------------------------------------------------
void readSensorButtons(unsigned long now) {
  for (int i = 0; i < ROADS; i++) {
    bool pressed = digitalRead(SENSOR_PIN[i]) == HIGH;

    if (pressed && !lastSensorState[i] && (now - lastSensorDebounce[i] > DEBOUNCE_MS)) {
      lastSensorDebounce[i] = now;
      if (!sensorFault[i]) {
        vehicleCount[i]++;
      }
      // If the sensor is faulted, presses are ignored - a real stuck IR
      // sensor would not register new vehicles either.
    }
    lastSensorState[i] = pressed;
  }
}

// ---------------------------------------------------------------------
// 5. SENSOR HEALTH  (button toggles fault state on/off)
// ---------------------------------------------------------------------
void readFaultButtons(unsigned long now) {
  for (int i = 0; i < ROADS; i++) {
    bool pressed = digitalRead(FAULT_PIN[i]) == HIGH;

    if (pressed && !lastFaultBtnState[i] && (now - lastFaultDebounce[i] > DEBOUNCE_MS)) {
      lastFaultDebounce[i] = now;
      sensorFault[i] = !sensorFault[i];
      Serial.print(">>> ");
      Serial.print(ROAD_NAME[i]);
      Serial.println(sensorFault[i] ? " SENSOR FAULT INJECTED" : " sensor restored to NORMAL");
    }
    lastFaultBtnState[i] = pressed;
  }
}

// ---------------------------------------------------------------------
// 6. WAITING TIME  (every road not currently GREEN accumulates wait)
// ---------------------------------------------------------------------
void tickWaitingTimes() {
  for (int i = 0; i < ROADS; i++) {
    bool isServed = (i == currentRoad && phase == PHASE_GREEN);
    if (!isServed) {
      waitingTime[i]++;
    }
  }
}

// ---------------------------------------------------------------------
// 7. PRIORITY FORMULA
// ---------------------------------------------------------------------
float priorityOf(int i) {
  return (DENSITY_WEIGHT * vehicleCount[i]) + (WAITING_WEIGHT * waitingTime[i]);
}

int pickNextRoad(int excludeRoad) {
  int best = -1;
  float bestScore = -1;
  for (int i = 0; i < ROADS; i++) {
    if (i == excludeRoad) continue; // never repeat the same road immediately
    float score = priorityOf(i);
    if (score > bestScore) {
      bestScore = score;
      best = i;
    }
  }
  if (best == -1) best = (excludeRoad + 1) % ROADS; // fallback, all zero
  return best;
}

// ---------------------------------------------------------------------
// 8/9/10. STATE MACHINE: GREEN -> YELLOW -> next road GREEN
//          Enforces max green time and guarantees no two roads are
//          GREEN/YELLOW at the same time.
// ---------------------------------------------------------------------
void runSignalStateMachine(unsigned long now) {
  unsigned long elapsed = now - phaseStartTime;

  if (phase == PHASE_GREEN) {
    bool hitMax   = elapsed >= MAX_GREEN_TIME_MS;
    bool minMet   = elapsed >= MIN_GREEN_TIME_MS;
    bool noCars   = vehicleCount[currentRoad] == 0;
    bool starved  = someoneElseUrgentlyWaiting();

    if (hitMax || (minMet && (noCars || starved))) {
      phase = PHASE_YELLOW;
      phaseStartTime = now;
      applySignals();
    }
  } else { // PHASE_YELLOW
    if (elapsed >= YELLOW_TIME_MS) {
      // Serve the road: some vehicles pass, waiting resets
      int served = min(vehicleCount[currentRoad], VEHICLES_SERVED_PER_GREEN);
      vehicleCount[currentRoad] -= served;
      waitingTime[currentRoad] = 0;

      int next = pickNextRoad(currentRoad);
      currentRoad = next;
      phase = PHASE_GREEN;
      phaseStartTime = now;
      applySignals();
    }
  }
}

// Fairness / starvation guard: if some other road's priority has grown
// well past the current road's, cut the current green short (still
// respecting MIN_GREEN_TIME_MS above).
bool someoneElseUrgentlyWaiting() {
  float currentScore = priorityOf(currentRoad);
  for (int i = 0; i < ROADS; i++) {
    if (i == currentRoad) continue;
    if (priorityOf(i) > currentScore + 5.0) return true; // margin avoids flapping
  }
  return false;
}

// ---------------------------------------------------------------------
// APPLY LEDS  (never allow conflicting greens)
// ---------------------------------------------------------------------
void applySignals() {
  for (int i = 0; i < ROADS; i++) {
    bool isCurrent = (i == currentRoad);
    digitalWrite(RED_PIN[i],    isCurrent ? LOW  : HIGH);
    digitalWrite(YELLOW_PIN[i], (isCurrent && phase == PHASE_YELLOW) ? HIGH : LOW);
    digitalWrite(GREEN_PIN[i],  (isCurrent && phase == PHASE_GREEN)  ? HIGH : LOW);
  }
}

// ---------------------------------------------------------------------
// 11/12. SERIAL DASHBOARD
// ---------------------------------------------------------------------
void printDashboard() {
  unsigned long greenElapsed = (millis() - phaseStartTime) / 1000;

  Serial.println();
  Serial.println("========================================");
  Serial.println("       INTELLIGENT TRAFFIC SYSTEM");
  Serial.println("========================================");

  for (int i = 0; i < ROADS; i++) {
    Serial.println(ROAD_NAME[i]);
    Serial.print("Vehicles   : "); Serial.println(vehicleCount[i]);
    Serial.print("Waiting    : "); Serial.print(waitingTime[i]); Serial.println(" sec");
    Serial.print("Priority   : "); Serial.println(priorityOf(i), 1);
    Serial.print("Signal     : "); Serial.println(signalTextFor(i));
    Serial.print("Sensor     : "); Serial.println(sensorFault[i] ? "FAULT" : "NORMAL");
    Serial.println();
  }

  Serial.print("Current Green Road: ");
  Serial.println(String(ROAD_NAME[currentRoad]) + (phase == PHASE_YELLOW ? " (YELLOW/clearing)" : ""));
  Serial.print("Green Time: "); Serial.print(greenElapsed); Serial.println(" sec");
  Serial.println("========================================");
}

String signalTextFor(int i) {
  if (i != currentRoad) return "RED";
  return (phase == PHASE_GREEN) ? "GREEN" : "YELLOW";
}

// ---------------------------------------------------------------------
// 15. CLOUD (optional) - WiFi + HTTP backend and/or MQTT broker
//     This block never blocks the main loop and never stops the local
//     traffic control algorithm if it fails or if WiFi is unavailable.
// ---------------------------------------------------------------------
#if ENABLE_CLOUD
void setupWiFi() {
  Serial.print("Connecting to WiFi (");
  Serial.print(WIFI_SSID);
  Serial.println(")...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void maintainCloudConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    return; // don't block; local algorithm keeps running regardless
  }
  if (!wifiEverConnected) {
    wifiEverConnected = true;
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
#if ENABLE_MQTT
  if (!mqttClient.connected()) {
    String clientId = "esp32-traffic-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    mqttClient.connect(clientId.c_str());
  }
  mqttClient.loop();
#endif
}

// Builds the shared JSON status payload used by both HTTP and MQTT.
size_t buildStatusJson(char* buffer, size_t bufferSize) {
  StaticJsonDocument<512> doc;
  JsonArray roads = doc.createNestedArray("roads");
  for (int i = 0; i < ROADS; i++) {
    JsonObject r = roads.createNestedObject();
    r["name"]     = ROAD_NAME[i];
    r["vehicles"] = vehicleCount[i];
    r["waiting"]  = waitingTime[i];
    r["priority"] = priorityOf(i);
    r["signal"]   = signalTextFor(i);
    r["sensor"]   = sensorFault[i] ? "FAULT" : "NORMAL";
  }
  doc["currentGreen"] = ROAD_NAME[currentRoad];
  doc["phase"]        = (phase == PHASE_GREEN) ? "GREEN" : "YELLOW";

  return serializeJson(doc, buffer, bufferSize);
}

void publishStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  char buffer[512];
  size_t n = buildStatusJson(buffer, sizeof(buffer));

#if ENABLE_HTTP_BACKEND
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)buffer, n);
  if (code <= 0) {
    Serial.print("Backend POST failed: ");
    Serial.println(http.errorToString(code));
  }
  http.end();
#endif

#if ENABLE_MQTT
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_TOPIC, buffer, n);
  }
#endif
}
#endif
