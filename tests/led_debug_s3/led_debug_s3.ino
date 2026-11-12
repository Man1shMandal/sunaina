/*
  Project Sunaina - RGB LED debug (ESP32-S3), LED only. No Wi-Fi, OLED or servo.

  Board: "ESP32S3 Dev Module". USB CDC On Boot: Enabled. Serial Monitor 115200, "Newline".
  (This sketch compiles on any board on purpose, so it can tell you which board was picked.)

  At boot it prints board / core / chip info, then runs test 1 on GPIO48.

  Commands (type + Enter):
    1   colour sequence with the core driver (rgbLedWrite)
    2   colour sequence bit-banged (no driver, exact CPU timing)
    3   colour order check (is RED really red?)
    4   brightness ramp
    5   plain on/off blink (for boards with a normal, non-addressable LED)
    6   pin scan: white on every safe pin, one by one (bit-banged)
    7   built-in mapping: digitalWrite(RGB_BUILTIN) (core's own idea of the LED pin)
    a   run 1, 2, 3, 4
    r g b w   solid red / green / blue / white      0   off
    p38 / p48 / pNN   use another pin
*/

#include <esp_arduino_version.h>
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#if ESP_ARDUINO_VERSION_MAJOR < 3
  #define rgbLedWrite neopixelWrite
#endif

int pin = 48;
const int BRIGHT = 60;   // same level as the ESPS3 board test

// ---------- bit-banged WS2812 (GRB, 800 kHz) ----------
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR bbSend(int p, uint8_t r, uint8_t g, uint8_t b) {
  uint32_t mhz = ESP.getCpuFreqMHz();
  uint32_t t0h = mhz * 400 / 1000, t1h = mhz * 800 / 1000, per = mhz * 1250 / 1000;
  uint32_t setReg = p < 32 ? GPIO_OUT_W1TS_REG : GPIO_OUT1_W1TS_REG;
  uint32_t clrReg = p < 32 ? GPIO_OUT_W1TC_REG : GPIO_OUT1_W1TC_REG;
  uint32_t mask = 1u << (p < 32 ? p : p - 32);
  uint8_t data[3] = {g, r, b};

  pinMode(p, OUTPUT);
  REG_WRITE(clrReg, mask);
  delayMicroseconds(300);              // reset / latch

  portENTER_CRITICAL(&mux);
  for (int i = 0; i < 3; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      uint32_t start = ESP.getCycleCount();
      REG_WRITE(setReg, mask);
      uint32_t hi = (data[i] >> bit) & 1 ? t1h : t0h;
      while (ESP.getCycleCount() - start < hi) {}
      REG_WRITE(clrReg, mask);
      while (ESP.getCycleCount() - start < per) {}
    }
  }
  portEXIT_CRITICAL(&mux);
  delayMicroseconds(300);
}

// ---------- helpers ----------
void show(const char *name, uint8_t r, uint8_t g, uint8_t b, bool bitbang, int ms = 800) {
  Serial.printf("  GPIO%d %-8s -> %s\n", pin, bitbang ? "bitbang" : "driver", name);
  if (bitbang) bbSend(pin, r, g, b); else rgbLedWrite(pin, r, g, b);
  delay(ms);
}

void sequence(bool bitbang) {
  Serial.printf("\n=== Test %d: colour sequence, %s ===\n", bitbang ? 2 : 1,
                bitbang ? "bit-banged (no driver)" : "core driver rgbLedWrite");
  show("RED",   BRIGHT, 0, 0, bitbang);
  show("GREEN", 0, BRIGHT, 0, bitbang);
  show("BLUE",  0, 0, BRIGHT, bitbang);
  show("WHITE", BRIGHT, BRIGHT, BRIGHT, bitbang);
  show("OFF",   0, 0, 0, bitbang, 300);
  Serial.println("Did the LED follow the names? Note yes / no / which colours.");
}

void colourOrder() {
  Serial.println("\n=== Test 3: colour order ===");
  Serial.println("Each colour stays 2 s. Write down what you SEE for each.");
  show("RED (should look red)",     BRIGHT, 0, 0, false, 2000);
  show("GREEN (should look green)", 0, BRIGHT, 0, false, 2000);
  show("BLUE (should look blue)",   0, 0, BRIGHT, false, 2000);
  rgbLedWrite(pin, 0, 0, 0);
  Serial.println("RED shows green and GREEN shows red = LED wants RGB order, not GRB (fixable in code).");
}

void ramp() {
  Serial.println("\n=== Test 4: brightness ramp (white, 0 -> 120 -> 0) ===");
  for (int v = 0; v <= 120; v += 4) { rgbLedWrite(pin, v, v, v); delay(25); }
  for (int v = 120; v >= 0; v -= 4) { rgbLedWrite(pin, v, v, v); delay(25); }
  Serial.println("Smooth fade up and down = driver and LED are fine.");
}

