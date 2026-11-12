/*
  SpecCam - inspection engine (ESP32-S3 N16R8)

  Gets JPEG frames from the ESP32-CAM over UART, runs the active recipe,
  shows PASS/FAIL on OLED + RGB LED + output pins, and exposes an HTTP API
  that the laptop agent uses to set up new products.

  Wiring:
    CAM link:  GPIO18 (RX) <- CAM GPIO14,  GPIO17 (TX) -> CAM GPIO15,  GND, 5V
    OLED:      SDA GPIO8, SCL GPIO9, 3V3, GND   (SSD1306 128x64, I2C 0x3C, optional)
    Servo:     signal GPIO4, power 5V (separate supply recommended), GND common (optional)
    Outputs:   GPIO7 = PASS, GPIO6 = FAIL (to a PLC, relay or LEDs)
    BOOT btn:  inspect once / add a teach sample

  Arduino IDE: ESP32S3 Dev Module, Flash 16MB, PSRAM OPI, Partition "8M with spiffs".
  Put Wi-Fi in secrets.h. Board is reachable at http://speccam.local

  A recipe is plain JSON (see recipes/example_cap.json). The agent writes it,
  the S3 executes it deterministically. No AI in the inspection loop.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "img_converters.h"
#include "mbedtls/base64.h"
#include "secrets.h"

// ---------- Pins / settings ----------
#define LINK_RX 18
#define LINK_TX 17
#define LINK_BAUD 2000000
#define SERVO_PIN 4
#define OUT_PASS 7
#define OUT_FAIL 6
#define BTN_PIN 0
#define RGB_PIN 48
#define I2C_SDA 8
#define I2C_SCL 9
#define SWAP_565 false  // flip if colours come out wrong (red part measures blue hue)

const int MAXW = 640, MAXH = 480;
const size_t JPG_MAX = 200 * 1024;

// ---------- Buffers (PSRAM) ----------
uint8_t *jpgBuf, *rgbBuf, *grayBuf, *markBuf;
int32_t *stackBuf;
size_t jpgLen = 0;
int W = 0, H = 0;
bool frameValid = false;

// ---------- State ----------
WebServer server(80);
Preferences prefs;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;
JsonDocument recipe;
bool haveRecipe = false;
String activeId = "";
bool live = false;
unsigned long lastLive = 0;
int servoAngle = 90;
uint32_t nPass = 0, nFail = 0;
String lastResult = "{}";

struct Rect { int x, y, w, h; };
struct Loc { bool found; int x, y, w, h; float area; int thr; };

// Teach state (good-only anomaly learning)
bool teaching = false;
String teachCheck = "";
int teachN = 0, teachFeat = 0;
float *teachSum = nullptr, *teachSq = nullptr;

// =====================================================================
// Small helpers
// =====================================================================
void rgb(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(RGB_PIN, r, g, b); }

Rect clampRect(Rect r) {
  if (r.x < 0) { r.w += r.x; r.x = 0; }
  if (r.y < 0) { r.h += r.y; r.y = 0; }
  if (r.x + r.w > W) r.w = W - r.x;
  if (r.y + r.h > H) r.h = H - r.y;
  if (r.w < 1) r.w = 1;
  if (r.h < 1) r.h = 1;
  return r;
}

Rect readRoi(JsonVariant v, int dx, int dy) {
  if (v.is<JsonArray>() && v.size() == 4) {
    return clampRect({ v[0].as<int>() + dx, v[1].as<int>() + dy, v[2].as<int>(), v[3].as<int>() });
  }
  return { 0, 0, W, H };
}

inline void pixRGB(int i, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint16_t p = SWAP_565 ? (rgbBuf[2 * i] | (rgbBuf[2 * i + 1] << 8)) : ((rgbBuf[2 * i] << 8) | rgbBuf[2 * i + 1]);
  r = ((p >> 11) & 0x1F) << 3;
  g = ((p >> 5) & 0x3F) << 2;
  b = (p & 0x1F) << 3;
}

void rgb2hsv(uint8_t r, uint8_t g, uint8_t b, float &h, float &s, float &v) {
  float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
  v = mx / 255.0f;
  s = mx > 0 ? d / mx : 0;
  if (d == 0) h = 0;
  else if (mx == r) h = 60.0f * fmodf((g - b) / d, 6.0f);
  else if (mx == g) h = 60.0f * ((b - r) / d + 2.0f);
  else h = 60.0f * ((r - g) / d + 4.0f);
  if (h < 0) h += 360;
}

int otsu(Rect r) {
  uint32_t hist[256] = { 0 };
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) hist[grayBuf[y * W + x]]++;
  uint32_t n = r.w * r.h;
  double sum = 0;
  for (int t = 0; t < 256; t++) sum += (double)t * hist[t];
  double sumB = 0, best = -1;
  uint32_t wB = 0;
  int th = 128;
  for (int t = 0; t < 256; t++) {
    wB += hist[t];
    if (!wB) continue;
    uint32_t wF = n - wB;
    if (!wF) break;
    sumB += (double)t * hist[t];
    double mB = sumB / wB, mF = (sum - sumB) / wF;
    double between = (double)wB * wF * (mB - mF) * (mB - mF);
    if (between > best) { best = between; th = t; }
  }
  return th;
}

// =====================================================================
// Camera link
// =====================================================================
bool readExact(uint8_t *d, size_t n, uint32_t idleMs) {
  size_t got = 0;
  unsigned long t = millis();
  while (got < n) {
    int a = Serial1.available();
    if (a > 0) {
      got += Serial1.readBytes(d + got, min((size_t)a, n - got));
      t = millis();
    } else if (millis() - t > idleMs) {
      return false;
    }
  }
  return true;
}

String camCmd(const String &cmd, uint32_t timeout = 600) {
  while (Serial1.available()) Serial1.read();
  Serial1.print(cmd + "\n");
  String resp;
  unsigned long t = millis();
  while (millis() - t < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      if (c == '\n') {
        if (resp.startsWith("OK") || resp.startsWith("ERR")) return resp;
        resp = "";
      } else if (c != '\r') {
        resp += c;
      }
    }
  }
  return "TIMEOUT";
}

bool camCapture() {
  while (Serial1.available()) Serial1.read();
  Serial1.print("CAP\n");
  unsigned long t = millis();
  int st = 0;
  while (st < 3 && millis() - t < 1500) {
    if (!Serial1.available()) continue;
    uint8_t c = Serial1.read();
    if (st == 0) st = (c == 0xA5) ? 1 : 0;
    else if (st == 1) st = (c == 0x5A) ? 2 : (c == 0xA5 ? 1 : 0);
    else st = (c == 'F') ? 3 : (c == 0xA5 ? 1 : 0);
  }
  if (st != 3) return false;
  uint8_t h[8];
  if (!readExact(h, 8, 300)) return false;
  uint32_t len = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
  uint32_t sum = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
  if (len == 0 || len > JPG_MAX) return false;
  if (!readExact(jpgBuf, len, 500)) return false;
  uint32_t s = 0;
  for (uint32_t i = 0; i < len; i++) s += jpgBuf[i];
  if (s != sum) {
    Serial.println("frame checksum mismatch");
    return false;
  }
  jpgLen = len;
  return true;
}

bool jpegSize(const uint8_t *d, size_t n, int &w, int &h) {
  size_t i = 2;
  while (i + 9 < n) {
    if (d[i] != 0xFF) { i++; continue; }
    uint8_t m = d[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      h = (d[i + 5] << 8) | d[i + 6];
      w = (d[i + 7] << 8) | d[i + 8];
      return true;
    }
    if (m == 0xD8 || m == 0x01 || m == 0xFF || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    i += 2 + ((d[i + 2] << 8) | d[i + 3]);
  }
  return false;
}

bool decodeFrame() {
  int w, h;
  frameValid = false;
  if (!jpegSize(jpgBuf, jpgLen, w, h) || w > MAXW || h > MAXH) return false;
  if (!jpg2rgb565(jpgBuf, jpgLen, rgbBuf, JPG_SCALE_NONE)) return false;
  W = w;
  H = h;
  for (int i = 0; i < W * H; i++) {
    uint8_t r, g, b;
    pixRGB(i, r, g, b);
    grayBuf[i] = (r * 77 + g * 150 + b * 29) >> 8;
  }
  frameValid = true;
  return true;
}

// =====================================================================
// Vision primitives
// =====================================================================
Loc locate(const char *mode, int thr) {
  Loc L = { false, 0, 0, 0, 0, 0, thr };
  if (thr <= 0) L.thr = otsu({ 0, 0, W, H });
  bool dark = strcmp(mode, "light_on_dark") != 0;
  static uint16_t rows[MAXH], cols[MAXW];
  memset(rows, 0, sizeof(rows));
  memset(cols, 0, sizeof(cols));
  uint32_t fg = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t g = grayBuf[y * W + x];
      if (dark ? g < L.thr : g > L.thr) { rows[y]++; cols[x]++; fg++; }
    }
  L.area = (float)fg / (W * H);
  int minR = max(3, W / 50), minC = max(3, H / 50);
  int y0 = -1, y1 = -1, x0 = -1, x1 = -1;
  for (int y = 0; y < H; y++) if (rows[y] >= minR) { if (y0 < 0) y0 = y; y1 = y; }
  for (int x = 0; x < W; x++) if (cols[x] >= minC) { if (x0 < 0) x0 = x; x1 = x; }
  if (y0 >= 0 && x0 >= 0 && L.area > 0.005f) {
    L.found = true;
    L.x = x0; L.y = y0; L.w = x1 - x0 + 1; L.h = y1 - y0 + 1;
  }
  return L;
}

// Anomaly features: per cell mean gray + mean gradient
void cellFeatures(Rect r, int cx, int cy, float *f) {
  for (int j = 0; j < cy; j++)
    for (int i = 0; i < cx; i++) {
      int x0 = r.x + i * r.w / cx, x1 = r.x + (i + 1) * r.w / cx;
      int y0 = r.y + j * r.h / cy, y1 = r.y + (j + 1) * r.h / cy;
      uint32_t sum = 0, grad = 0, n = 0;
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          int p = y * W + x;
          sum += grayBuf[p];
          if (x + 1 < x1 && y + 1 < y1) grad += abs(grayBuf[p + 1] - grayBuf[p]) + abs(grayBuf[p + W] - grayBuf[p]);
          n++;
        }
      int k = (j * cx + i) * 2;
      f[k] = n ? (float)sum / n : 0;
      f[k + 1] = n ? (float)grad / n : 0;
    }
}

int countBlobs(Rect r, bool dark, int thr, int minPx, int &largest) {
  memset(markBuf, 0, W * H);
  int count = 0;
  largest = 0;
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) {
      int p = y * W + x;
      bool on = dark ? grayBuf[p] < thr : grayBuf[p] > thr;
      if (!on || markBuf[p]) continue;
      int sp = 0, size = 0;
      stackBuf[sp++] = p;
      markBuf[p] = 1;
      while (sp) {
        int q = stackBuf[--sp];
        size++;
        int qx = q % W, qy = q / W;
        const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (auto &d : nb) {
          int nx = qx + d[0], ny = qy + d[1];
          if (nx < r.x || ny < r.y || nx >= r.x + r.w || ny >= r.y + r.h) continue;
          int np = ny * W + nx;
          if (markBuf[np]) continue;
          bool non = dark ? grayBuf[np] < thr : grayBuf[np] > thr;
          if (!non) continue;
          markBuf[np] = 1;
          stackBuf[sp++] = np;
        }
      }
      if (size >= minPx) count++;
      if (size > largest) largest = size;
    }
  return count;
}

// =====================================================================
// Recipe execution
// =====================================================================
bool inRange(float v, JsonObject c, float defMin, float defMax) {
  float mn = c["min"] | defMin, mx = c["max"] | defMax;
  return v >= mn && v <= mx;
}

bool runCheck(JsonObject c, int dx, int dy, Loc &loc, JsonObject out) {
  const char *type = c["type"] | "";
  Rect r = readRoi(c["roi"], dx, dy);
  out["name"] = c["name"] | type;
  out["type"] = type;
  JsonArray ro = out["roi"].to<JsonArray>();
  ro.add(r.x); ro.add(r.y); ro.add(r.w); ro.add(r.h);
  float value = 0;
  bool pass = false;

  if (!strcmp(type, "present")) {
    value = loc.area;
    pass = loc.found && inRange(value, c, 0.01, 1.0);
  } else if (!strcmp(type, "mean")) {
    uint32_t s = 0;
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) s += grayBuf[y * W + x];
    value = (float)s / (r.w * r.h);
    pass = inRange(value, c, 0, 255);
  } else if (!strcmp(type, "pixels")) {
    bool dark = strcmp(c["polarity"] | "dark", "bright") != 0;
    int thr = c["thr"] | 0;
    if (thr <= 0) thr = loc.thr;
    uint32_t n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) {
        uint8_t g = grayBuf[y * W + x];
        if (dark ? g < thr : g > thr) n++;
      }
    value = (float)n / (r.w * r.h);
    pass = inRange(value, c, 0, 1);
  } else if (!strcmp(type, "color")) {
    float h0 = c["h"][0] | 0.0f, h1 = c["h"][1] | 360.0f;
    float s0 = c["s"][0] | 0.0f, s1 = c["s"][1] | 1.0f;
    float v0 = c["v"][0] | 0.0f, v1 = c["v"][1] | 1.0f;
    uint32_t n = 0;
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) {
        uint8_t R, G, B;
        float h, s, v;
        pixRGB(y * W + x, R, G, B);
        rgb2hsv(R, G, B, h, s, v);
        bool hOk = h0 <= h1 ? (h >= h0 && h <= h1) : (h >= h0 || h <= h1);
        if (hOk && s >= s0 && s <= s1 && v >= v0 && v <= v1) n++;
      }
    value = (float)n / (r.w * r.h);
    pass = inRange(value, c, 0.5, 1.0);
  } else if (!strcmp(type, "edges")) {
    uint32_t g = 0, n = 0;
    for (int y = r.y; y < r.y + r.h - 1; y++)
      for (int x = r.x; x < r.x + r.w - 1; x++) {
        int p = y * W + x;
        g += abs(grayBuf[p + 1] - grayBuf[p]) + abs(grayBuf[p + W] - grayBuf[p]);
        n++;
      }
    value = n ? (float)g / n : 0;
    pass = inRange(value, c, 0, 1000);
  } else if (!strcmp(type, "blobs")) {
    bool dark = strcmp(c["polarity"] | "dark", "bright") != 0;
    int thr = c["thr"] | 0;
    if (thr <= 0) thr = otsu(r);
    int largest;
    value = countBlobs(r, dark, thr, c["min_px"] | 20, largest);
    pass = inRange(value, c, 1, 1000);
  } else if (!strcmp(type, "anomaly")) {
    int cx = c["cells"][0] | 8, cy = c["cells"][1] | 6;
    JsonArray m = c["model"]["m"], s = c["model"]["s"];
    int nf = cx * cy * 2;
    if (m.isNull() || (int)m.size() != nf) {
      out["error"] = "not taught yet";
      value = -1;
      pass = false;
    } else {
      float *f = (float *)malloc(nf * sizeof(float));
      cellFeatures(r, cx, cy, f);
      float worst = 0;
      int worstCell = 0;
      for (int k = 0; k < nf; k++) {
        float floorS = (k % 2 == 0) ? 3.0f : 1.5f;
        float z = fabsf(f[k] - m[k].as<float>()) / max(s[k].as<float>(), floorS);
        if (z > worst) { worst = z; worstCell = k / 2; }
      }
      free(f);
      value = worst;
      pass = worst <= (c["z"] | 4.0f);
      int i = worstCell % cx, j = worstCell / cx;
      JsonArray wc = out["cell"].to<JsonArray>();
      wc.add(r.x + i * r.w / cx); wc.add(r.y + j * r.h / cy); wc.add(r.w / cx); wc.add(r.h / cy);
    }
  } else {
    out["error"] = "unknown type";
  }
  out["value"] = value;
  out["pass"] = pass;
  return pass;
}

// Runs the active recipe on the frame in memory. Returns "pass", "fail" or "empty".
String inspectFrame(JsonDocument &res, uint32_t captureMs) {
  unsigned long t0 = micros();
  res.clear();
  res["recipe"] = activeId;
  if (!haveRecipe) { res["state"] = "error"; res["error"] = "no active recipe"; return "error"; }
  if (!frameValid) { res["state"] = "error"; res["error"] = "no frame"; return "error"; }

  JsonObject lc = recipe["locate"];
  const char *mode = lc["mode"] | "none";
  Loc loc = { true, 0, 0, W, H, 1.0f, 128 };
  int dx = 0, dy = 0;
  if (strcmp(mode, "none") != 0) {
    loc = locate(mode, lc["thr"] | 0);
    JsonArray b = res["part"].to<JsonArray>();
    b.add(loc.x); b.add(loc.y); b.add(loc.w); b.add(loc.h);
    res["area"] = loc.area;
    if (!loc.found || loc.area < (lc["min_area"] | 0.02f)) {
      res["state"] = "empty";
      res["ms"] = (micros() - t0) / 1000.0 + captureMs;
      return "empty";
    }
    if (!lc["ref"].isNull()) {
      dx = loc.x - lc["ref"][0].as<int>();
      dy = loc.y - lc["ref"][1].as<int>();
    }
  } else {
    loc.thr = otsu({ 0, 0, W, H });
  }
  res["offset"][0] = dx;
  res["offset"][1] = dy;

  bool all = true;
  String firstFail = "";
  JsonArray outs = res["checks"].to<JsonArray>();
  for (JsonObject c : recipe["checks"].as<JsonArray>()) {
    JsonObject o = outs.add<JsonObject>();
    if (!runCheck(c, dx, dy, loc, o)) {
      if (all) firstFail = o["name"].as<String>();
      all = false;
    }
  }
  res["state"] = all ? "pass" : "fail";
  if (!all) res["failed"] = firstFail;
  res["ms_capture"] = captureMs;
  res["ms_inspect"] = (micros() - t0) / 1000.0;
  return all ? "pass" : "fail";
}

// =====================================================================
// Outputs: OLED, LED, pins
// =====================================================================
void showResult(const String &state, const String &detail, float ms) {
  digitalWrite(OUT_PASS, state == "pass");
  digitalWrite(OUT_FAIL, state == "fail");
  if (state == "pass") rgb(0, 60, 0);
  else if (state == "fail") rgb(70, 0, 0);
  else rgb(0, 0, 15);
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(recipe["name"] | activeId.c_str());
  oled.setTextSize(3);
  oled.setCursor(0, 16);
  oled.print(state == "pass" ? "PASS" : state == "fail" ? "FAIL" : state == "empty" ? "----" : "ERR");
  oled.setTextSize(1);
  oled.setCursor(0, 44);
  oled.print(detail.substring(0, 21));
  oled.setCursor(0, 56);
  oled.printf("%.0fms P%lu F%lu", ms, nPass, nFail);
  oled.display();
}

void showMessage(const String &a, const String &b) {
  Serial.println(a + " " + b);
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("SpecCam");
  oled.setTextSize(2);
  oled.setCursor(0, 18);
  oled.print(a.substring(0, 10));
  oled.setTextSize(1);
  oled.setCursor(0, 46);
  oled.print(b.substring(0, 42));
  oled.display();
}

void moveServo(int angle) {
  angle = constrain(angle, 0, 180);
  int step = angle > servoAngle ? 1 : -1;
  while (servoAngle != angle) {
    servoAngle += step;
    uint32_t us = 500 + servoAngle * 2000 / 180;
    ledcWrite(SERVO_PIN, us * 16383 / 20000);
    delay(8);
  }
  uint32_t us = 500 + servoAngle * 2000 / 180;
  ledcWrite(SERVO_PIN, us * 16383 / 20000);
}

// =====================================================================
// Recipes
// =====================================================================
String recipePath(const String &id) { return "/r/" + id + ".json"; }

bool applyRecipeHardware() {
  JsonObject cam = recipe["camera"];
  for (JsonPair kv : cam) camCmd("SET " + String(kv.key().c_str()) + " " + String(kv.value().as<int>()));
  if (!recipe["servo"].isNull()) moveServo(recipe["servo"].as<int>());
  return true;
}

bool activate(const String &id) {
  File f = LittleFS.open(recipePath(id), "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(recipe, f);
  f.close();
  if (e) return false;
  haveRecipe = true;
  activeId = id;
  prefs.putString("active", id);
  nPass = nFail = 0;
  applyRecipeHardware();
  showMessage("READY", recipe["name"] | id.c_str());
  return true;
}

bool saveActive() {
  File f = LittleFS.open(recipePath(activeId), "w");
  if (!f) return false;
  serializeJson(recipe, f);
  f.close();
  return true;
}

// =====================================================================
// One full cycle: capture -> decode -> inspect -> outputs
// =====================================================================
String cycle(JsonDocument &res) {
  unsigned long t = millis();
  if (!camCapture() || !decodeFrame()) {
    res.clear();
    res["state"] = "error";
    res["error"] = "capture failed (check CAM wiring/power)";
    showResult("error", "camera link", 0);
    return "error";
  }
  uint32_t capMs = millis() - t;
  String st = inspectFrame(res, capMs);
  if (st == "pass") nPass++;
  if (st == "fail") nFail++;
  float total = capMs + res["ms_inspect"].as<float>();
  showResult(st, st == "fail" ? res["failed"].as<String>() : st == "empty" ? "place a part" : "", total);
  serializeJson(res, lastResult);
  return st;
}

// =====================================================================
// HTTP API
// =====================================================================
void sendJson(JsonDocument &d, int code = 200) {
  String s;
  serializeJson(d, s);
  server.send(code, "application/json", s);
}

void sendErr(const String &m, int code = 400) {
  server.send(code, "application/json", "{\"error\":\"" + m + "\"}");
}

static const char PAGE[] PROGMEM = R"html(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SpecCam</title><style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:12px;max-width:760px}
button{padding:8px 12px;margin:3px;border:0;border-radius:4px;background:#2d6cdf;color:#fff}
canvas{width:100%;background:#222;border-radius:6px}#st{font-size:40px;font-weight:bold}
.pass{color:#4c4}.fail{color:#e44}.empty{color:#888}td{padding:2px 8px 2px 0;font-size:14px}
</style></head><body><h2>SpecCam</h2>
<div><button onclick="go('/inspect')">Inspect</button><button onclick="live(1)">Live on</button>
<button onclick="live(0)">Live off</button><button onclick="go('/frame.jpg',1)">Capture only</button></div>
<div id="st"></div><div id="rc"></div><canvas id="c" width="320" height="240"></canvas><table id="t"></table>
<script>
const c=document.getElementById('c'),x=c.getContext('2d');let timer=null;
function draw(r){const im=new Image();im.onload=()=>{c.width=im.width;c.height=im.height;x.drawImage(im,0,0);
 if(r&&r.part){x.strokeStyle='#08f';x.strokeRect(...r.part)}
 if(r&&r.checks)for(const k of r.checks){x.strokeStyle=k.pass?'#0f0':'#f00';x.lineWidth=2;x.strokeRect(...k.roi);
  if(k.cell){x.strokeStyle='#ff0';x.strokeRect(...k.cell)}}};im.src='/last.jpg?'+Date.now()}
function show(r){const s=document.getElementById('st');s.textContent=(r.state||'').toUpperCase();s.className=r.state;
 document.getElementById('rc').textContent=(r.recipe||'')+'  '+((r.ms_capture||0)+(r.ms_inspect||0)).toFixed(0)+' ms';
 let h='';for(const k of (r.checks||[]))h+='<tr><td class="'+(k.pass?'pass':'fail')+'">'+(k.pass?'OK':'NG')+'</td><td>'+k.name+'</td><td>'+(+k.value).toFixed(3)+'</td></tr>';
 document.getElementById('t').innerHTML=h;draw(r)}
async function go(u,img){if(img){await fetch(u);draw(null);return}show(await (await fetch(u)).json())}
function live(on){fetch('/live?on='+on);clearInterval(timer);if(on)timer=setInterval(async()=>show(await (await fetch('/result')).json()),400)}
</script></body></html>)html";

void setupRoutes() {
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });

  server.on("/status", []() {
    JsonDocument d;
    d["recipe"] = activeId;
    d["live"] = live;
    d["teaching"] = teaching;
    d["teach_n"] = teachN;
    d["servo"] = servoAngle;
    d["pass"] = nPass;
    d["fail"] = nFail;
    d["cam"] = camCmd("PING");
    d["oled"] = oledOk;
    d["heap"] = ESP.getFreeHeap();
    d["psram"] = ESP.getFreePsram();
    d["ip"] = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    sendJson(d);
  });

  // Capture a fresh frame (and keep it for /measure, /locate)
  server.on("/frame.jpg", []() {
    if (!camCapture() || !decodeFrame()) return sendErr("capture failed", 500);
    server.send_P(200, "image/jpeg", (const char *)jpgBuf, jpgLen);
  });
  server.on("/last.jpg", []() {
    if (!jpgLen) return sendErr("no frame", 404);
    server.send_P(200, "image/jpeg", (const char *)jpgBuf, jpgLen);
  });

  // Camera settings: /camera?aec=300&flash=40&gain=0
  server.on("/camera", []() {
    JsonDocument d;
    for (int i = 0; i < server.args(); i++) d[server.argName(i)] = camCmd("SET " + server.argName(i) + " " + server.arg(i));
    sendJson(d);
  });

  server.on("/servo", []() {
    moveServo(server.arg("angle").toInt());
    server.send(200, "application/json", "{\"servo\":" + String(servoAngle) + "}");
  });

  // Stats of a region on the current frame. Used by the agent to design checks.
  server.on("/measure", []() {
    if (!frameValid) return sendErr("no frame, call /frame.jpg first");
    Rect r = { 0, 0, W, H };
    if (server.hasArg("w")) r = clampRect({ server.arg("x").toInt(), server.arg("y").toInt(), server.arg("w").toInt(), server.arg("h").toInt() });
    double sg = 0, sg2 = 0, sr = 0, sgr = 0, sb = 0, hx = 0, hy = 0, ss = 0, sv = 0, ge = 0;
    uint32_t n = r.w * r.h;
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) {
        int p = y * W + x;
        uint8_t R, G, B;
        float h, s, v;
        pixRGB(p, R, G, B);
        rgb2hsv(R, G, B, h, s, v);
        sg += grayBuf[p]; sg2 += grayBuf[p] * grayBuf[p];
        sr += R; sgr += G; sb += B;
        hx += s * cosf(h * PI / 180); hy += s * sinf(h * PI / 180);
        ss += s; sv += v;
        if (x + 1 < r.x + r.w && y + 1 < r.y + r.h) ge += abs(grayBuf[p + 1] - grayBuf[p]) + abs(grayBuf[p + W] - grayBuf[p]);
      }
    JsonDocument d;
    float mean = sg / n;
    float hue = atan2f(hy, hx) * 180 / PI;
    if (hue < 0) hue += 360;
    JsonArray ra = d["roi"].to<JsonArray>();
    ra.add(r.x); ra.add(r.y); ra.add(r.w); ra.add(r.h);
    d["size"][0] = W; d["size"][1] = H;
    d["gray_mean"] = mean;
    d["gray_std"] = sqrt(max(0.0, sg2 / n - mean * mean));
    d["rgb"][0] = sr / n; d["rgb"][1] = sgr / n; d["rgb"][2] = sb / n;
    d["hue"] = hue;
    d["sat"] = ss / n;
    d["val"] = sv / n;
    d["edges"] = ge / n;
    d["otsu"] = otsu(r);
    sendJson(d);
  });

  server.on("/locate", []() {
    if (!frameValid) return sendErr("no frame, call /frame.jpg first");
    Loc L = locate(server.hasArg("mode") ? server.arg("mode").c_str() : "dark_on_light", server.arg("thr").toInt());
    JsonDocument d;
    d["found"] = L.found;
    d["bbox"][0] = L.x; d["bbox"][1] = L.y; d["bbox"][2] = L.w; d["bbox"][3] = L.h;
    d["area"] = L.area;
    d["thr"] = L.thr;
    sendJson(d);
  });

  // Recipes
  server.on("/recipes", []() {
    JsonDocument d;
    d["active"] = activeId;
    JsonArray a = d["recipes"].to<JsonArray>();
    File dir = LittleFS.open("/r");
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      if (n.endsWith(".json")) a.add(n.substring(0, n.length() - 5));
    }
    sendJson(d);
  });
  server.on("/recipe", HTTP_GET, []() {
    String id = server.hasArg("id") ? server.arg("id") : activeId;
    File f = LittleFS.open(recipePath(id), "r");
    if (!f) return sendErr("not found", 404);
    server.streamFile(f, "application/json");
    f.close();
  });
  server.on("/recipe", HTTP_POST, []() {
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) return sendErr("bad json");
    String id = d["id"] | "";
    if (!id.length()) return sendErr("recipe needs an id");
    File f = LittleFS.open(recipePath(id), "w");
    serializeJson(d, f);
    f.close();
    if (server.arg("activate") == "1" || id == activeId) activate(id);
    server.send(200, "application/json", "{\"saved\":\"" + id + "\"}");
  });
  server.on("/activate", []() {
    if (!activate(server.arg("id"))) return sendErr("not found", 404);
    server.send(200, "application/json", "{\"active\":\"" + activeId + "\"}");
  });
  server.on("/delete", []() {
    LittleFS.remove(recipePath(server.arg("id")));
    server.send(200, "application/json", "{\"deleted\":true}");
  });

  // Inspect live camera
  server.on("/inspect", []() {
    JsonDocument res;
    cycle(res);
    sendJson(res);
  });
  // Inspect an uploaded image (base64 JPEG in body). Lets the agent self-test on a dataset.
  server.on("/inspect_image", HTTP_POST, []() {
    const String &b = server.arg("plain");
    size_t outLen = 0;
    if (mbedtls_base64_decode(jpgBuf, JPG_MAX, &outLen, (const uint8_t *)b.c_str(), b.length()) != 0) return sendErr("bad base64");
    jpgLen = outLen;
    if (!decodeFrame()) return sendErr("bad jpeg");
    JsonDocument res;
    inspectFrame(res, 0);
    sendJson(res);
  });
  server.on("/result", []() { server.send(200, "application/json", lastResult); });
  server.on("/live", []() {
    live = server.arg("on") == "1";
    server.send(200, "application/json", String("{\"live\":") + (live ? "true" : "false") + "}");
  });

  // Teach good-only anomaly model: /teach/start?check=NAME, /teach/add (or BOOT), /teach/finish
  server.on("/teach/start", []() {
    if (!haveRecipe) return sendErr("no active recipe");
    teachCheck = server.arg("check");
    JsonObject chk;
    for (JsonObject c : recipe["checks"].as<JsonArray>())
      if (teachCheck == (c["name"] | "")) chk = c;
    if (chk.isNull() || strcmp(chk["type"] | "", "anomaly") != 0) return sendErr("no anomaly check with that name");
    teachFeat = (chk["cells"][0] | 8) * (chk["cells"][1] | 6) * 2;
    free(teachSum); free(teachSq);
    teachSum = (float *)calloc(teachFeat, sizeof(float));
    teachSq = (float *)calloc(teachFeat, sizeof(float));
    teachN = 0;
    teaching = true;
    live = false;
    rgb(30, 0, 40);
    showMessage("TEACH 0", "place GOOD part, press BOOT");
    server.send(200, "application/json", "{\"teaching\":true}");
  });
  server.on("/teach/add", []() {
    String msg;
    if (!teachAdd(msg)) return sendErr(msg);
    server.send(200, "application/json", "{\"n\":" + String(teachN) + "}");
  });
  server.on("/teach/finish", []() {
    if (!teaching || teachN < 3) return sendErr("need at least 3 samples");
    for (JsonObject c : recipe["checks"].as<JsonArray>()) {
      if (teachCheck != (c["name"] | "")) continue;
      c.remove("model");
      JsonArray m = c["model"]["m"].to<JsonArray>(), s = c["model"]["s"].to<JsonArray>();
      for (int k = 0; k < teachFeat; k++) {
        float mean = teachSum[k] / teachN;
        m.add(mean);
        s.add(sqrtf(max(0.0f, teachSq[k] / teachN - mean * mean)));
      }
      c["model"]["n"] = teachN;
      if (server.hasArg("z")) c["z"] = server.arg("z").toFloat();
    }
    saveActive();
    teaching = false;
    showMessage("TAUGHT", String(teachN) + " good samples");
    server.send(200, "application/json", "{\"taught\":" + String(teachN) + "}");
  });
}

bool teachAdd(String &msg) {
  if (!teaching) { msg = "not teaching"; return false; }
  if (!camCapture() || !decodeFrame()) { msg = "capture failed"; return false; }
  int dx = 0, dy = 0;
  JsonObject lc = recipe["locate"];
  const char *mode = lc["mode"] | "none";
  if (strcmp(mode, "none") != 0) {
    Loc L = locate(mode, lc["thr"] | 0);
    if (!L.found) { msg = "no part found"; return false; }
    if (!lc["ref"].isNull()) { dx = L.x - lc["ref"][0].as<int>(); dy = L.y - lc["ref"][1].as<int>(); }
  }
  JsonObject chk;
  for (JsonObject c : recipe["checks"].as<JsonArray>())
    if (teachCheck == (c["name"] | "")) chk = c;
  Rect r = readRoi(chk["roi"], dx, dy);
  float *f = (float *)malloc(teachFeat * sizeof(float));
  cellFeatures(r, chk["cells"][0] | 8, chk["cells"][1] | 6, f);
  for (int k = 0; k < teachFeat; k++) {
    teachSum[k] += f[k];
    teachSq[k] += f[k] * f[k];
  }
  free(f);
  teachN++;
  showMessage("TEACH " + String(teachN), "next GOOD part, press BOOT");
  return true;
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nSpecCam S3 engine");

  pinMode(OUT_PASS, OUTPUT);
  pinMode(OUT_FAIL, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  rgb(0, 0, 20);

  jpgBuf = (uint8_t *)ps_malloc(JPG_MAX);
  rgbBuf = (uint8_t *)ps_malloc(MAXW * MAXH * 2);
  grayBuf = (uint8_t *)ps_malloc(MAXW * MAXH);
  markBuf = (uint8_t *)ps_malloc(MAXW * MAXH);
  stackBuf = (int32_t *)ps_malloc(MAXW * MAXH * sizeof(int32_t));
  if (!jpgBuf || !rgbBuf || !grayBuf || !markBuf || !stackBuf) {
    Serial.println("PSRAM allocation failed: set Tools > PSRAM > OPI PSRAM");
    rgb(80, 0, 0);
    while (true) delay(1000);
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.beginTransmission(0x3C);
  oledOk = Wire.endTransmission() == 0 && oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  showMessage("BOOT", "starting...");

  ledcAttach(SERVO_PIN, 50, 14);
  moveServo(90);

  Serial1.setRxBufferSize(16384);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  delay(200);
  String ping = camCmd("PING");
  Serial.println("CAM: " + ping);

  LittleFS.begin(true);
  LittleFS.mkdir("/r");
  prefs.begin("speccam", false);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(250);
  String ip;
  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP().toString();
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SpecCam", "12345678");
    ip = WiFi.softAPIP().toString();
  }
  MDNS.begin("speccam");
  setupRoutes();
  server.begin();
  Serial.println("http://" + ip + "  (or http://speccam.local)");

  String last = prefs.getString("active", "");
  if (last.length() && activate(last)) Serial.println("Active recipe: " + last);
  else showMessage(ping.startsWith("OK") ? "NO RECIPE" : "CAM ?", ip);
}

void loop() {
  server.handleClient();

  // BOOT button: teach sample when teaching, otherwise inspect once
  static bool lastBtn = HIGH;
  bool b = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && b == LOW) {
    if (teaching) {
      String msg;
      if (!teachAdd(msg)) showMessage("TEACH ERR", msg);
    } else {
      JsonDocument res;
      cycle(res);
    }
  }
  lastBtn = b;

  if (live && !teaching && millis() - lastLive > 150) {
    lastLive = millis();
    JsonDocument res;
    cycle(res);
  }

  // Serial: 'i' inspect, 'p' ping camera
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'i') {
      JsonDocument res;
      cycle(res);
      serializeJsonPretty(res, Serial);
      Serial.println();
    } else if (c == 'p') {
      Serial.println(camCmd("PING"));
    }
  }
  delay(2);
}
