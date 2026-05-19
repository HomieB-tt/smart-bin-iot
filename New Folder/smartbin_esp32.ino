// ============================================================
//  SmartBin ESP32 Firmware
//  ISBAT University — IoT Unit
//
//  Hardware:
//    - Inductive proximity sensor (LJ12A3)  → GPIO 34 (digital)
//    - Capacitive moisture sensor v1.2       → GPIO 35 (analog)
//    - HC-SR04 ultrasonic ×3 (fill level)   → see pin definitions
//    - MG996R servo motors ×3 (trapdoors)   → GPIO 16, 17, 18
//    - HC-SR04 proximity (lid open)          → GPIO 25, 26
//
//  Libraries required (install via Arduino Library Manager):
//    - PubSubClient by Nick O'Leary  (MQTT)
//    - ESP32Servo by Kevin Harrington
//    - ArduinoJson by Benoit Blanchon
//
//  Board: ESP32 Dev Module
//  Upload speed: 115200
// ============================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ── Wi-Fi credentials ────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ── MQTT broker (laptop's LAN IP — NOT "localhost") ──────────
// Find with: ipconfig (Windows) or ip addr (Linux/Mac)
const char* MQTT_SERVER   = "192.168.1.100";   // ← change this
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "smartbin-esp32";

// ── MQTT Topics ──────────────────────────────────────────────
#define TOPIC_FILL_METAL      "smartbin/fill/metal"
#define TOPIC_FILL_ORGANIC    "smartbin/fill/organic"
#define TOPIC_FILL_INORGANIC  "smartbin/fill/inorganic"
#define TOPIC_CLASSIFY        "smartbin/classify"
#define TOPIC_SENSOR_STATUS   "smartbin/sensor/status"
#define TOPIC_HEARTBEAT       "smartbin/system/heartbeat"

// ── Pin Definitions ──────────────────────────────────────────

// Classification sensors
#define PIN_INDUCTIVE       34    // Digital: HIGH = metal detected
#define PIN_MOISTURE        35    // Analog:  low value = wet/organic

// Fill-level HC-SR04 sensors (TRIG, ECHO per compartment)
#define PIN_FILL_METAL_TRIG    4
#define PIN_FILL_METAL_ECHO    5
#define PIN_FILL_ORG_TRIG      13
#define PIN_FILL_ORG_ECHO      14
#define PIN_FILL_INORG_TRIG    19
#define PIN_FILL_INORG_ECHO    21

// Lid proximity HC-SR04
#define PIN_LID_TRIG        25
#define PIN_LID_ECHO        26

// Servo motors (one per trapdoor compartment)
#define PIN_SERVO_METAL     16
#define PIN_SERVO_ORGANIC   17
#define PIN_SERVO_INORGANIC 18

// Status LED
#define PIN_LED             2     // Built-in LED on most ESP32 boards

// ── Calibration constants ────────────────────────────────────

// Moisture sensor: adjust these to your sensor + bin conditions
// Lower raw ADC value = wetter. Read dry air and wet material to calibrate.
#define MOISTURE_WET_THRESHOLD   1500   // below this → organic
#define MOISTURE_DRY_MIN         2800   // above this → not organic

// Tank geometry: distance from sensor to empty bin floor (cm)
// Measure physically with a ruler after mounting the sensor
#define BIN_DEPTH_CM             30.0   // adjust per your bin

// Lid opens when someone is within this distance (cm)
#define LID_TRIGGER_DISTANCE_CM  50.0

// Servo angles
#define SERVO_CLOSED_DEG         0
#define SERVO_OPEN_DEG           90

// Timing
#define FILL_PUBLISH_INTERVAL_MS   10000   // publish fill levels every 10s
#define HEARTBEAT_INTERVAL_MS      30000   // heartbeat every 30s
#define CLASSIFICATION_HOLD_MS     2000    // hold trapdoor open for 2s
#define LID_HOLD_MS                3000    // keep lid open for 3s after no presence
#define DEBOUNCE_MS                500     // debounce classification events

// ── Globals ──────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

Servo servoMetal;
Servo servoOrganic;
Servo servoInorganic;

unsigned long lastFillPublish    = 0;
unsigned long lastHeartbeat      = 0;
unsigned long lastClassification = 0;
unsigned long lidOpenedAt        = 0;

bool lidIsOpen           = false;
bool classifyingItem     = false;
unsigned long classifyAt = 0;
String pendingCategory   = "";

// ── Wi-Fi ────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected. IP: " + WiFi.localIP().toString());
    digitalWrite(PIN_LED, HIGH);
  } else {
    Serial.println("\n[WiFi] FAILED — running in offline mode");
  }
}

