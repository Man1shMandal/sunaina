/*
  Project Sunaina - OLED test on a classic ESP32 (DevKit / WROOM)
  Module: 1.3" I2C OLED, 128x64 (usually SH1106 chip)

  Wiring:
    OLED VCC -> 3V3
    OLED GND -> GND
    OLED SDA -> D21
    OLED SCL -> D22

  Board: "ESP32 Dev Module". Serial Monitor: 115200, "Newline".
  Libraries: Adafruit SH110X, Adafruit GFX  (Adafruit SSD1306 only if you switch below)

  Runs ONCE: scan -> init -> draw test screen -> stop. Nothing repeats on its own.
  Serial commands:
    r   run the test again (after fixing wiring)
    p   show PASS
    f   show FAIL
    i   invert colours (quick "is it alive" check)

  Shifted picture with a noisy stripe on one edge -> set USE_SH1106 to false.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <esp_system.h>

#define USE_SH1106 true   // false = SSD1306
#define I2C_SDA 21
#define I2C_SCL 22

#if USE_SH1106
  #include <Adafruit_SH110X.h>
  Adafruit_SH1106G oled(128, 64, &Wire, -1, 100000, 100000);  // 100 kHz = more reliable
  #define COLOR SH110X_WHITE
#else
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 oled(128, 64, &Wire, -1, 100000, 100000);
  #define COLOR SSD1306_WHITE
#endif

bool ready = false;
bool inverted = false;

uint8_t findOled() {
  uint8_t found = 0;
  Serial.println("1) I2C scan on SDA D21, SCL D22:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("   found 0x%02X\n", a);
      if (a == 0x3C || a == 0x3D) found = a;
    }
  }
  return found;
}

void showBig(const char *t) {
  oled.clearDisplay();
  oled.setTextSize(3);
  oled.setCursor((128 - strlen(t) * 18) / 2, 20);
  oled.print(t);
  oled.display();
}

void runTest() {
  ready = false;
  uint8_t addr = findOled();
  if (!addr) {
    Serial.println("   No OLED at 0x3C/0x3D.");
    Serial.println("   Check: VCC->3V3, GND->GND, SDA->D21, SCL->D22 (try swapping). Then type r.");
    return;
  }

  Serial.printf("2) Init display at 0x%02X as %s... ", addr, USE_SH1106 ? "SH1106" : "SSD1306");
#if USE_SH1106
  bool ok = oled.begin(addr, true);
#else
  bool ok = oled.begin(SSD1306_SWITCHCAPVCC, addr);
#endif
  if (!ok) {
    Serial.println("FAILED (out of memory or I2C error). Type r to retry.");
    return;
  }
  Serial.println("ok");

  Serial.print("3) Drawing test screen... ");
  oled.clearDisplay();
  oled.setTextColor(COLOR);
  oled.drawRect(0, 0, 128, 64, COLOR);   // all 4 edges should be visible
  oled.setTextSize(1);
  oled.setCursor(10, 12); oled.print("Project Sunaina");
  oled.setCursor(10, 30); oled.print("ESP32 OLED OK");
  oled.setCursor(10, 46); oled.printf("I2C 0x%02X", addr);
  oled.display();
  Serial.println("done");
  Serial.println("Look at the screen: border on all 4 edges + 3 lines of text = PASS.");
  Serial.println("Commands: r = rerun, p = PASS, f = FAIL, i = invert");
  ready = true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nProject Sunaina - OLED test on ESP32");
  Serial.printf("Reset reason: %d  (1 = power on, 4-7 = crash/watchdog, 9 = brownout)\n",
                (int)esp_reset_reason());
  Wire.begin(I2C_SDA, I2C_SCL);
  runTest();
}

void loop() {
  if (!Serial.available()) return;
  String in = Serial.readStringUntil('\n');
  in.trim();
  if (in == "r") { runTest(); return; }
  if (!ready) { Serial.println("Display not ready. Type r to retry."); return; }
  if (in == "p") showBig("PASS");
  else if (in == "f") showBig("FAIL");
  else if (in == "i") { inverted = !inverted; oled.invertDisplay(inverted); }
  else Serial.println("?  r / p / f / i");
}
