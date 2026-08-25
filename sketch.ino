/*
  ===========================================================================
  IoT-Based Intelligent Adaptive Traffic Signal System
  Wokwi Simulation - ESP32

  Core algorithm (vehicle counting, waiting-time tracking, priority-based
  adaptive signal control, max green time, starvation prevention) runs
  100% locally and keeps working even if WiFi/backend is unavailable.

  NEW MODULE: Sensor Fault Detection & Fallback (see SECTION 7A/7B/7C)
  - SensorHealthManager   : tracks NORMAL/FAULT per road (existing fault
                             buttons now feed this + write to the event log)
  - FallbackSignalController : fixed 60s-per-road cycling, used ONLY while
                             any sensor is faulty, so a faulty sensor can
                             never bias the adaptive priority algorithm
  The existing adaptive algorithm (SECTION 7) is untouched and behaves
  exactly as before whenever every sensor is NORMAL.
  ===========================================================================
*/

#include <WiFi.h>

// ---------------------------------------------------------------------
// 0. CLOUD FEATURE TOGGLES
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
const float DENSITY_WEIGHT              = 1.0;
const float WAITING_WEIGHT              = 0.5;
const unsigned long MIN_GREEN_TIME_MS   = 5000;
const unsigned long MAX_GREEN_TIME_MS   = 20000;
const unsigned long YELLOW_TIME_MS      = 3000;
const unsigned long DASHBOARD_PERIOD_MS = 1000;
const unsigned long CLOUD_PERIOD_MS     = 5000;
const unsigned long DEBOUNCE_MS         = 200;
const int VEHICLES_SERVED_PER_GREEN     = 4;

// ---------------------------------------------------------------------
// 2A. FALLBACK MODE CONFIGURATION (new)
// ---------------------------------------------------------------------
const unsigned long FALLBACK_GREEN_MS  = 60000; // exactly 60s per road, per spec
const unsigned long FALLBACK_ALLRED_MS = 2000;  // safety all-red clearance

// ---------------------------------------------------------------------
// 3. STATE
// ---------------------------------------------------------------------
int  vehicleCount[ROADS]   = { 0, 0, 0, 0 };
unsigned long waitingTime[ROADS] = { 0, 0, 0, 0 };
bool sensorFault[ROADS]    = { false, false, false, false }; // SensorHealthManager state

enum Phase { PHASE_GREEN, PHASE_YELLOW, PHASE_ALL_RED };
Phase phase = PHASE_GREEN;
int currentRoad = 0; // -1 during PHASE_ALL_RED (no road is green)
unsigned long phaseStartTime = 0;

// SystemMode: which controller is driving signal selection right now
enum SystemMode { MODE_ADAPTIVE, MODE_FALLBACK };
SystemMode systemMode = MODE_ADAPTIVE;
int fallbackRoadIndex = 0; // FallbackSignalController's Road1->Road2->Road3->Road4 pointer

unsigned long lastTickTime = 0;
unsigned long lastCloudTime = 0;

bool lastSensorState[ROADS] = { false, false, false, false };
bool lastFaultBtnState[ROADS] = { false, false, false, false };
unsigned long lastSensorDebounce[ROADS] = { 0, 0, 0, 0 };
unsigned long lastFaultDebounce[ROADS]  = { 0, 0, 0, 0 };

// ---------------------------------------------------------------------
// 3A. EVENT LOG (new) - small ring buffer, newest events pushed to backend
// ---------------------------------------------------------------------
#define LOG_SIZE 8
String eventLog[LOG_SIZE];
int logHead = 0; // next slot to write (oldest entry currently here)

void addEvent(const String& msg) {
  eventLog[logHead] = msg;
  logHead = (logHead + 1) % LOG_SIZE;
  Serial.print("[EVENT] ");
  Serial.println(msg);
}

#if ENABLE_CLOUD
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";
bool wifiEverConnected  = false;

#if ENABLE_HTTP_BACKEND
  const char* BACKEND_URL = "https://traffic-signal-backend.onrender.com/api/status";
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
    pinMode(SENSOR_PIN[i], INPUT);
    pinMode(FAULT_PIN[i], INPUT);
  }

  currentRoad = 0;
  phase = PHASE_GREEN;
  phaseStartTime = millis();
  applySignals();

  Serial.println();
  Serial.println("=======================================================");
  Serial.println(" INTELLIGENT ADAPTIVE TRAFFIC SIGNAL SYSTEM - BOOT OK");
  Serial.println("=======================================================");
  Serial.println("Blue button  = vehicle sensor (simulates a car crossing)");
  Serial.println("Red button   = toggles that road's sensor fault");
  Serial.println("=======================================================");
  addEvent("System started in ADAPTIVE_MODE.");

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
// 4. VEHICLE DETECTION
// ---------------------------------------------------------------------
void readSensorButtons(unsigned long now) {
  for (int i = 0; i < ROADS; i++) {
    bool pressed = digitalRead(SENSOR_PIN[i]) == HIGH;

    if (pressed && !lastSensorState[i] && (now - lastSensorDebounce[i] > DEBOUNCE_MS)) {
      lastSensorDebounce[i] = now;
      if (!sensorFault[i]) {
        vehicleCount[i]++;
      }
    }
    lastSensorState[i] = pressed;
  }
}