// ── MQTT ─────────────────────────────────────────────────────
void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connecting...");
    if (mqtt.connect(MQTT_CLIENT)) {
      Serial.println(" connected.");
      publishSensorStatus("metal",    true);
      publishSensorStatus("organic",  true);
      publishSensorStatus("inorganic",true);
    } else {
      Serial.print(" failed (rc=");
      Serial.print(mqtt.state());
      Serial.println("). Retrying in 3s...");
      delay(3000);
    }
  }
}

// ── Ultrasonic distance (blocking, ~30ms per call) ───────────
float readDistanceCm(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
  if (duration == 0) return -1.0; // timeout = out of range

  float distanceCm = duration * 0.034 / 2.0;
  return distanceCm;
}

// ── Fill % from distance ──────────────────────────────────────
// Sensor is mounted at top of bin pointing down.
// distance = how far to water/waste surface.
// fill % = (binDepth - distance) / binDepth * 100
int distanceToFillPct(float distanceCm) {
  if (distanceCm < 0) return -1; // sensor error
  float filled = BIN_DEPTH_CM - distanceCm;
  if (filled < 0)           filled = 0;
  if (filled > BIN_DEPTH_CM) filled = BIN_DEPTH_CM;
  return (int)((filled / BIN_DEPTH_CM) * 100.0);
}

// ── Classification ────────────────────────────────────────────
// Returns: "metal", "organic", "inorganic", or "" (no item)
String classifyItem() {
  bool isMetal   = (digitalRead(PIN_INDUCTIVE) == HIGH);
  int  moisture  = analogRead(PIN_MOISTURE);       // 0–4095 on ESP32

  Serial.printf("[SENSOR] Inductive: %s  Moisture ADC: %d\n",
    isMetal ? "METAL" : "no", moisture);

  if (isMetal) {
    return "metal";
  }
  if (moisture < MOISTURE_WET_THRESHOLD) {
    return "organic";
  }
  // Default: inorganic (dry, non-metal)
  if (moisture > MOISTURE_DRY_MIN) {
    return "inorganic";
  }

  return ""; // ambiguous — no confident classification
}

// ── Servo control ─────────────────────────────────────────────
void openTrapdoor(const String& category) {
  Serial.printf("[SERVO] Opening trapdoor: %s\n", category.c_str());
  if (category == "metal")     servoMetal.write(SERVO_OPEN_DEG);
  if (category == "organic")   servoOrganic.write(SERVO_OPEN_DEG);
  if (category == "inorganic") servoInorganic.write(SERVO_OPEN_DEG);
}

void closeTrapdoor(const String& category) {
  Serial.printf("[SERVO] Closing trapdoor: %s\n", category.c_str());
  if (category == "metal")     servoMetal.write(SERVO_CLOSED_DEG);
  if (category == "organic")   servoOrganic.write(SERVO_CLOSED_DEG);
  if (category == "inorganic") servoInorganic.write(SERVO_CLOSED_DEG);
}

void closeAllTrapdoors() {
  servoMetal.write(SERVO_CLOSED_DEG);
  servoOrganic.write(SERVO_CLOSED_DEG);
  servoInorganic.write(SERVO_CLOSED_DEG);
}

// ── MQTT publish helpers ──────────────────────────────────────
void publishFill(const char* topic, float distanceCm) {
  int pct = distanceToFillPct(distanceCm);
  if (pct < 0) {
    publishSensorStatus(
      strstr(topic, "metal")    ? "metal" :
      strstr(topic, "organic")  ? "organic" : "inorganic",
      false
    );
    return;
  }

  StaticJsonDocument<128> doc;
  doc["fill_pct"]     = pct;
  doc["distance_cm"]  = distanceCm;

  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish(topic, buf, true); // retained=true so dashboard gets it on connect

  Serial.printf("[MQTT] %s → fill_pct=%d%% dist=%.1fcm\n", topic, pct, distanceCm);
}

void publishClassification(const String& category) {
  StaticJsonDocument<128> doc;
  doc["category"]   = category;
  doc["confidence"] = 1.0; // rule-based = deterministic

  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_CLASSIFY, buf);

  Serial.printf("[MQTT] classify → %s\n", category.c_str());
}

void publishSensorStatus(const char* sensor, bool ok) {
  StaticJsonDocument<64> doc;
  doc["sensor"] = sensor;
  doc["ok"]     = ok;

  char buf[64];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_SENSOR_STATUS, buf);
}

