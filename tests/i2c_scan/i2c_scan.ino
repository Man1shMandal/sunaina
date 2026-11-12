/*
  Project Sunaina - I2C scan on every pin pair (ESP32-S3 or classic ESP32)
  Picks a safe pin list for the board you compile for.

  Tries each usable GPIO pair as SDA/SCL and scans addresses 0x01-0x7E.
  Use it when you don't know (or don't trust) the wiring.
  Expected for the 1.3" OLED: 0x3C (sometimes 0x3D).

  Classic ESP32 skips: 6-11 (flash - touching them resets the board),
    0, 2, 5, 12, 15 (strapping), 1, 3 (Serial), 34-39 (input only).

  S3 skips (unsafe or busy on the S3 DevKit):
    0, 3, 45, 46  strapping pins
    19, 20        USB
    26-37         flash / PSRAM
    43, 44        UART0 (Serial)
    48            RGB LED
    4             servo signal (would twitch the servo)

  Serial Monitor 115200. Press Enter to scan again.
*/

#include <Wire.h>
#include <esp_system.h>

#if CONFIG_IDF_TARGET_ESP32
const char *BOARD = "classic ESP32";
const uint8_t PINS[] = {4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
#else
const char *BOARD = "ESP32-S3";
const uint8_t PINS[] = {1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                        17, 18, 21, 38, 39, 40, 41, 42, 47};
#endif
const int N = sizeof(PINS);

// An idle I2C bus sits HIGH on both lines (pull-ups on the module).
bool linesHigh(uint8_t a, uint8_t b) {
  pinMode(a, INPUT_PULLUP);
  pinMode(b, INPUT_PULLUP);
  delayMicroseconds(50);
  return digitalRead(a) && digitalRead(b);
}

void scanAll() {
  Serial.println("\nScanning all pin pairs...");
  int hits = 0;
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      if (i == j) continue;
      uint8_t sda = PINS[i], scl = PINS[j];
      if (!linesHigh(sda, scl)) continue;

      Wire.begin(sda, scl, 100000);
      Wire.setTimeOut(20);
      for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
          Serial.printf("  SDA=GPIO%-2d SCL=GPIO%-2d  device 0x%02X%s\n", sda, scl, a,
                        (a == 0x3C || a == 0x3D) ? "  <- OLED" : "");
          hits++;
        }
      }
      Wire.end();
      delay(1);   // let background tasks run
    }
  }
  if (hits) Serial.printf("Done, %d hit(s).\n", hits);
  else Serial.println("Nothing found. Check OLED VCC -> 3V3, GND, and that SDA/SCL go to GPIO pins listed above.");
  Serial.println("Press Enter to scan again.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.printf("\nProject Sunaina - I2C scan on every pin pair (%s)\n", BOARD);
  Serial.printf("Last reset reason: %d  (1 = power on, 3 = software, 4-7 = crash/watchdog, 9 = brownout)\n",
                (int)esp_reset_reason());
  scanAll();
}

void loop() {
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    scanAll();
  }
}
