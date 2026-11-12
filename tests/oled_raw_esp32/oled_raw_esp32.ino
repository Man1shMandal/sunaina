/*
  Project Sunaina - raw OLED diagnostic (classic ESP32), NO display library.
  For when the OLED is found on I2C but the screen stays blank.

  Wiring: VCC->3V3, GND->GND, SDA->D21, SCL->D22
  Board: "ESP32 Dev Module". Serial Monitor: 115200, "Newline".

  On boot it switches the panel on and lights EVERY pixel (white screen).
  Commands:
    a   all pixels on   (panel test, works on SH1106 and SSD1306)
    n   normal mode     (shows what is in memory)
    1   stripes, SH1106 style   (2 px column offset)
    2   stripes, SSD1306 style
    c   clear memory
    h   switch on again using the SH1106 power command only
    s   switch on again using the SSD1306 power command only
*/

#include <Wire.h>

#define I2C_SDA 21
#define I2C_SCL 22
uint8_t ADDR = 0x3C;

bool cmd(uint8_t c) {
  Wire.beginTransmission(ADDR);
  Wire.write(0x00);   // control byte: command
  Wire.write(c);
  return Wire.endTransmission() == 0;
}

void cmds(const uint8_t *list, int n) {
  for (int i = 0; i < n; i++) cmd(list[i]);
}

// Same basic setup for both chips. Only the power-on command differs.
const uint8_t BASE[] = {
  0xAE,        // display off
  0xD5, 0x80,  // clock
  0xA8, 0x3F,  // 64 rows
  0xD3, 0x00,  // no vertical offset
  0x40,        // start line 0
  0xA1,        // flip columns
  0xC8,        // flip rows
  0xDA, 0x12,  // COM pins
  0x81, 0xCF,  // contrast
  0xD9, 0xF1,  // pre-charge
  0xDB, 0x40,  // VCOM level
  0x20, 0x02,  // page addressing (SSD1306; harmless on SH1106)
};

void switchOn(bool sh1106, bool ssd1306) {
  cmds(BASE, sizeof(BASE));
  if (ssd1306) { cmd(0x8D); cmd(0x14); }   // SSD1306 charge pump on
  if (sh1106)  { cmd(0xAD); cmd(0x8B); }   // SH1106 DC-DC on
  cmd(0xA6);   // not inverted
  cmd(0xAF);   // display on
  delay(100);
}

void fill(uint8_t pattern, uint8_t colOffset) {
  for (uint8_t page = 0; page < 8; page++) {
    cmd(0xB0 + page);
    cmd(0x00 + (colOffset & 0x0F));
    cmd(0x10 + (colOffset >> 4));
    for (int x = 0; x < 128; x += 16) {
      Wire.beginTransmission(ADDR);
      Wire.write(0x40);   // control byte: data
      for (int k = 0; k < 16; k++) Wire.write(((x + k) / 8) % 2 ? pattern : 0x00);
      Wire.endTransmission();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nProject Sunaina - raw OLED diagnostic");
  Wire.begin(I2C_SDA, I2C_SCL, 100000);

  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() != 0) {
    Wire.beginTransmission(0x3D);
    if (Wire.endTransmission() == 0) ADDR = 0x3D;
    else { Serial.println("No OLED at 0x3C or 0x3D. Check wiring."); return; }
  }
  Serial.printf("OLED at 0x%02X\n", ADDR);

  switchOn(true, true);   // send both power commands; each chip ignores the other's
  cmd(0xA5);              // all pixels on
  Serial.println("Sent: power on + ALL PIXELS ON. The whole screen should be white/blue now.");
  Serial.println("Commands: a, n, 1, 2, c, h, s  (see top of file)");
}

void loop() {
  if (!Serial.available()) return;
  String in = Serial.readStringUntil('\n');
  in.trim();
  if      (in == "a") { cmd(0xA5); Serial.println("all pixels on"); }
  else if (in == "n") { cmd(0xA4); Serial.println("normal mode (memory contents)"); }
  else if (in == "1") { cmd(0xA4); fill(0xFF, 2); Serial.println("stripes, SH1106 style"); }
  else if (in == "2") { cmd(0xA4); fill(0xFF, 0); Serial.println("stripes, SSD1306 style"); }
  else if (in == "c") { fill(0x00, 0); fill(0x00, 2); Serial.println("cleared"); }
  else if (in == "h") { switchOn(true, false); cmd(0xA5); Serial.println("on with SH1106 power cmd, all pixels"); }
  else if (in == "s") { switchOn(false, true); cmd(0xA5); Serial.println("on with SSD1306 power cmd, all pixels"); }
  else Serial.println("?  a / n / 1 / 2 / c / h / s");
}