void plainBlink() {
  Serial.printf("\n=== Test 5: plain on/off blink on GPIO%d (5 times) ===\n", pin);
  pinMode(pin, OUTPUT);
  for (int i = 0; i < 5; i++) { digitalWrite(pin, HIGH); delay(400); digitalWrite(pin, LOW); delay(400); }
  Serial.println("A normal LED blinks here. An addressable RGB LED usually ignores this.");
}

void pinScan() {
  const int pins[] = {1, 2, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48};
  Serial.println("\n=== Test 6: pin scan (bit-banged white, 1 s per pin) ===");
  Serial.println("Skipped: 0 3 45 46 (boot), 19 20 (USB), 26-37 (flash/PSRAM), 43 44 (serial), 4 8 9 (servo/OLED).");
  Serial.println("Watch the LED and note the GPIO number printed when it lights up.");
  for (int p : pins) {
    Serial.printf("  GPIO%d\n", p);
    bbSend(p, BRIGHT, BRIGHT, BRIGHT);
    delay(1000);
    bbSend(p, 0, 0, 0);
    pinMode(p, INPUT);
  }
  Serial.println("Done. Type pNN to use the pin that lit up.");
}

void builtin() {
  Serial.println("\n=== Test 7: core's built-in LED mapping ===");
#ifdef RGB_BUILTIN
  Serial.printf("RGB_BUILTIN is defined for this board (LED on GPIO%d). Writing white via digitalWrite.\n",
                RGB_BUILTIN >= SOC_GPIO_PIN_COUNT ? RGB_BUILTIN - SOC_GPIO_PIN_COUNT : RGB_BUILTIN);
  digitalWrite(RGB_BUILTIN, HIGH); delay(1500);
  digitalWrite(RGB_BUILTIN, LOW);
#else
  Serial.println("RGB_BUILTIN is NOT defined for this board. The selected board may be wrong.");
#endif
}

void info() {
  Serial.println("\n########## RGB LED DEBUG ##########");
#ifdef ARDUINO_BOARD
  Serial.printf("Board selected : %s\n", ARDUINO_BOARD);
#endif
#if defined(ARDUINO_ESP32S3_DEV)
  Serial.println("Board check    : OK (ESP32S3 Dev Module)");
#else
  Serial.println("Board check    : NOT ESP32S3 Dev Module  <-- select it in Tools > Board > esp32");
#endif
  Serial.printf("Arduino core   : %d.%d.%d\n", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("Chip           : %s rev %d, %d MHz\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
  Serial.printf("Flash / PSRAM  : %d MB / %d MB\n", ESP.getFlashChipSize() / 1048576, ESP.getPsramSize() / 1048576);
#ifdef RGB_BUILTIN
  Serial.printf("RGB_BUILTIN    : GPIO%d\n", RGB_BUILTIN >= SOC_GPIO_PIN_COUNT ? RGB_BUILTIN - SOC_GPIO_PIN_COUNT : RGB_BUILTIN);
#else
  Serial.println("RGB_BUILTIN    : not defined");
#endif
  Serial.printf("Testing pin    : GPIO%d\n", pin);
}

void menu() {
  Serial.println("\nCommands: 1 driver | 2 bitbang | 3 order | 4 ramp | 5 blink | 6 pin scan | 7 builtin");
  Serial.println("          a all | r g b w solid | 0 off | p38 / p48 / pNN change pin");
}

void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(300);
  info();
  sequence(false);
  menu();
}

void loop() {
  if (!Serial.available()) return;
  String in = Serial.readStringUntil('\n');
  in.trim();
  if (!in.length()) return;

  if      (in == "1") sequence(false);
  else if (in == "2") sequence(true);
  else if (in == "3") colourOrder();
  else if (in == "4") ramp();
  else if (in == "5") plainBlink();
  else if (in == "6") pinScan();
  else if (in == "7") builtin();
  else if (in == "a") { sequence(false); sequence(true); colourOrder(); ramp(); }
  else if (in == "r") show("RED", BRIGHT, 0, 0, false, 0);
  else if (in == "g") show("GREEN", 0, BRIGHT, 0, false, 0);
  else if (in == "b") show("BLUE", 0, 0, BRIGHT, false, 0);
  else if (in == "w") show("WHITE", BRIGHT, BRIGHT, BRIGHT, false, 0);
  else if (in == "0") show("OFF", 0, 0, 0, false, 0);
  else if (in[0] == 'p') {
    int n = in.substring(1).toInt();
    if (n >= 0 && n <= 48) { rgbLedWrite(pin, 0, 0, 0); pin = n; Serial.printf("Now testing GPIO%d\n", pin); }
    else Serial.println("Pin must be 0-48");
  }
  else Serial.println("?");
  menu();
}