// ---------------------------------------------------------------------
// 5. SensorHealthManager - detects fault button presses, updates status,
//    and logs the transition. This is the ONLY place sensorFault[] changes.
// ---------------------------------------------------------------------
void readFaultButtons(unsigned long now) {
  for (int i = 0; i < ROADS; i++) {
    bool pressed = digitalRead(FAULT_PIN[i]) == HIGH;

    if (pressed && !lastFaultBtnState[i] && (now - lastFaultDebounce[i] > DEBOUNCE_MS)) {
      lastFaultDebounce[i] = now;
      sensorFault[i] = !sensorFault[i];
      if (sensorFault[i]) {
        addEvent(String(ROAD_NAME[i]) + " sensor fault detected.");
      } else {
        addEvent(String(ROAD_NAME[i]) + " sensor restored.");
      }
    }
    lastFaultBtnState[i] = pressed;
  }
}

bool anySensorFaulty() {
  for (int i = 0; i < ROADS; i++) {
    if (sensorFault[i]) return true;
  }
  return false;
}

// ---------------------------------------------------------------------
// 6. WAITING TIME
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
// 7. ADAPTIVE PRIORITY FORMULA (unchanged) - a faulty road is additionally
//    forced to the lowest possible priority as defense-in-depth, so even
//    a stale/inflated reading from a faulty sensor cannot win selection.
// ---------------------------------------------------------------------
float priorityOf(int i) {
  if (sensorFault[i]) return -1.0; // never let a faulty road win adaptively
  return (DENSITY_WEIGHT * vehicleCount[i]) + (WAITING_WEIGHT * waitingTime[i]);
}

int pickNextRoad(int excludeRoad) {
  int best = -1;
  float bestScore = -1e9;
  for (int i = 0; i < ROADS; i++) {
    if (i == excludeRoad) continue;
    float score = priorityOf(i);
    if (score > bestScore) {
      bestScore = score;
      best = i;
    }
  }
  if (best == -1) best = (excludeRoad + 1) % ROADS;
  return best;
}

