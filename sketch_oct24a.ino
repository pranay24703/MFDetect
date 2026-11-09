// Person alert: ESP32 + VL53L4CX (ToF) + MAX98357A (I2S amp)
// Object seen between 1.0 and 1.5 m -> push notification (ntfy) + chime.
//
// Arduino libraries (Library Manager):
//   - "STM32duino VL53L4CX"
// Board package: "esp32" by Espressif, version 3.x (uses built in ESP_I2S)
// Board: "ESP32 Dev Module"

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <vl53l4cx_class.h>
#include "voice_data.h"   // keep in the same folder as this sketch

// ---------- USER SETTINGS ----------
const char* WIFI_SSID = "EvoPhilly";
const char* WIFI_PASS = "";   // empty = open network
// Pick a long random topic, anyone who knows it can read/post to it
const char* NTFY_TOPIC = "pranay-doorsensor-8f3k2x9q";
// Heartbeats go to a separate topic so they don't buzz your alert topic.
// Subscribe to it in the ntfy app only if you want to see them.
const char* HEARTBEAT_TOPIC = "pranay-doorsensor-8f3k2x9q-heartbeat";
const uint32_t HEARTBEAT_MS = 2UL * 60UL * 1000UL;  // every 2 minutes

// Only active between these local hours (24h clock). 0 to 6 = midnight to 6 AM.
const int ACTIVE_START_HOUR = 0;
const int ACTIVE_END_HOUR   = 6;
const char* TZ_INFO = "EST5EDT,M3.2.0,M11.1.0";  // Philadelphia

const float VOICE_VOLUME = 0.8f;  // 0.0 to 1.0

// Debugging
const bool DEBUG_PRINT_DISTANCE = true;       // print every reading to Serial Monitor
const bool DEBUG_IGNORE_TIME_WINDOW = True;  // true = work any time of day (set false for real use)

const int MIN_MM = 400;           // detection window
const int MAX_MM = 1000;
const int HITS_REQUIRED = 5;      // consecutive in-range readings before alert
const uint32_t REARM_MS = 3000;   // must be clear this long before next alert
const uint32_t COOLDOWN_MS = 10000;  // minimum time between alerts

// ---------- PINS ----------
#define PIN_SDA   21
#define PIN_SCL   22
#define PIN_XSHUT 4
#define PIN_BCLK  26
#define PIN_LRC   25
#define PIN_DIN   27

// ---------- GLOBALS ----------
// Qwiic has no XSHUT line, so pass -1. (If wiring XSHUT to GPIO4, use PIN_XSHUT.)
VL53L4CX sensor(&Wire, -1);
I2SClass i2s;
volatile bool playRequest = false;
int lastMm = -1;   // most recent distance seen, for the heartbeat (-1 = no object)

// ---------- AUDIO ----------
void playTone(float freq, int ms, float amp) {
  const int SR = 16000;
  const int n = SR * ms / 1000;
  const int fade = SR / 200;  // 5 ms fade in/out, avoids clicks
  static int16_t buf[256];
  float phase = 0, inc = 2.0f * PI * freq / SR;
  int done = 0;
  while (done < n) {
    int chunk = min(256, n - done);
    for (int i = 0; i < chunk; i++) {
      int idx = done + i;
      float env = min(1.0f, (float)min(idx, n - idx) / fade);
      buf[i] = (int16_t)(sinf(phase) * amp * 32767.0f * env);
      phase += inc;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;
    }
    i2s.write((uint8_t*)buf, chunk * sizeof(int16_t));
    done += chunk;
  }
}

// Plays the pre generated speech clip (see make_voice.sh)
void playVoice(float volume) {
  static int16_t buf[256];
  for (uint32_t i = 0; i < VOICE_SAMPLES; i += 256) {
    int chunk = min((uint32_t)256, VOICE_SAMPLES - i);
    for (int k = 0; k < chunk; k++) buf[k] = (int16_t)(VOICE_DATA[i + k] * volume);
    i2s.write((uint8_t*)buf, chunk * sizeof(int16_t));
  }
}

