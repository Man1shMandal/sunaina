/*
  Project Sunaina - comic-style spider animation (ESP32-S3 + 1.3" I2C OLED)

  Wiring: VCC->3V3, GND->GND, SDA->GPIO8, SCL->GPIO9  (same as the main firmware)
  Board: "ESP32S3 Dev Module"
  Serial: if you use the USB port marked "USB" (not "COM/UART"), set
          Tools > USB CDC On Boot > Enabled, or you will see no Serial output.
  Libraries: Adafruit SH110X, Adafruit GFX  (Adafruit SSD1306 if you switch below)

  Scenes (loop forever):
    1. Web spins out from the centre
    2. Spider drops on a thread and bounces
    3. Glitch reveal: split, sliced, scan-line emblem
    4. Halftone dot burst behind the emblem
    5. "THWIP!" comic burst with screen shake
    6. Zoom through speed lines, flash, repeat

  Onboard RGB LED (GPIO48) is synced to every frame:
    web = blue glow, drop = red bounce, glitch = red/blue flicker,
    halftone = red/blue pulse, THWIP = yellow pop, zoom = red ramp to white.
  At boot the LED shows red, green, blue (self-test), then blinks white while
  waiting for the OLED (slow white blink). LED dark? Type 8 in Serial Monitor to swap GPIO48 / 38.

  The spider emblem is drawn from code (original design, not a copied logo).
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <esp_system.h>
#include <esp_arduino_version.h>
#if ESP_ARDUINO_VERSION_MAJOR < 3
  #define rgbLedWrite neopixelWrite   // older ESP32 core (2.x) name
#endif

#define USE_SH1106 true   // false = SSD1306 (use whichever worked in the diagnostic)
#define I2C_SDA 8
#define I2C_SCL 9
int rgbPin = 48;        // some S3 boards use 38; type 8 to swap
#define RGB_MAX 150     // LED brightness cap (0-255)

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

const int W = 128, H = 64, CX = 64, CY = 32;
const int FRAME_MS = 30;
bool screenInverted = false;   // set when a frame inverts, so the LED can flash too

void rgb(int r, int g, int b) {
  r = constrain(r, 0, 255) * RGB_MAX / 255;
  g = constrain(g, 0, 255) * RGB_MAX / 255;
  b = constrain(b, 0, 255) * RGB_MAX / 255;
  rgbLedWrite(rgbPin, r, g, b);
}

// Type 8 any time to swap the LED pin (same as the S3 board test).
void checkSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '8') {
      rgbLedWrite(rgbPin, 0, 0, 0);
      rgbPin = (rgbPin == 48) ? 38 : 48;
      Serial.printf("RGB pin is now GPIO%d\n", rgbPin);
    }
  }
}

// ---------- drawing helpers ----------

void thickLine(int x0, int y0, int x1, int y1, uint16_t c, bool thick) {
  oled.drawLine(x0, y0, x1, y1, c);
  if (thick) {
    oled.drawLine(x0 + 1, y0, x1 + 1, y1, c);
    oled.drawLine(x0, y0 + 1, x1, y1 + 1, c);
  }
}

// Leg shape for the right side: hip, knee, tip (mirrored for the left).
const int8_t LEGS[4][6] = {
  {2, -4, 10, -12,  8, -22},
  {3, -2, 14,  -6, 16, -16},
  {3,  1, 14,   4, 16,  14},
  {2,  3, 10,  10,  8,  22},
};

void drawSpider(int cx, int cy, float s, uint16_t c) {
  bool thick = s >= 1.0;
  // abdomen: circle + tail = teardrop pointing down
  oled.fillCircle(cx, cy + 6 * s, 6 * s, c);
  oled.fillTriangle(cx - 4 * s, cy + 9 * s, cx + 4 * s, cy + 9 * s, cx, cy + 18 * s, c);
  // thorax and head
  oled.fillCircle(cx, cy - 2 * s, 4.5 * s, c);
  oled.fillCircle(cx, cy - 8 * s, 3 * s, c);
  // legs
  for (int i = 0; i < 4; i++) {
    for (int side = -1; side <= 1; side += 2) {
      int hx = cx + side * LEGS[i][0] * s, hy = cy + LEGS[i][1] * s;
      int kx = cx + side * LEGS[i][2] * s, ky = cy + LEGS[i][3] * s;
      int tx = cx + side * LEGS[i][4] * s, ty = cy + LEGS[i][5] * s;
      thickLine(hx, hy, kx, ky, c, thick);
      thickLine(kx, ky, tx, ty, c, thick);
    }
  }
}

// Shift random 8-px bands sideways (works on the raw page buffer).
void glitchBands(int amount) {
  if (amount <= 0) return;
  uint8_t *b = oled.getBuffer();
  uint8_t tmp[W];
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
  for (int i = 0; i <= 4; i++) {
    float a = i * (PI / 2) / 4;
    oled.drawLine(0, 0, cos(a) * 34, sin(a) * 34, ON);
  }
  for (int r = 9; r <= 30; r += 7) {
    for (int i = 0; i < 4; i++) {
      float a0 = i * (PI / 2) / 4, a1 = (i + 1) * (PI / 2) / 4, am = (a0 + a1) / 2;
      int x0 = cos(a0) * r, y0 = sin(a0) * r, x1 = cos(a1) * r, y1 = sin(a1) * r;
      int xm = cos(am) * r * 0.85, ym = sin(am) * r * 0.85;
      oled.drawLine(x0, y0, xm, ym, ON);
      oled.drawLine(xm, ym, x1, y1, ON);
    }
  }
}

// ---------- scenes (f = frame number inside the scene) ----------

void sceneWeb(int f) {
  const int SP = 12;
  float grow = min(1.0f, f / 18.0f);
  float spin = f * 0.02;
  for (int i = 0; i < SP; i++) {
    float a = i * 2 * PI / SP + spin;
    oled.drawLine(CX, CY, CX + cos(a) * 80 * grow, CY + sin(a) * 80 * grow, ON);
  }
  int rings = (f - 18) / 4;
  for (int k = 1; k <= min(rings, 9); k++) {
    int r = k * 8;
    for (int i = 0; i < SP; i++) {
      float a0 = i * 2 * PI / SP + spin, a1 = (i + 1) * 2 * PI / SP + spin, am = (a0 + a1) / 2;
      int x0 = CX + cos(a0) * r, y0 = CY + sin(a0) * r;
      int x1 = CX + cos(a1) * r, y1 = CY + sin(a1) * r;
      int xm = CX + cos(am) * r * 0.82, ym = CY + sin(am) * r * 0.82;
      oled.drawLine(x0, y0, xm, ym, ON);
      oled.drawLine(xm, ym, x1, y1, ON);
    }
  }
  if (f > 50) {
    float s = min(0.6f, (f - 50) * 0.06f);
    oled.fillCircle(CX, CY, 14 * s + 2, OFF);
    drawSpider(CX, CY, s, ON);
  }
}

void sceneDrop(int f) {
  const float s = 0.6;
  const int target = 34;
  float y;
  if (f < 10) y = -15 + (target + 15) * f / 10.0;
  else y = target + 12 * exp(-(f - 10) / 12.0) * cos((f - 10) * 0.45);
  int x = CX + 5 * sin(f * 0.12);
  cornerWeb();
  oled.drawLine(CX, 0, x, y - 8 * s, ON);   // thread
  drawSpider(x, y, s, ON);
}

void sceneGlitch(int f) {
  int intensity = max(0, 24 - (int)(f * 0.6));
  if (intensity > 2) {
    int d = random(2, 5);
    drawSpider(CX - d, CY, 1.3, ON);
    drawSpider(CX + d, CY + random(-1, 2), 1.3, INV);   // split / misprint look
  } else {
    drawSpider(CX, CY, 1.3, ON);
  }
  glitchBands(intensity);
  if (intensity > 6 && random(8) == 0) invertBuffer();
  int scan = (f * 4) % H;
  oled.drawFastHLine(0, scan, W, INV);
}

void sceneHalftone(int f) {
  float wave = f * 2.4;
  for (int gx = 4; gx < W; gx += 8) {
    for (int gy = 4; gy < H; gy += 8) {
      float d = sqrt((gx - CX) * (gx - CX) + (gy - CY) * (gy - CY));
      float v = 1.0 - fabs(d - wave) / 18.0;
      int r = (d < wave) ? 1 : 0;
      if (v > 0) r = max(r, (int)round(v * 4));
      if (r) oled.fillCircle(gx, gy, r, ON);
    }
  }
  if (f > 12) {
    int rad = min(29, (f - 12) * 3);
    oled.fillCircle(CX, CY, rad + 2, OFF);
    oled.fillCircle(CX, CY, rad, ON);
    if (rad > 20) drawSpider(CX, CY, 1.15, OFF);
  }
}

void sceneThwip(int f) {
  int sx = f < 22 ? random(-3, 4) : 0;
  int sy = f < 22 ? random(-2, 3) : 0;
  float grow = min(1.0f, f / 6.0f);
  const int N = 18;
  for (int i = 0; i < N; i++) {
    float a0 = i * 2 * PI / N + f * 0.03, a1 = (i + 1) * 2 * PI / N + f * 0.03;
    float r0 = (i % 2 ? 46 : 74) * grow, r1 = ((i + 1) % 2 ? 46 : 74) * grow;
    oled.fillTriangle(CX + sx, CY + sy,
                      CX + sx + cos(a0) * r0, CY + sy + sin(a0) * r0,
                      CX + sx + cos(a1) * r1, CY + sy + sin(a1) * r1, ON);
  }
  int size = f < 3 ? 1 : f < 6 ? 2 : 3;
  const char *t = "THWIP!";
  int tw = strlen(t) * 6 * size;
  oled.setTextSize(size);
  oled.setTextColor(OFF);
  oled.setCursor(CX - tw / 2 + sx, CY - 4 * size + sy);
  oled.print(t);
}

void sceneZoom(int f) {
  for (int i = 0; i < 20; i++) {
    float a = i * 2 * PI / 20 + 0.15;
    float r0 = fmod(f * 7 + i * 13, 75);
    float r1 = r0 + 6 + r0 / 6;
    oled.drawLine(CX + cos(a) * r0, CY + sin(a) * r0, CX + cos(a) * r1, CY + sin(a) * r1, ON);
  }
  float s = 0.2 * pow(1.075, f);
  drawSpider(CX, CY + 2 * s, s, ON);
  if (f > 40) invertBuffer();
}

// ---------- RGB LED, one call per frame ----------

void ledFor(int scene, int f) {
  if (screenInverted) { rgb(255, 255, 255); return; }   // flash with the screen
  switch (scene) {
    case 0: {   // web: blue glow growing with the web
      int v = min(255, 60 + f * 4);
      rgb(0, v / 6, v);
      break;
    }
    case 1: {   // drop: red, brightest at the bottom of each bounce
      float bounce = f < 10 ? f / 10.0 : 0.5 + 0.5 * cos((f - 10) * 0.45) * exp(-(f - 10) / 12.0);
      rgb(60 + 195 * bounce, 0, 10);
      break;
    }
    case 2: {   // glitch: random red/blue flicker, settles to red
      int intensity = max(0, 24 - (int)(f * 0.6));
      if (intensity > 2 && random(3) == 0) rgb(random(2) ? 255 : 0, 0, random(2) ? 255 : 0);
      else rgb(200, 0, 0);
      break;
    }
    case 3: {   // halftone: pulse red/blue with the dot wave
      float p = 0.5 + 0.5 * sin(f * 0.4);
      rgb(255 * p, 0, 255 * (1 - p));
      break;
    }
    case 4: {   // THWIP: yellow pop, flickers while shaking
      if (f < 6) rgb(255, 255, 255);
      else if (f < 22) rgb(255, random(120, 220), 0);
      else rgb(255, 180, 0);
      break;
    }
    case 5: {   // zoom: red ramping up toward white
      int v = min(255, f * 6);
      rgb(255, v / 2, v / 2);
      break;
    }
  }
}

// ---------- engine ----------

typedef void (*SceneFn)(int);
struct Scene { SceneFn fn; int frames; };
Scene scenes[] = {
  {sceneWeb,      70},
  {sceneDrop,     70},
  {sceneGlitch,   60},
  {sceneHalftone, 60},
  {sceneThwip,    45},
  {sceneZoom,     46},
};

void flash() {
  rgb(255, 255, 255);
  oled.fillScreen(ON);
  oled.display();
  delay(40);
}

void setup() {
  Serial.begin(115200);
  delay(1500);   // give USB serial time to connect on the S3
  Serial.println("\nProject Sunaina - spider animation on ESP32-S3, SDA GPIO8, SCL GPIO9");

  // LED self-test first, so it works even if the OLED is missing.
  Serial.printf("RGB self-test on GPIO%d (compare the LED with these names):\n", rgbPin);
  Serial.println("  RED");   rgb(255, 0, 0); delay(700);
  Serial.println("  GREEN"); rgb(0, 255, 0); delay(700);
  Serial.println("  BLUE");  rgb(0, 0, 255); delay(700);
  rgb(0, 0, 0);
  Serial.println("Self-test done. A slow WHITE blink from now on = waiting for the OLED.");

  // Wait here until the OLED answers. Tries SDA/SCL both ways round and
  // reports the line levels so a loose wire is easy to spot.
  int sda = I2C_SDA, scl = I2C_SCL;
  bool found = false;
  while (!found) {
    for (int t = 0; t < 2 && !found; t++) {
      int a = t ? I2C_SCL : I2C_SDA, b = t ? I2C_SDA : I2C_SCL;
      Wire.end();
      pinMode(a, INPUT);              // no internal pull-up: only the OLED's own
      pinMode(b, INPUT);              // pull-ups can make these read HIGH
      delay(2);
      int la = digitalRead(a), lb = digitalRead(b);
      Wire.begin(a, b, 100000);
      Wire.beginTransmission(0x3C);
      if (Wire.endTransmission() == 0) { sda = a; scl = b; found = true; break; }
      if (t == 0) {
        Serial.printf("No OLED at 0x3C. Line levels: GPIO%d=%s  GPIO%d=%s\n",
                      a, la ? "HIGH" : "LOW", b, lb ? "HIGH" : "LOW");
        if (!la || !lb)
          Serial.println("  A LOW line = that wire is not reaching the OLED (or OLED has no power).");
        else
          Serial.println("  Both HIGH = wires reach the OLED but it does not answer. Check GND is shared.");
      }
    }
    if (found) break;
    for (int i = 0; i < 2; i++) {     // white blink = waiting for OLED (same in any colour order)
      checkSerial();
      rgb(120, 120, 120); delay(500);
      rgb(0, 0, 0);       delay(500);
    }
  }
  if (sda != I2C_SDA) Serial.println("Note: SDA and SCL are swapped in your wiring. Working anyway.");
  Serial.printf("OLED found at 0x3C on SDA GPIO%d, SCL GPIO%d\n", sda, scl);

#if USE_SH1106
  bool ok = oled.begin(0x3C, true);
#else
  bool ok = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
#endif
  if (!ok) { Serial.println("OLED init failed"); while (true) delay(1000); }
  randomSeed(esp_random());
  rgb(0, 0, 0);
  Serial.println("Spider animation running");
}

void loop() {
  int n = 0;
  for (auto &sc : scenes) {
    for (int f = 0; f < sc.frames; f++) {
      unsigned long t0 = millis();
      screenInverted = false;
      oled.clearDisplay();
      sc.fn(f);
      ledFor(n, f);
      checkSerial();
      oled.display();
      long wait = FRAME_MS - (long)(millis() - t0);
      if (wait > 0) delay(wait);
    }
    flash();
    n++;
  }
}
