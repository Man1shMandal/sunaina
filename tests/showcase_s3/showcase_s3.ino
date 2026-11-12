/*
  Project Sunaina - showcase (ESP32-S3)
  Servo web control + OLED animations + synced RGB LED, all from one web page.

  Board: "ESP32S3 Dev Module" (pick it manually!). USB CDC On Boot: Enabled. Serial 115200.
  Libraries: Adafruit SH110X, Adafruit GFX  (Adafruit SSD1306 if you switch below)

  Wiring:
    Servo signal -> GPIO4,  servo + -> external 5-6 V 2 A+,  servo - -> supply GND + S3 GND
    OLED VCC -> 3V3, GND -> GND, SDA -> GPIO8, SCL -> GPIO9
    RGB LED: onboard, GPIO48

  Wi-Fi: joins hotspot "Sunaina-Servo" (password sunaina123) -> http://192.168.4.1
         or fill HOME_SSID / HOME_PASS. http://sunaina.local also works on most laptops.

  How it runs:
    core 1 (loop):  web server + servo motion (never blocks)
    core 0 (task):  OLED frames at ~30 fps, picks the LED colour
    The LED itself is written from loop() on core 1 (same as the board test, where it works).
    If the OLED is missing, servo and web keep working; the LED blinks white slowly.

  Display modes (web page): Auto (cycles all scenes), Web, Drop, Glitch, Halftone,
  THWIP, Zoom, Gauge (live servo needle + angle), Off.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <esp_system.h>

// The RGB LED only worked with this exact board entry (see docs/agent/STATE.md).
#if !defined(ARDUINO_ESP32S3_DEV)
  #error "Wrong board: pick Tools > Board > esp32 > ESP32S3 Dev Module (not the auto-detected board, not Arduino Nano ESP32)"
#endif

#define USE_SH1106 true   // false = SSD1306

#if USE_SH1106
  #include <Adafruit_SH110X.h>
  Adafruit_SH1106G oled(128, 64, &Wire, -1, 400000, 400000);
  #define ON  SH110X_WHITE
  #define OFF SH110X_BLACK
  #define INV SH110X_INVERSE
#else
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 oled(128, 64, &Wire, -1, 400000, 400000);
  #define ON  SSD1306_WHITE
  #define OFF SSD1306_BLACK
  #define INV SSD1306_INVERSE
#endif

// ---------- Wi-Fi ----------
// Home Wi-Fi lives in wifi_secrets.h (git-ignored). Copy wifi_secrets.example.h to start.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef HOME_WIFI_SSID
#define HOME_WIFI_SSID ""
#define HOME_WIFI_PASS ""
#endif
const char *HOME_SSID = HOME_WIFI_SSID;            // leave empty to use the hotspot
const char *HOME_PASS = HOME_WIFI_PASS;
const char *AP_SSID   = "Sunaina-Servo";
const char *AP_PASS   = "sunaina123";

// ---------- pins ----------
#define SERVO_PIN 4
#define I2C_SDA 8
#define I2C_SCL 9
volatile int rgbPin = 48;   // some S3 boards use 38: switch on the page or type 8 in Serial

// ---------- camera arm positions (measured on the rig, servo degrees) ----------
#define VIEW_FRONT 0      // camera level with the product, looking at the front face
#define VIEW_TOP   63     // arm up, camera looking straight down
#define VIEW_BACK  120    // camera level with the product, looking at the back face
#define DEFAULT_SPEED 120 // deg/s, mid speed: about 0.5 s between views

WebServer server(80);
Preferences prefs;

// =====================================================================
// Servo (core 1)
// =====================================================================
int   minUs = 500, maxUs = 2500;    // MG995 full ~180 deg
volatile float pos = VIEW_TOP;     // boot in the top view
volatile int   target = VIEW_TOP;
float speedDps = DEFAULT_SPEED;
volatile bool looping = false;
int   loopA = 0, loopB = 180, pauseMs = 300;
volatile int sweepStage = 0;
volatile bool attached = false;
volatile int rawUs = -1;
int   lastUs = -1;
unsigned long lastMicros = 0, holdUntil = 0;
const int SWEEP[] = {0, 180, 0, 90};

void writeUs(int us) {
  us = constrain(us, 300, 2700);   // hard safety limits
  if (us == lastUs) return;
  lastUs = us;
  ledcWrite(SERVO_PIN, (uint32_t)us * 16383 / 20000);
}
int angleToUs(float a) { return minUs + (long)(constrain(a, 0.0f, 180.0f) * (maxUs - minUs) / 180.0f); }
void attachServo() { if (attached) return; ledcAttach(SERVO_PIN, 50, 14); attached = true; lastUs = -1; }
void releaseServo() {
  looping = false; sweepStage = 0;
  if (!attached) return;
  ledcDetach(SERVO_PIN); pinMode(SERVO_PIN, OUTPUT); digitalWrite(SERVO_PIN, LOW);
  attached = false;
}
void goTo(int a) { attachServo(); rawUs = -1; target = constrain(a, 0, 180); }
void stopHere() { looping = false; sweepStage = 0; target = round(pos); }
void startLoop() { goTo(loopA); sweepStage = 0; looping = true; }
void startSweep() { looping = false; goTo(SWEEP[0]); sweepStage = 1; }

void updateMotion() {
  unsigned long now = micros();
  float dt = (now - lastMicros) / 1e6;
  lastMicros = now;
  if (!attached || rawUs >= 0 || millis() < holdUntil) return;
  float p = pos, step = speedDps * dt;
  if (fabs(target - p) <= step) p = target; else p += (target > p) ? step : -step;
  pos = p;
  writeUs(angleToUs(p));
  if (p != target) return;
  if (looping) { target = (target == loopB) ? loopA : loopB; holdUntil = millis() + pauseMs; }
  else if (sweepStage) {
    if (sweepStage < 4) { target = SWEEP[sweepStage]; sweepStage = sweepStage + 1; holdUntil = millis() + 300; }
    else sweepStage = 0;
  }
}

const char *servoMode() {
  if (!attached) return "LIMP";
  if (rawUs >= 0) return "RAW";
  if (looping) return "LOOP";
  if (sweepStage) return "SWEEP";
  return (fabs(pos - target) < 0.5) ? "HOLD" : "MOVE";
}

// =====================================================================
// OLED + RGB (core 0 task)
// =====================================================================
enum { D_AUTO = 0, D_WEB, D_DROP, D_GLITCH, D_HALF, D_THWIP, D_ZOOM, D_GAUGE, D_OFF };
volatile int dispMode = D_AUTO;
enum { RGB_SYNC = 0, RGB_MANUAL, RGB_OFF };
volatile int rgbMode = RGB_SYNC;     // sync = follows the animation
volatile int rgbMax = 150;
volatile uint8_t manR = 255, manG = 0, manB = 60;
volatile uint8_t ledR = 0, ledG = 0, ledB = 0;
volatile bool ledDirty = true;
volatile bool ledTestReq = false;
volatile int  ledPinReq = -1;
volatile bool oledOk = false;
volatile bool flipOled = true;       // OLED mounted upside down -> rotate 180 deg
volatile int curScene = 0;           // what is on screen now (0-5 = scenes, 6 = gauge)

const int W = 128, H = 64, CX = 64, CY = 32;
bool screenInverted = false;

// Called from the display task: only records the colour. loop() sends it to the LED.
void rgb(int r, int g, int b) {
  if (rgbMode == RGB_MANUAL) { r = manR; g = manG; b = manB; }
  int m = (rgbMode == RGB_OFF) ? 0 : rgbMax;
  uint8_t nr = constrain(r, 0, 255) * m / 255, ng = constrain(g, 0, 255) * m / 255, nb = constrain(b, 0, 255) * m / 255;
  if (nr != ledR || ng != ledG || nb != ledB) { ledR = nr; ledG = ng; ledB = nb; ledDirty = true; }
}

// Same colour sequence as the ESPS3 board test, from loop() on core 1.
void ledTest() {
  Serial.printf("LED test on GPIO%d: RED, GREEN, BLUE, WHITE\n", (int)rgbPin);
  rgbLedWrite(rgbPin, 60, 0, 0);  delay(500);
  rgbLedWrite(rgbPin, 0, 60, 0);  delay(500);
  rgbLedWrite(rgbPin, 0, 0, 60);  delay(500);
  rgbLedWrite(rgbPin, 50, 50, 50); delay(500);
  rgbLedWrite(rgbPin, 0, 0, 0);
  Serial.println("Colours did not change? Switch the LED pin (48 / 38) and test again.");
  ledDirty = true;
}

void setLedPin(int p) {
  rgbLedWrite(rgbPin, 0, 0, 0);   // turn the old one off
  rgbPin = p;
  Serial.printf("RGB pin is now GPIO%d\n", p);
  ledDirty = true;
}

// Called from loop() on core 1.
void pushLed() {
  if (ledPinReq >= 0) { setLedPin(ledPinReq); ledPinReq = -1; }
  if (ledTestReq) { ledTestReq = false; ledTest(); }
  if (!ledDirty) return;
  ledDirty = false;
  rgbLedWrite(rgbPin, ledR, ledG, ledB);
}

void thickLine(int x0, int y0, int x1, int y1, uint16_t c, bool thick) {
  oled.drawLine(x0, y0, x1, y1, c);
  if (thick) { oled.drawLine(x0 + 1, y0, x1 + 1, y1, c); oled.drawLine(x0, y0 + 1, x1, y1 + 1, c); }
}

const int8_t LEGS[4][6] = {
  {2, -4, 10, -12,  8, -22}, {3, -2, 14, -6, 16, -16},
  {3,  1, 14,   4, 16,  14}, {2,  3, 10, 10,  8,  22},
};

void drawSpider(int cx, int cy, float s, uint16_t c) {
  bool thick = s >= 1.0;
  oled.fillCircle(cx, cy + 6 * s, 6 * s, c);
  oled.fillTriangle(cx - 4 * s, cy + 9 * s, cx + 4 * s, cy + 9 * s, cx, cy + 18 * s, c);
  oled.fillCircle(cx, cy - 2 * s, 4.5 * s, c);
  oled.fillCircle(cx, cy - 8 * s, 3 * s, c);
  for (int i = 0; i < 4; i++)
    for (int side = -1; side <= 1; side += 2) {
      int hx = cx + side * LEGS[i][0] * s, hy = cy + LEGS[i][1] * s;
      int kx = cx + side * LEGS[i][2] * s, ky = cy + LEGS[i][3] * s;
      int tx = cx + side * LEGS[i][4] * s, ty = cy + LEGS[i][5] * s;
      thickLine(hx, hy, kx, ky, c, thick);
      thickLine(kx, ky, tx, ty, c, thick);
    }
}

void glitchBands(int amount) {
  if (amount <= 0) return;
  uint8_t *b = oled.getBuffer(), tmp[W];
  for (int p = 0; p < 8; p++) {
    if (random(100) > 45) continue;
    int d = random(-amount, amount + 1);
    memcpy(tmp, b + p * W, W);
    for (int x = 0; x < W; x++) b[p * W + x] = tmp[(x - d + W) % W];
  }
}

void invertBuffer() {
  uint8_t *b = oled.getBuffer();
  for (int i = 0; i < W * H / 8; i++) b[i] ^= 0xFF;
  screenInverted = true;
}

void cornerWeb() {
  for (int i = 0; i <= 4; i++) { float a = i * (PI / 2) / 4; oled.drawLine(0, 0, cos(a) * 34, sin(a) * 34, ON); }
  for (int r = 9; r <= 30; r += 7)
    for (int i = 0; i < 4; i++) {
      float a0 = i * (PI / 2) / 4, a1 = (i + 1) * (PI / 2) / 4, am = (a0 + a1) / 2;
      int xm = cos(am) * r * 0.85, ym = sin(am) * r * 0.85;
      oled.drawLine(cos(a0) * r, sin(a0) * r, xm, ym, ON);
      oled.drawLine(xm, ym, cos(a1) * r, sin(a1) * r, ON);
    }
}

// ----- scenes -----
void sceneWeb(int f) {
  const int SP = 12;
  float grow = min(1.0f, f / 18.0f), spin = f * 0.02;
  for (int i = 0; i < SP; i++) {
    float a = i * 2 * PI / SP + spin;
    oled.drawLine(CX, CY, CX + cos(a) * 80 * grow, CY + sin(a) * 80 * grow, ON);
  }
  int rings = (f - 18) / 4;
  for (int k = 1; k <= min(rings, 9); k++) {
    int r = k * 8;
    for (int i = 0; i < SP; i++) {
      float a0 = i * 2 * PI / SP + spin, a1 = (i + 1) * 2 * PI / SP + spin, am = (a0 + a1) / 2;
      int xm = CX + cos(am) * r * 0.82, ym = CY + sin(am) * r * 0.82;
      oled.drawLine(CX + cos(a0) * r, CY + sin(a0) * r, xm, ym, ON);
      oled.drawLine(xm, ym, CX + cos(a1) * r, CY + sin(a1) * r, ON);
    }
  }
  if (f > 50) {
    float s = min(0.6f, (f - 50) * 0.06f);
    oled.fillCircle(CX, CY, 14 * s + 2, OFF);
    drawSpider(CX, CY, s, ON);
  }
}

void sceneDrop(int f) {
  const float s = 0.6; const int tgt = 34;
  float y = f < 10 ? -15 + (tgt + 15) * f / 10.0 : tgt + 12 * exp(-(f - 10) / 12.0) * cos((f - 10) * 0.45);
  int x = CX + 5 * sin(f * 0.12);
  cornerWeb();
  oled.drawLine(CX, 0, x, y - 8 * s, ON);
  drawSpider(x, y, s, ON);
}

void sceneGlitch(int f) {
  int intensity = max(0, 24 - (int)(f * 0.6));
  if (intensity > 2) {
    int d = random(2, 5);
    drawSpider(CX - d, CY, 1.3, ON);
    drawSpider(CX + d, CY + random(-1, 2), 1.3, INV);
  } else drawSpider(CX, CY, 1.3, ON);
  glitchBands(intensity);
  if (intensity > 6 && random(8) == 0) invertBuffer();
  oled.drawFastHLine(0, (f * 4) % H, W, INV);
}

void sceneHalftone(int f) {
  float wave = f * 2.4;
  for (int gx = 4; gx < W; gx += 8)
    for (int gy = 4; gy < H; gy += 8) {
      float d = sqrt((gx - CX) * (gx - CX) + (gy - CY) * (gy - CY));
      float v = 1.0 - fabs(d - wave) / 18.0;
      int r = (d < wave) ? 1 : 0;
      if (v > 0) r = max(r, (int)round(v * 4));
      if (r) oled.fillCircle(gx, gy, r, ON);
    }
  if (f > 12) {
    int rad = min(29, (f - 12) * 3);
    oled.fillCircle(CX, CY, rad + 2, OFF);
    oled.fillCircle(CX, CY, rad, ON);
    if (rad > 20) drawSpider(CX, CY, 1.15, OFF);
  }
}

void sceneThwip(int f) {
  int sx = f < 22 ? random(-3, 4) : 0, sy = f < 22 ? random(-2, 3) : 0;
  float grow = min(1.0f, f / 6.0f);
  const int N = 18;
  for (int i = 0; i < N; i++) {
    float a0 = i * 2 * PI / N + f * 0.03, a1 = (i + 1) * 2 * PI / N + f * 0.03;
    float r0 = (i % 2 ? 46 : 74) * grow, r1 = ((i + 1) % 2 ? 46 : 74) * grow;
    oled.fillTriangle(CX + sx, CY + sy, CX + sx + cos(a0) * r0, CY + sy + sin(a0) * r0,
                      CX + sx + cos(a1) * r1, CY + sy + sin(a1) * r1, ON);
  }
  int size = f < 3 ? 1 : f < 6 ? 2 : 3;
  const char *t = "THWIP!";
  oled.setTextSize(size);
  oled.setTextColor(OFF);
  oled.setCursor(CX - (int)strlen(t) * 3 * size + sx, CY - 4 * size + sy);
  oled.print(t);
}

void sceneZoom(int f) {
  for (int i = 0; i < 20; i++) {
    float a = i * 2 * PI / 20 + 0.15, r0 = fmod(f * 7 + i * 13, 75), r1 = r0 + 6 + r0 / 6;
    oled.drawLine(CX + cos(a) * r0, CY + sin(a) * r0, CX + cos(a) * r1, CY + sin(a) * r1, ON);
  }
  float s = 0.2 * pow(1.075, f);
  drawSpider(CX, CY + 2 * s, s, ON);
  if (f > 40) invertBuffer();
}

// Live servo gauge: needle = where it is, dot = where it is going.
void sceneGauge(int f) {
  const int gx = 64, gy = 60, R = 44;
  float p = pos; int tg = target;
  for (int d = 0; d <= 180; d += 5) {
    float t = PI * (1 - d / 180.0);
    int x = gx + cos(t) * R, y = gy - sin(t) * R;
    oled.drawPixel(x, y, ON);
    if (d % 30 == 0) oled.drawLine(x, y, gx + cos(t) * (R - 6), gy - sin(t) * (R - 6), ON);
  }
  float t = PI * (1 - p / 180.0);
  thickLine(gx, gy, gx + cos(t) * (R - 4), gy - sin(t) * (R - 4), ON, true);
  oled.fillCircle(gx, gy, 3, ON);
  float tt = PI * (1 - tg / 180.0);
  oled.fillCircle(gx + cos(tt) * (R + 4), gy - sin(tt) * (R + 4), 2, ON);
  oled.setTextColor(ON);
  oled.setTextSize(2);
  oled.setCursor(0, 0);
  oled.printf("%d", (int)round(p));
  oled.setTextSize(1);
  const char *m = servoMode();
  oled.setCursor(W - strlen(m) * 6, 0);
  oled.print(m);
  oled.setCursor(W - 30, 10);
  oled.printf("%3d/s", (int)speedDps);
}

typedef void (*SceneFn)(int);
struct Scene { SceneFn fn; int frames; };
Scene scenes[] = {
  {sceneWeb, 70}, {sceneDrop, 70}, {sceneGlitch, 60},
  {sceneHalftone, 60}, {sceneThwip, 45}, {sceneZoom, 46},
};
const int NSCENES = 6;

void ledFor(int scene, int f) {
  if (screenInverted) { rgb(255, 255, 255); return; }
  switch (scene) {
    case 0: { int v = min(255, 60 + f * 4); rgb(0, v / 6, v); break; }
    case 1: {
      float b = f < 10 ? f / 10.0 : 0.5 + 0.5 * cos((f - 10) * 0.45) * exp(-(f - 10) / 12.0);
      rgb(60 + 195 * b, 0, 10); break;
    }
    case 2: {
      int intensity = max(0, 24 - (int)(f * 0.6));
      if (intensity > 2 && random(3) == 0) rgb(random(2) ? 255 : 0, 0, random(2) ? 255 : 0);
      else rgb(200, 0, 0);
      break;
    }
    case 3: { float p = 0.5 + 0.5 * sin(f * 0.4); rgb(255 * p, 0, 255 * (1 - p)); break; }
    case 4: { if (f < 6) rgb(255, 255, 255); else if (f < 22) rgb(255, random(120, 220), 0); else rgb(255, 180, 0); break; }
    case 5: { int v = min(255, f * 6); rgb(255, v / 2, v / 2); break; }
    case 6: {   // gauge: blue at 0 deg -> red at 180 deg, brighter while moving
      float a = pos / 180.0;
      float k = (fabs(pos - target) > 0.5 || looping || sweepStage) ? 1.0 : 0.35;
      rgb(255 * a * k, 0, 255 * (1 - a) * k); break;
    }
  }
}

bool probeOled() {
  Wire.beginTransmission(0x3C);
  return Wire.endTransmission() == 0;
}

bool initOled() {
  if (!probeOled()) return false;
#if USE_SH1106
  return oled.begin(0x3C, true);
#else
  return oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
#endif
}

void displayTask(void *) {
  Wire.begin(I2C_SDA, I2C_SCL);

  int f = 0, scene = 0, lastMode = -1;
  for (;;) {
    unsigned long t0 = millis();
    if (!oledOk) {                       // no OLED: keep trying, blink white
      oledOk = initOled();
      if (!oledOk) {
        rgb(120, 120, 120); vTaskDelay(pdMS_TO_TICKS(500));
        rgb(0, 0, 0);       vTaskDelay(pdMS_TO_TICKS(1500));
        continue;
      }
      Serial.println("OLED ready");
    }

    static int rot = -1;
    int wantRot = flipOled ? 2 : 0;
    if (wantRot != rot) { rot = wantRot; oled.setRotation(rot); }

    int mode = dispMode;
    if (mode != lastMode) { f = 0; scene = (mode >= D_WEB && mode <= D_ZOOM) ? mode - D_WEB : 0; lastMode = mode; }

    if (mode == D_OFF) {
      oled.clearDisplay(); oled.display(); rgb(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    screenInverted = false;
    oled.clearDisplay();
    if (mode == D_GAUGE) {
      curScene = 6;
      sceneGauge(f);
      ledFor(6, f);
    } else {
      if (f >= scenes[scene].frames) {   // scene finished
        f = 0;
        if (mode == D_AUTO) {
          oled.fillScreen(ON); oled.display(); rgb(255, 255, 255);   // flash between scenes
          vTaskDelay(pdMS_TO_TICKS(40));
          oled.clearDisplay();
          scene = (scene + 1) % NSCENES;
        }
      }
      curScene = scene;
      scenes[scene].fn(f);
      ledFor(scene, f);
    }
    oled.display();
    f++;
    // ~30 fps, but always sleep at least 1 tick so core 0's watchdog stays happy
    long wait = 30 - (long)(millis() - t0);
    TickType_t ticks = pdMS_TO_TICKS(max(wait, 1L));
    vTaskDelay(ticks ? ticks : 1);
  }
}

// =====================================================================
// Settings
// =====================================================================
void loadSettings() {
  prefs.begin("showcase", true);
  minUs = prefs.getInt("min", 500);
  maxUs = prefs.getInt("max", 2500);
  speedDps = prefs.getFloat("spd", DEFAULT_SPEED);   // new key: old saved 90 deg/s is ignored
  pauseMs = prefs.getInt("pause", 300);
  loopA = prefs.getInt("la", 0);
  loopB = prefs.getInt("lb", 180);
  dispMode = prefs.getInt("disp", D_AUTO);
  rgbMode = prefs.getInt("rgbm", RGB_SYNC);
  manR = prefs.getUChar("mr", 255); manG = prefs.getUChar("mg", 0); manB = prefs.getUChar("mb", 60);
  rgbMax = prefs.getInt("bright", 150);
  rgbPin = prefs.getInt("lpin", 48);
  flipOled = prefs.getBool("flip", true);
  prefs.end();
}

void saveSettings() {
  prefs.begin("showcase", false);
  prefs.putInt("min", minUs); prefs.putInt("max", maxUs);
  prefs.putFloat("spd", speedDps); prefs.putInt("pause", pauseMs);
  prefs.putInt("la", loopA); prefs.putInt("lb", loopB);
  prefs.putInt("disp", dispMode); prefs.putInt("rgbm", rgbMode);
  prefs.putUChar("mr", manR); prefs.putUChar("mg", manG); prefs.putUChar("mb", manB); prefs.putInt("bright", rgbMax);
  prefs.putInt("lpin", rgbPin);
  prefs.putBool("flip", flipOled);
  prefs.end();
}

// =====================================================================
// Web page
// =====================================================================
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sunaina Control</title>
<style>
:root{--bg:#f3f4f2;--card:#fff;--fg:#1c2124;--mute:#5f686d;--line:#d5d9d8;--acc:#d4520f;--btn:#eceeed}
@media (prefers-color-scheme:dark){:root{--bg:#131718;--card:#1b2022;--fg:#e9ecec;--mute:#9aa4a7;--line:#2f3739;--acc:#ff7a3d;--btn:#262c2e}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:520px;margin:auto;padding:16px}
h1{font-size:18px;margin:4px 0 12px}h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--mute);margin:0 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:6px}
.lbl{display:flex;justify-content:space-between;color:var(--mute);font-size:13px;margin:8px 0 4px}
.lbl b{color:var(--fg);font-variant-numeric:tabular-nums}
input[type=range]{width:100%;accent-color:var(--acc)}
input[type=number]{width:76px;padding:6px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg)}
button{flex:1;min-width:60px;padding:10px 6px;border:0;border-radius:8px;background:var(--btn);color:var(--fg);font:inherit;cursor:pointer}
button.on,button.pri{background:var(--acc);color:#fff}
#stat{font-size:13px;color:var(--mute);text-align:center;font-variant-numeric:tabular-nums}
svg{display:block;width:100%;max-width:300px;margin:0 auto}
</style></head><body><main>
<h1>Sunaina control</h1>

<div class="card">
  <svg viewBox="0 0 200 112">
    <path d="M10 100 A90 90 0 0 1 190 100" stroke-width="8" stroke-linecap="round" style="fill:none;stroke:var(--line)"/>
    <circle id="tgt" r="5" style="fill:var(--mute)"/>
    <line id="needle" x1="100" y1="100" x2="100" y2="14" stroke-width="4" stroke-linecap="round" style="stroke:var(--acc)"/>
    <circle cx="100" cy="100" r="6" style="fill:var(--fg)"/>
  </svg>
  <div id="stat">connecting...</div>
</div>

<div class="card">
  <h2>Servo</h2>
  <div class="lbl">Angle <b id="angv">90</b></div>
  <input id="ang" type="range" min="0" max="180" value="90">
  <div class="lbl">Speed (deg/s) <b id="spdv">120</b></div>
  <input id="spd" type="range" min="5" max="500" value="120">
  <div class="row" style="margin-top:10px">
    <button onclick="go(0)">Front 0</button><button onclick="go(63)">Top 63</button><button onclick="go(120)">Back 120</button>
  </div>
  <div class="row" style="margin-top:8px">
    <button class="pri" onclick="cmd('loop')">Loop</button>
    <button onclick="cmd('sweep')">Sweep</button>
    <button onclick="cmd('stop')">Stop</button>
    <button onclick="cmd('release')">Release</button>
  </div>
  <div class="lbl">Loop from / to (deg), pause at ends (ms)</div>
  <div class="row">
    <input id="la" type="number" min="0" max="180" value="0">
    <input id="lb" type="number" min="0" max="180" value="180">
    <input id="pz" type="number" min="0" max="5000" step="50" value="300">
    <button onclick="setp('loopA',$('la').value+'&loopB='+$('lb').value+'&pause='+$('pz').value)">Set</button>
  </div>
</div>

<div class="card">
  <h2>Display</h2>
  <div class="grid" id="modes"></div>
  <div class="row" style="margin-top:8px"><button id="flip" onclick="setp('flip',flipped?0:1)">Flip screen</button></div>
  <h2 style="margin-top:14px">RGB light</h2>
  <div class="row">
    <button id="r0" onclick="setp('rgbm',0)">Sync</button>
    <button id="r1" onclick="setp('rgbm',1)">Manual</button>
    <button id="r2" onclick="setp('rgbm',2)">Off</button>
    <input id="col" type="color" value="#ff003c" style="width:52px;height:40px;border:0;background:none;padding:0">
  </div>
  <div class="row" style="margin-top:8px">
    <button onclick="cmd('ledtest')">LED test</button>
    <button id="lp" onclick="setp('ledpin',ledPin==48?38:48)">LED pin 48</button>
  </div>
  <div class="lbl">Brightness <b id="brv">150</b></div>
  <input id="br" type="range" min="5" max="255" value="150">
</div>

<div class="card">
  <h2>Calibration</h2>
  <div class="lbl">Pulse for 0 and 180 deg (us)</div>
  <div class="row">
    <input id="mn" type="number" min="300" max="2700" step="10" value="500">
    <input id="mx" type="number" min="300" max="2700" step="10" value="2500">
    <button onclick="setp('min',$('mn').value+'&max='+$('mx').value)">Apply</button>
  </div>
  <div class="lbl">Raw pulse: slide slowly to each end, stop when it stops moving or buzzes <b id="rawv">1500</b></div>
  <input id="raw" type="range" min="300" max="2700" step="10" value="1500">
  <div class="row" style="margin-top:8px">
    <button onclick="$('mn').value=$('raw').value">Use as 0 deg</button>
    <button onclick="$('mx').value=$('raw').value">Use as 180 deg</button>
  </div>
  <div class="row" style="margin-top:10px"><button class="pri" onclick="cmd('save')">Save all settings</button></div>
</div>

<script>
const $=id=>document.getElementById(id);
const send=u=>fetch(u).catch(()=>{});
const setp=(k,v)=>send('/set?'+k+'='+v);
const cmd=c=>send('/cmd?do='+c);
const go=a=>{$('ang').value=a;$('angv').textContent=a;setp('angle',a)};
const MODES=['Auto','Web','Drop','Glitch','Halftone','THWIP','Zoom','Gauge','Off'];
MODES.forEach((m,i)=>{const b=document.createElement('button');b.textContent=m;b.id='m'+i;b.onclick=()=>setp('disp',i);$('modes').appendChild(b)});
function live(el,out,fn){let t=0;el.oninput=()=>{out.textContent=el.value;const n=Date.now();if(n-t>80){t=n;fn(el.value)}};el.onchange=()=>fn(el.value)}
live($('ang'),$('angv'),v=>setp('angle',v));
live($('spd'),$('spdv'),v=>setp('speed',v));
live($('br'),$('brv'),v=>setp('bright',v));
$('col').oninput=e=>setp('color',e.target.value.slice(1));
live($('raw'),$('rawv'),v=>send('/cmd?do=raw&us='+v));
function pt(a,r){const t=Math.PI*(1-a/180);return[100+r*Math.cos(t),100-r*Math.sin(t)]}
let first=true,flipped=true,ledPin=48;
async function poll(){
  try{
    const s=await (await fetch('/status')).json();
    const [x,y]=pt(s.pos,86);$('needle').setAttribute('x2',x);$('needle').setAttribute('y2',y);
    const [tx,ty]=pt(s.target,90);$('tgt').setAttribute('cx',tx);$('tgt').setAttribute('cy',ty);
    $('stat').textContent=`${s.pos.toFixed(1)} deg -> ${s.target} | ${s.us} us | ${s.mode} | OLED ${s.oled?'ok':'not found'}`;
    MODES.forEach((m,i)=>$('m'+i).classList.toggle('on',i==s.disp));
    [0,1,2].forEach(i=>$('r'+i).classList.toggle('on',i==s.rgbMode));
    flipped=s.flip;$('flip').classList.toggle('on',s.flip);
    ledPin=s.ledpin;$('lp').textContent='LED pin '+s.ledpin;
    if(first){first=false;
      $('spd').value=s.speed;$('spdv').textContent=Math.round(s.speed);
      $('ang').value=s.target;$('angv').textContent=s.target;
      $('la').value=s.loopA;$('lb').value=s.loopB;$('pz').value=s.pause;
      $('mn').value=s.min;$('mx').value=s.max;$('br').value=s.bright;$('brv').textContent=s.bright;$('col').value='#'+s.color;}
  }catch(e){$('stat').textContent='not connected';}
  setTimeout(poll,250);
}
poll();
</script></main></body></html>)HTML";

void handleStatus() {
  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"pos\":%.1f,\"target\":%d,\"speed\":%.0f,\"mode\":\"%s\",\"loopA\":%d,\"loopB\":%d,\"pause\":%d,"
    "\"min\":%d,\"max\":%d,\"us\":%d,\"disp\":%d,\"rgbMode\":%d,\"color\":\"%02x%02x%02x\",\"bright\":%d,\"oled\":%s,\"flip\":%s,\"ledpin\":%d}",
    (float)pos, (int)target, speedDps, servoMode(), loopA, loopB, pauseMs, minUs, maxUs, lastUs,
    (int)dispMode, (int)rgbMode, manR, manG, manB, (int)rgbMax, oledOk ? "true" : "false", flipOled ? "true" : "false", (int)rgbPin);
  server.send(200, "application/json", buf);
}

void handleSet() {
  if (server.hasArg("speed"))  speedDps = constrain(server.arg("speed").toFloat(), 1.0f, 600.0f);
  if (server.hasArg("loopA"))  loopA = constrain(server.arg("loopA").toInt(), 0, 180);
  if (server.hasArg("loopB"))  loopB = constrain(server.arg("loopB").toInt(), 0, 180);
  if (server.hasArg("pause"))  pauseMs = constrain(server.arg("pause").toInt(), 0, 5000);
  if (server.hasArg("min"))    minUs = constrain(server.arg("min").toInt(), 300, 2700);
  if (server.hasArg("max"))    maxUs = constrain(server.arg("max").toInt(), 300, 2700);
  if (server.hasArg("disp"))   dispMode = constrain(server.arg("disp").toInt(), 0, D_OFF);
  if (server.hasArg("ledpin")) ledPinReq = (server.arg("ledpin").toInt() == 38) ? 38 : 48;
  if (server.hasArg("flip"))   flipOled = server.arg("flip").toInt() != 0;
  if (server.hasArg("rgbm"))   rgbMode = constrain(server.arg("rgbm").toInt(), 0, 2);
  if (server.hasArg("color")) {
    long c = strtol(server.arg("color").c_str(), nullptr, 16);
    manR = (c >> 16) & 255; manG = (c >> 8) & 255; manB = c & 255;
    rgbMode = RGB_MANUAL;
  }
  if (server.hasArg("bright")) rgbMax = constrain(server.arg("bright").toInt(), 0, 255);
  if (server.hasArg("angle"))  { looping = false; sweepStage = 0; goTo(server.arg("angle").toInt()); }
  if (minUs >= maxUs) maxUs = minUs + 10;
  server.send(200, "text/plain", "ok");
}

void handleCmd() {
  String c = server.arg("do");
  if      (c == "loop")    startLoop();
  else if (c == "sweep")   startSweep();
  else if (c == "stop")    stopHere();
  else if (c == "release") releaseServo();
  else if (c == "save")    saveSettings();
  else if (c == "ledtest") ledTestReq = true;
  else if (c == "raw") {
    looping = false; sweepStage = 0; attachServo();
    rawUs = constrain(server.arg("us").toInt(), 300, 2700);
    writeUs(rawUs);
  } else { server.send(400, "text/plain", "unknown"); return; }
  server.send(200, "text/plain", "ok");
}

void startWifi() {
  if (strlen(HOME_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOME_SSID, HOME_PASS);
    Serial.printf("Connecting to %s", HOME_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) { Serial.printf("Open http://%s\n", WiFi.localIP().toString().c_str()); return; }
    Serial.println("Home Wi-Fi failed, starting hotspot instead.");
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("Join Wi-Fi \"%s\" (password %s), then open http://%s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nProject Sunaina - showcase: servo GPIO4, OLED GPIO8/9, RGB GPIO48");
#ifdef ARDUINO_BOARD
  Serial.printf("Board: %s\n", ARDUINO_BOARD);
#endif
  randomSeed(esp_random());
  loadSettings();
  ledTest();   // red, green, blue, white at boot
  attachServo();
  writeUs(angleToUs(pos));

  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, 0);

  startWifi();
  if (MDNS.begin("sunaina")) Serial.println("Also at http://sunaina.local");
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  server.on("/cmd", handleCmd);
  server.begin();
  lastMicros = micros();
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == '8') ledPinReq = (rgbPin == 48) ? 38 : 48;
  else if (c == 't') ledTestReq = true;
}

void loop() {
  server.handleClient();
  handleSerial();
  updateMotion();
  pushLed();
  delay(1);   // lets the idle task run on this core
}