void audioTask(void*) {
  for (;;) {
    if (playRequest) {
      playRequest = false;
      playTone(880, 150, 0.5f);   // short chime to get attention
      playTone(1175, 250, 0.5f);
      vTaskDelay(pdMS_TO_TICKS(150));
      playVoice(VOICE_VOLUME);    // the clip in voice_data.h
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------- WIFI / NOTIFY ----------
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  // Empty password = open network
  WiFi.begin(WIFI_SSID, strlen(WIFI_PASS) ? WIFI_PASS : nullptr);
  Serial.print("WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " FAILED");
}

// Generic ntfy POST. Returns the HTTP code (or -1 if it couldn't start).
int ntfyPost(const char* topic, const char* title, const char* priority,
             const char* tags, const String& body) {
  WiFiClientSecure client;
  client.setInsecure();  // skips cert check; fine for a hobby project
  HTTPClient http;
  http.setTimeout(8000);
  String url = String("https://ntfy.sh/") + topic;
  if (!http.begin(client, url)) return -1;
  http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  http.addHeader("Tags", tags);
  int code = http.POST(body);
  http.end();
  return code;
}

void sendNotification(int mm) {
  if (WiFi.status() != WL_CONNECTED) wifiConnect();
  if (WiFi.status() != WL_CONNECTED) return;
  int code = ntfyPost(NTFY_TOPIC, "Motherfucker detected", "high", "warning",
                      "Something at " + String(mm / 10) + " cm");
  Serial.printf("ntfy HTTP %d\n", code);
}

// "min" priority = no sound or vibration, just a quiet entry in the topic.
void sendHeartbeat(const char* reason) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Heartbeat skipped, WiFi down (reconnecting)");
    WiFi.reconnect();  // non blocking, so ranging keeps running
    return;
  }
  String body = String(reason) + " | uptime " + String(millis() / 60000) + " min"
              + " | RSSI " + String(WiFi.RSSI()) + " dBm"
              + " | last distance " + (lastMm < 0 ? String("none") : String(lastMm) + " mm")
              + " | active window " + (isActiveTime() ? "yes" : "no");
  int code = ntfyPost(HEARTBEAT_TOPIC, "Heartbeat", "min", "green_heart", body);
  Serial.printf("Heartbeat HTTP %d\n", code);
}

bool isActiveTime() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;  // clock not synced yet
  return t.tm_hour >= ACTIVE_START_HOUR && t.tm_hour < ACTIVE_END_HOUR;
}

// Init and start ranging. 0x52 is the sensor's default address, so we never move it.
// (Moving it to another address sticks across ESP32 resets because the sensor stays
// powered, and then the next boot can't find it. That caused the random hangs.)
bool startSensor() {
  int st = sensor.InitSensor(0x52);
  if (st != 0) {
    Serial.printf("  InitSensor status %d\n", st);
    return false;
  }
  st = sensor.VL53L4CX_StartMeasurement();
  if (st != 0) {
    Serial.printf("  StartMeasurement status %d\n", st);
    return false;
  }
  return true;
}

int scanI2C(bool &has29, bool &has09) {
  int found = 0;
  has29 = has09 = false;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X\n", a);
      found++;
      if (a == 0x29) has29 = true;
      if (a == 0x09) has09 = true;
    }
  }
  return found;
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  i2s.setPins(PIN_BCLK, PIN_LRC, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("I2S init failed");
  }
  xTaskCreate(audioTask, "audio", 4096, nullptr, 1, nullptr);

  wifiConnect();
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);  // slower is more reliable over Qwiic cables
  delay(200);             // let the sensor finish powering up
  // I2C scan: the VL53L4CX should show up at 0x29
  Serial.println("Scanning I2C...");
  bool has29 = false, has09 = false;
  int found = scanI2C(has29, has09);
  if (!found) Serial.println("  NOTHING FOUND: check SDA/SCL/3V3/GND wiring (power cycle too)");

  // Older versions of this sketch moved the sensor to address 0x09. It keeps that
  // address until power is removed, so put it back to 0x29.
  if (has09 && !has29) {
    Serial.println("Sensor is stuck at 0x09, moving it back to 0x29...");
    Wire.beginTransmission(0x09);
    Wire.write(0x00);  // register 0x0001 = I2C_SLAVE__DEVICE_ADDRESS
    Wire.write(0x01);
    Wire.write(0x29);
    Serial.printf("  result: %d (0 = OK)\n", Wire.endTransmission());
    delay(50);
    Serial.println("Rescanning...");
    scanI2C(has29, has09);
  }

  sensor.begin();
  int attempt = 0;
  while (!startSensor()) {
    attempt++;
    Serial.printf("Sensor start attempt %d FAILED. Check the I2C scan above:\n", attempt);
    Serial.println("  no device listed = wiring/power problem; unplug USB for 5 s and retry");
    delay(2000);
    scanI2C(has29, has09);
  }
  Serial.println("Sensor started OK");
  Serial.println("Ready");

  sendHeartbeat("booted");
  playRequest = true;  // startup chime
}

