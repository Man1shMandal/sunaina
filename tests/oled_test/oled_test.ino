/*
  Project Sunaina - OLED test (ESP32-S3)
  Module: 1.3" I2C OLED, 128x64 (usually SH1106 chip; 0.96" ones are SSD1306)

  Wiring (same pins as the main firmware):
    OLED VCC -> 3V3
    OLED GND -> GND
    OLED SDA -> GPIO8
    OLED SCL -> GPIO9

  Libraries (Arduino IDE > Library Manager):
    Adafruit SH110X, Adafruit GFX   (and Adafruit SSD1306 only if you switch below)

  What it does:
    1. Scans I2C and prints every address found (expect 0x3C, sometimes 0x3D).
    2. Shows a border + text, then loops PASS / FAIL screens.

  If the picture is shifted by 2 px with a stripe of noise on one edge,
  the chip is the other type: flip USE_SH1106 below.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>

#define USE_SH1106 true   // false = SSD1306
#define I2C_SDA 8
#define I2C_SCL 9

#if USE_SH1106
  #include <Adafruit_SH110X.h>
  Adafruit_SH1106G oled(128, 64, &Wire, -1);
  #define WHITE SH110X_WHITE
#else
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 oled(128, 64, &Wire, -1);
  #define WHITE SSD1306_WHITE
#endif

uint8_t addr = 0;

void scanI2C() {
  Serial.println("I2C scan:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\n", a);
      if (a == 0x3C || a == 0x3D) addr = a;
    }
  }
  if (!addr) Serial.println("  no OLED at 0x3C/0x3D. Check wiring, SDA/SCL swapped, 3V3 and GND.");
}

void bigText(const char *t) {
  oled.clearDisplay();
  oled.setTextSize(3);
  oled.setCursor((128 - strlen(t) * 18) / 2, 20);
  oled.print(t);
  oled.display();
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\nProject Sunaina - OLED test, SDA GPIO8, SCL GPIO9");
  Wire.begin(I2C_SDA, I2C_SCL);
  scanI2C();
  if (!addr) return;

#if USE_SH1106
  bool ok = oled.begin(addr, true);
#else
  bool ok = oled.begin(SSD1306_SWITCHCAPVCC, addr);
#endif
  if (!ok) { Serial.println("Display init failed"); addr = 0; return; }
  Serial.printf("OLED ready at 0x%02X (%s)\n", addr, USE_SH1106 ? "SH1106" : "SSD1306");

  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.drawRect(0, 0, 128, 64, WHITE);   // all 4 edges should be visible
  oled.setTextSize(1);
  oled.setCursor(10, 12); oled.print("Project Sunaina");
  oled.setCursor(10, 30); oled.print("OLED test OK");
  oled.setCursor(10, 46); oled.printf("I2C 0x%02X", addr);
  oled.display();
  delay(3000);
}

void loop() {
  if (!addr) { delay(1000); return; }
  bigText("PASS");
  delay(1500);
  bigText("FAIL");
  delay(1500);
}