void publishHeartbeat() {
  StaticJsonDocument<64> doc;
  doc["uptime_s"]  = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();

  char buf[64];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_HEARTBEAT, buf);
  Serial.println("[MQTT] Heartbeat sent");
}

// ── Lid control ───────────────────────────────────────────────
// Add a servo for the lid if you want motorised lid.
// For now this just controls an LED as a proxy — swap for servo logic.
void openLid() {
  if (!lidIsOpen) {
    Serial.println("[LID] Opening");
    // servoLid.write(SERVO_OPEN_DEG);  // uncomment if you add a lid servo
    lidIsOpen   = true;
    lidOpenedAt = millis();
  }
}

void closeLid() {
  if (lidIsOpen) {
    Serial.println("[LID] Closing");
    // servoLid.write(SERVO_CLOSED_DEG);
    lidIsOpen = false;
  }
}

// ── setup() ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SmartBin ESP32 Firmware ===");

  // Pin modes
  pinMode(PIN_INDUCTIVE,        INPUT);
  pinMode(PIN_FILL_METAL_TRIG,  OUTPUT);
  pinMode(PIN_FILL_METAL_ECHO,  INPUT);
  pinMode(PIN_FILL_ORG_TRIG,    OUTPUT);
  pinMode(PIN_FILL_ORG_ECHO,    INPUT);
  pinMode(PIN_FILL_INORG_TRIG,  OUTPUT);
  pinMode(PIN_FILL_INORG_ECHO,  INPUT);
  pinMode(PIN_LID_TRIG,         OUTPUT);
  pinMode(PIN_LID_ECHO,         INPUT);
  pinMode(PIN_LED,              OUTPUT);

  // Attach servos
  servoMetal.attach(PIN_SERVO_METAL);
  servoOrganic.attach(PIN_SERVO_ORGANIC);
  servoInorganic.attach(PIN_SERVO_INORGANIC);
  closeAllTrapdoors();

  // Connect
  connectWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setKeepAlive(60);
  connectMQTT();

  // Publish initial fill levels
  lastFillPublish = millis() - FILL_PUBLISH_INTERVAL_MS;
  Serial.println("[READY] SmartBin online.");
}

// ── loop() ───────────────────────────────────────────────────
void loop() {
  // Maintain connections
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected())             connectMQTT();
  mqtt.loop();

  unsigned long now = millis();

  // ── 1. Lid proximity ──────────────────────────────────────
  float lidDist = readDistanceCm(PIN_LID_TRIG, PIN_LID_ECHO);
  if (lidDist > 0 && lidDist < LID_TRIGGER_DISTANCE_CM) {
    openLid();
  } else if (lidIsOpen && (now - lidOpenedAt > LID_HOLD_MS)) {
    closeLid();
  }

  // ── 2. Item classification ────────────────────────────────
  // Only classify while lid is open and we're not mid-classification
  if (lidIsOpen && !classifyingItem) {
    unsigned long sinceLastClassify = now - lastClassification;
    if (sinceLastClassify > DEBOUNCE_MS) {
      String category = classifyItem();
      if (category.length() > 0) {
        classifyingItem = true;
        pendingCategory = category;
        classifyAt      = now;

        openTrapdoor(category);
        publishClassification(category);
        lastClassification = now;
      }
    }
  }

  // Close trapdoor after hold time
  if (classifyingItem && (now - classifyAt > CLASSIFICATION_HOLD_MS)) {
    closeTrapdoor(pendingCategory);
    pendingCategory = "";
    classifyingItem = false;
  }

  // ── 3. Fill level publishing ──────────────────────────────
  if (now - lastFillPublish > FILL_PUBLISH_INTERVAL_MS) {
    float dMetal    = readDistanceCm(PIN_FILL_METAL_TRIG,   PIN_FILL_METAL_ECHO);
    float dOrganic  = readDistanceCm(PIN_FILL_ORG_TRIG,     PIN_FILL_ORG_ECHO);
    float dInorganic= readDistanceCm(PIN_FILL_INORG_TRIG,   PIN_FILL_INORG_ECHO);

    publishFill(TOPIC_FILL_METAL,     dMetal);
    publishFill(TOPIC_FILL_ORGANIC,   dOrganic);
    publishFill(TOPIC_FILL_INORGANIC, dInorganic);

    lastFillPublish = now;
  }

  // ── 4. Heartbeat ─────────────────────────────────────────
  if (now - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    publishHeartbeat();
    lastHeartbeat = now;
  }

  delay(50); // small yield to avoid WDT reset
}