// ---------- LOOP ----------
void loop() {
  static int hits = 0;
  static bool armed = true;
  static uint32_t lastInRange = 0, lastAlert = 0;
  static uint32_t lastBeat = millis();

  // Heartbeat runs even outside the active window, so you know the device is alive
  if (millis() - lastBeat >= HEARTBEAT_MS) {
    lastBeat = millis();
    sendHeartbeat("alive");
  }

  if (!DEBUG_IGNORE_TIME_WINDOW && !isActiveTime()) {
    static uint32_t lastMsg = 0;
    if (DEBUG_PRINT_DISTANCE && millis() - lastMsg > 10000) {
      lastMsg = millis();
      struct tm t;
      if (getLocalTime(&t, 0)) Serial.printf("Outside active hours (%02d:%02d)\n", t.tm_hour, t.tm_min);
      else Serial.println("Clock not synced yet");
    }
    hits = 0;
    armed = true;
    delay(1000);
    return;
  }

  uint8_t ready = 0;
  sensor.VL53L4CX_GetMeasurementDataReady(&ready);
  static uint32_t lastData = millis(), lastWarn = 0;
  if (!ready) {
    if (millis() - lastData > 2000 && millis() - lastWarn > 2000) {
      lastWarn = millis();
      Serial.println("No data from sensor for 2 s, restarting it");
      static int failures = 0;
      if (startSensor()) {
        failures = 0;
        lastData = millis();
      } else if (++failures >= 3) {
        Serial.println("Sensor not recovering, rebooting");
        delay(500);
        ESP.restart();
      }
    }
    delay(5);
    return;
  }
  lastData = millis();

  VL53L4CX_MultiRangingData_t data;
  if (sensor.VL53L4CX_GetMultiRangingData(&data) == 0) {
    if (DEBUG_PRINT_DISTANCE) {
      if (data.NumberOfObjectsFound == 0) Serial.println("no object");
      for (int j = 0; j < data.NumberOfObjectsFound; j++) {
        Serial.printf("obj%d: %4d mm  status=%d%s\n", j,
                      data.RangeData[j].RangeMilliMeter, data.RangeData[j].RangeStatus,
                      (data.RangeData[j].RangeMilliMeter >= MIN_MM &&
                       data.RangeData[j].RangeMilliMeter <= MAX_MM) ? "  <-- IN WINDOW" : "");
      }
    }
    lastMm = data.NumberOfObjectsFound ? data.RangeData[0].RangeMilliMeter : -1;
    bool inRange = false;
    int nearest = 0;
    for (int j = 0; j < data.NumberOfObjectsFound; j++) {
      uint8_t st = data.RangeData[j].RangeStatus;
      int d = data.RangeData[j].RangeMilliMeter;
      if ((st == 0 || st == 6 || st == 9) && d >= MIN_MM && d <= MAX_MM) {
        inRange = true;
        nearest = d;
        break;
      }
    }

    if (inRange) {
      hits++;
      lastInRange = millis();
    } else {
      hits = 0;
      if (!armed && millis() - lastInRange > REARM_MS) armed = true;
    }

    if (hits >= HITS_REQUIRED && armed && (lastAlert == 0 || millis() - lastAlert > COOLDOWN_MS)) {
      armed = false;
      lastAlert = millis();
      Serial.printf("ALERT at %d mm\n", nearest);
      playRequest = true;
      sendNotification(nearest);
    }
  }
  sensor.VL53L4CX_ClearInterruptAndStartMeasurement();
}