bool someoneElseUrgentlyWaiting() {
  float currentScore = priorityOf(currentRoad);
  for (int i = 0; i < ROADS; i++) {
    if (i == currentRoad) continue;
    if (priorityOf(i) > currentScore + 5.0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------
// 7A/7B/7C. STATE MACHINE
//   ADAPTIVE_MODE : GREEN -> YELLOW -> next GREEN        (original, unchanged
//                    timing/behavior whenever no sensor is faulty)
//   FALLBACK_MODE : GREEN(60s) -> YELLOW -> ALL_RED -> next road's GREEN
//   Mode switches only happen at the safe boundary right after a road's
//   YELLOW (or ALL_RED) phase completes - never abruptly mid-green.
// ---------------------------------------------------------------------
void runSignalStateMachine(unsigned long now) {
  unsigned long elapsed = now - phaseStartTime;

  if (phase == PHASE_GREEN) {
    if (systemMode == MODE_ADAPTIVE) {
      bool hitMax  = elapsed >= MAX_GREEN_TIME_MS;
      bool minMet  = elapsed >= MIN_GREEN_TIME_MS;
      bool noCars  = vehicleCount[currentRoad] == 0;
      bool starved = someoneElseUrgentlyWaiting();

      if (hitMax || (minMet && (noCars || starved))) {
        phase = PHASE_YELLOW;
        phaseStartTime = now;
        applySignals();
      }
    } else { // MODE_FALLBACK: fixed 60s, no early exit - per spec
      if (elapsed >= FALLBACK_GREEN_MS) {
        phase = PHASE_YELLOW;
        phaseStartTime = now;
        applySignals();
      }
    }

  } else if (phase == PHASE_YELLOW) {
    if (elapsed >= YELLOW_TIME_MS) {
      // Serve the road that just had green (common to both modes)
      int served = min(vehicleCount[currentRoad], VEHICLES_SERVED_PER_GREEN);
      vehicleCount[currentRoad] -= served;
      waitingTime[currentRoad] = 0;

      // Safe boundary: check whether a mode switch is needed
      if (systemMode == MODE_ADAPTIVE && anySensorFaulty()) {
        systemMode = MODE_FALLBACK;
        fallbackRoadIndex = 0;
        addEvent("Adaptive priority disabled.");
        addEvent("System switched to fallback mode.");
      }

      if (systemMode == MODE_FALLBACK) {
        // Insert a safety ALL-RED clearance before the next road's green
        currentRoad = -1;
        phase = PHASE_ALL_RED;
        phaseStartTime = now;
        applySignals();
      } else {
        int next = pickNextRoad(currentRoad);
        currentRoad = next;
        phase = PHASE_GREEN;
        phaseStartTime = now;
        applySignals();
      }
    }

  } else { // PHASE_ALL_RED (fallback only)
    if (elapsed >= FALLBACK_ALLRED_MS) {
      // Safe boundary: allowed to return to adaptive mode here
      if (systemMode == MODE_FALLBACK && !anySensorFaulty()) {
        systemMode = MODE_ADAPTIVE;
        addEvent("Sensor validation successful.");
        addEvent("Adaptive priority resumed.");
      }

      int next;
      if (systemMode == MODE_FALLBACK) {
        next = fallbackRoadIndex;
        fallbackRoadIndex = (fallbackRoadIndex + 1) % ROADS;
        addEvent(String(ROAD_NAME[next]) + " assigned 60-second GREEN.");
      } else {
        next = pickNextRoad(-1); // first pick after returning to adaptive
      }
      currentRoad = next;
      phase = PHASE_GREEN;
      phaseStartTime = now;
      applySignals();
    }
  }
}

// ---------------------------------------------------------------------
// APPLY LEDS - never allow conflicting greens. currentRoad == -1 means
// every road is RED (used during the fallback ALL_RED clearance phase).
// ---------------------------------------------------------------------
void applySignals() {
  for (int i = 0; i < ROADS; i++) {
    bool isCurrent = (currentRoad >= 0 && i == currentRoad);
    digitalWrite(RED_PIN[i],    isCurrent ? LOW  : HIGH);
    digitalWrite(YELLOW_PIN[i], (isCurrent && phase == PHASE_YELLOW) ? HIGH : LOW);
    digitalWrite(GREEN_PIN[i],  (isCurrent && phase == PHASE_GREEN)  ? HIGH : LOW);
  }
}

// ---------------------------------------------------------------------
// 8/9. SERIAL DASHBOARD
// ---------------------------------------------------------------------
void printDashboard() {
  unsigned long stateElapsed = (millis() - phaseStartTime) / 1000;

  Serial.println();
  Serial.println("========================================");
  Serial.println("       INTELLIGENT TRAFFIC SYSTEM");
  Serial.print("SYSTEM MODE: ");
  Serial.println(systemMode == MODE_ADAPTIVE ? "ADAPTIVE_MODE" : "FALLBACK_MODE");
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
  Serial.println(currentRoad >= 0 ? ROAD_NAME[currentRoad] : "(none - ALL RED)");
  Serial.print("Phase: ");
  Serial.println(phase == PHASE_GREEN ? "GREEN" : (phase == PHASE_YELLOW ? "YELLOW" : "ALL_RED"));
  Serial.print("Time in phase: "); Serial.print(stateElapsed); Serial.println(" sec");
  Serial.println("========================================");
}

String signalTextFor(int i) {
  if (i != currentRoad) return "RED";
  return (phase == PHASE_GREEN) ? "GREEN" : (phase == PHASE_YELLOW ? "YELLOW" : "RED");
}

// ---------------------------------------------------------------------
// 10. CLOUD (optional) - WiFi + HTTP backend and/or MQTT broker
// ---------------------------------------------------------------------
#if ENABLE_CLOUD
void setupWiFi() {
  Serial.print("Connecting to WiFi (");
  Serial.print(WIFI_SSID);
  Serial.println(")...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void maintainCloudConnection() {
  if (WiFi.status() != WL_CONNECTED) return;
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

size_t buildStatusJson(char* buffer, size_t bufferSize) {
  StaticJsonDocument<1024> doc;
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
  doc["currentGreen"] = currentRoad >= 0 ? ROAD_NAME[currentRoad] : "NONE";
  doc["phase"]        = (phase == PHASE_GREEN) ? "GREEN" : (phase == PHASE_YELLOW ? "YELLOW" : "ALL_RED");
  doc["systemMode"]   = systemMode == MODE_ADAPTIVE ? "ADAPTIVE_MODE" : "FALLBACK_MODE";
  doc["sensorFaultDetected"] = anySensorFaulty();

  JsonArray events = doc.createNestedArray("events");
  for (int k = 0; k < LOG_SIZE; k++) {
    int idx = (logHead + k) % LOG_SIZE;
    if (eventLog[idx].length() > 0) events.add(eventLog[idx]);
  }

  return serializeJson(doc, buffer, bufferSize);
}

void publishStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  char buffer[1024];
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
