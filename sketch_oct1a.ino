#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <Wire.h>
#include <Adafruit_BMP280.h>

// ---------------- WiFi Credentials ----------------
#define WIFI_SSID "abc"
#define WIFI_PASSWORD "xyz"

// ---------------- Firebase Credentials ----------------
#define API_KEY "AIzaSyBjWu15EWhWOvNUSi7rkaugOV8yGSsNxIM"
#define DATABASE_URL "https://wheelchair-locator-485aa-default-rtdb.asia-southeast1.firebasedatabase.app/"

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------------- Anchors ----------------
struct Anchor {
  const char* ssid;
  float emaRssi;
  bool seen;
  unsigned long lastSeen;
};

Anchor anchors[] = {
  {"LOC_1", -100.0f, false, 0},
  {"LOC_2", -100.0f, false, 0},
};
const int NUM_ANCHORS = sizeof(anchors) / sizeof(anchors[0]);

// ---------------- RSSI Settings ----------------
const float ALPHA = 0.35f;
const unsigned long STALE_MS = 8000;
const float DECAY_PER_SCAN = 0.8f;
const int   RSSI_REF = -40;
const float N_PL     = 2.5f;

float estimateDistance(float rssi) {
  return powf(10.0f, (RSSI_REF - rssi) / (10.0f * N_PL));
}

int findAnchor(const String& s) {
  for (int i = 0; i < NUM_ANCHORS; i++) {
    if (s == anchors[i].ssid) return i;
  }
  return -1;
}

// ---------------- BMP280 Setup ----------------
Adafruit_BMP280 bmp; 
float baseAltitude = 0.0;

// ---------------- Button ----------------
#define BUTTON_PIN 15

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(" Connected!");

  // Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // BMP280 Init
  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 not found!");
    while (1);
  }
  baseAltitude = bmp.readAltitude(1013.25);  // store ground floor reference
  Serial.printf("Base altitude: %.2f m\n", baseAltitude);

  // ESP32 WiFi scan mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);
}

// ---------------- Loop ----------------
void loop() {
  // --- Scan anchors ---
  for (int i = 0; i < NUM_ANCHORS; i++) anchors[i].seen = false;

  int n = WiFi.scanNetworks(false, true);
  unsigned long now = millis();

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);

    int idx = findAnchor(ssid);
    if (idx >= 0) {
      anchors[idx].seen = true;
      anchors[idx].lastSeen = now;
      anchors[idx].emaRssi = ALPHA * rssi + (1.0f - ALPHA) * anchors[idx].emaRssi;
    }
  }

  for (int i = 0; i < NUM_ANCHORS; i++) {
    if (!anchors[i].seen) {
      anchors[i].emaRssi = (anchors[i].emaRssi * DECAY_PER_SCAN) + (-100.0f * (1.0f - DECAY_PER_SCAN));
    }
  }

  // --- Find nearest ---
  int nearestIdx = -1;
  float bestRssi = -9999.0f;
  for (int i = 0; i < NUM_ANCHORS; i++) {
    bool fresh = (now - anchors[i].lastSeen) <= STALE_MS;
    if (fresh && anchors[i].emaRssi > bestRssi) {
      bestRssi = anchors[i].emaRssi;
      nearestIdx = i;
    }
  }

  String nearest = "Unknown";
  if (nearestIdx >= 0) {
    nearest = anchors[nearestIdx].ssid;
  }

  // --- Floor detection ---
  float currentAlt = bmp.readAltitude(1013.25);
  int floor = round((currentAlt - baseAltitude-312) / 3.0);  // assume 3m per floor

  // --- Occupied Button ---
  bool occupied = (digitalRead(BUTTON_PIN) == LOW);  // pressed = occupied

  // --- Print ---
  Serial.println("\n--- Data ---");
  Serial.printf("Nearest: %s\n", nearest.c_str());
  Serial.printf("Floor: %d (alt: %.2f m)\n", floor, currentAlt);
  Serial.printf("Occupied: %s\n", occupied ? "Yes" : "No");

  // --- Firebase Upload ---
  String path = "/wheelchair";
  Firebase.RTDB.setString(&fbdo, path + "/nearest", nearest);
  Firebase.RTDB.setInt(&fbdo, path + "/floor", floor);
  Firebase.RTDB.setBool(&fbdo, path + "/occupied", occupied);

  for (int i = 0; i < NUM_ANCHORS; i++) {
    float dist = estimateDistance(anchors[i].emaRssi);
    Firebase.RTDB.setFloat(&fbdo, path + "/" + anchors[i].ssid + "/RSSI", anchors[i].emaRssi);
    Firebase.RTDB.setFloat(&fbdo, path + "/" + anchors[i].ssid + "/distance", dist);
  }

  WiFi.scanDelete();
  delay(3000);
}
