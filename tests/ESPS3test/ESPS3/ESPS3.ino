/*
  ESP32-S3 complete board test  (written for ESP32-S3 N16R8: 16MB flash, 8MB PSRAM)

  Automatic tests (no wiring needed):
    Chip info + reset reason, internal RAM, full PSRAM, flash file system, NVS,
    CPU (both cores, 80/160/240 MHz), hardware timer interrupt,
    hardware crypto (SHA-256, AES-256), random number generator,
    GPIO pin check (internal pull-up/down on every free pin), I2C bus scan,
    Wi-Fi scan + connect, Bluetooth LE scan + advertise

  Interactive tests:
    RGB LED colours, BOOT button, touch pins (live), light sleep, deep sleep,
    Wi-Fi speed test (button on the web page)

  Optional jumper-wire tests (skipped if the wire isn't there):
    GPIO4  <-> GPIO5   digital in/out, ADC, PWM, GPIO interrupt
    GPIO17 <-> GPIO18  UART loopback (115200 to 2M baud)
    GPIO11 <-> GPIO13  SPI loopback (1, 10, 20 MHz)

  Arduino IDE settings (Tools menu):
    Board:            ESP32S3 Dev Module
    USB CDC On Boot:  Enabled   (you're using the native USB port)
    Flash Size:       16MB (128Mb)
    PSRAM:            OPI PSRAM
    Partition Scheme: 8M with spiffs (3MB APP/1.5MB SPIFFS)
    Serial Monitor:   115200 baud
*/

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"

// ---------- Your Wi-Fi ----------
// Home Wi-Fi lives in wifi_secrets.h (git-ignored). Copy wifi_secrets.example.h to start.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef HOME_WIFI_SSID
#define HOME_WIFI_SSID ""
#define HOME_WIFI_PASS ""
#endif
const char *WIFI_SSID = HOME_WIFI_SSID;
const char *WIFI_PASS = HOME_WIFI_PASS;

const char *AP_SSID = "ESP32-S3-Test";
const char *AP_PASS = "12345678";

// ---------- Pins ----------
int rgbPin = 48;      // RGB LED. Some boards use 38; type '8' to switch.
#define BOOT_BTN 0
#define LOOP_OUT 4    // jumper GPIO4 <-> GPIO5
#define LOOP_IN 5
#define UART_TX 17    // jumper GPIO17 <-> GPIO18
#define UART_RX 18
#define SPI_MOSI 11   // jumper GPIO11 <-> GPIO13
#define SPI_MISO 13
#define SPI_SCK 12
#define I2C_SDA 8
#define I2C_SCL 9

// ---------- Results table ----------
// state: 0 = not run, 1 = PASS, -1 = FAIL, 2 = SKIP
enum TestId {
  T_RAM, T_PSRAM, T_FS, T_NVS, T_CPU, T_FREQ, T_TIMER, T_SHA, T_AES, T_RNG,
  T_GPIO, T_LOOP, T_PWM, T_IRQ, T_UART, T_SPI, T_I2C,
  T_RGB, T_BUTTON, T_WIFI, T_BLE, T_LSLEEP, T_DSLEEP, T_COUNT
};
struct Test {
  const char *name;
  int state;
  String info;
};
Test tests[T_COUNT] = {
  { "Internal RAM" }, { "PSRAM" }, { "Flash file system" }, { "NVS storage" },
  { "CPU dual core" }, { "CPU frequencies" }, { "Hardware timer" },
  { "SHA-256 (hw)" }, { "AES-256 (hw)" }, { "Random numbers" },
  { "GPIO pin check" }, { "GPIO/ADC 4<->5" }, { "PWM 4<->5" }, { "Interrupt 4<->5" },
  { "UART 17<->18" }, { "SPI 11<->13" }, { "I2C scan" },
  { "RGB LED" }, { "BOOT button" }, { "Wi-Fi" }, { "Bluetooth LE" },
  { "Light sleep" }, { "Deep sleep" }
};

const char *stateText(int s) {
  return s == 1 ? "PASS" : s == -1 ? "FAIL" : s == 2 ? "SKIP" : "-";
}

void setResult(TestId id, int state, String info = "") {
  tests[id].state = state;
  tests[id].info = info;
  Serial.printf("  -> %s: %s %s\n", tests[id].name, stateText(state), info.c_str());
}

enum Mode { MODE_STATUS, MODE_RAINBOW, MODE_MANUAL };
Mode ledMode = MODE_STATUS;

RTC_DATA_ATTR int deepSleepCount = 0;  // survives deep sleep
WebServer server(80);
bool bleReady = false;

void rgb(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(rgbPin, r, g, b);
}

// Returns true if a wire connects pin a to pin b
bool jumperPresent(int a, int b) {
  pinMode(a, OUTPUT);
  digitalWrite(a, HIGH);
  pinMode(b, INPUT_PULLDOWN);
  delayMicroseconds(100);
  int h = digitalRead(b);
  digitalWrite(a, LOW);
  pinMode(b, INPUT_PULLUP);
  delayMicroseconds(100);
  int l = digitalRead(b);
  pinMode(a, INPUT);
  pinMode(b, INPUT);
  return h == 1 && l == 0;
}

// =====================================================================
// Chip info
// =====================================================================
const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power supply too weak)";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_EXT: return "reset button";
    default: return "other";
  }
}

void testChip() {
  Serial.println("\n=== CHIP ===");
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flashSize = 0;
  esp_flash_get_size(NULL, &flashSize);

  Serial.printf("Model:        %s rev v%d.%d, %d cores @ %lu MHz\n", ESP.getChipModel(),
                chip.revision / 100, chip.revision % 100, chip.cores, ESP.getCpuFreqMHz());
  Serial.printf("Flash:        %lu MB, sketch %lu KB\n", flashSize / (1024 * 1024), ESP.getSketchSize() / 1024);
  Serial.printf("PSRAM:        %lu KB\n", ESP.getPsramSize() / 1024);
  Serial.printf("Free heap:    %lu KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("MAC:          %s\n", WiFi.macAddress().c_str());
  Serial.printf("Temperature:  %.1f C (chip internal)\n", temperatureRead());
  Serial.printf("Reset reason: %s\n", resetReason());
  if (flashSize < 16 * 1024 * 1024) Serial.println("Note: your board has 16MB flash. Set Tools > Flash Size > 16MB.");
}

// =====================================================================
// Memory
// =====================================================================
bool memTest(uint8_t *buf, size_t len, float &writeMBs, float &readMBs) {
  uint32_t *p = (uint32_t *)buf;
  size_t n = len / 4;
  unsigned long t = micros();
  for (size_t i = 0; i < n; i++) p[i] = i * 2654435761u;
  writeMBs = len / (float)(micros() - t);
  t = micros();
  bool ok = true;
  for (size_t i = 0; i < n; i++) {
    if (p[i] != i * 2654435761u) { ok = false; break; }
  }
  readMBs = len / (float)(micros() - t);
  return ok;
}

void testMemory() {
  Serial.println("\n=== MEMORY ===");
  float w, r;
  size_t ramLen = 64 * 1024;
  uint8_t *ram = (uint8_t *)heap_caps_malloc(ramLen, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (ram) {
    bool ok = memTest(ram, ramLen, w, r);
    free(ram);
    setResult(T_RAM, ok ? 1 : -1, "64KB, write " + String(w, 0) + " MB/s, read " + String(r, 0) + " MB/s");
  } else {
    setResult(T_RAM, -1, "could not allocate");
  }

  if (!psramFound()) {
    setResult(T_PSRAM, -1, "not found - set Tools > PSRAM > OPI PSRAM");
    return;
  }
  size_t psLen = (ESP.getFreePsram() - 512 * 1024) & ~3;
  uint8_t *ps = (uint8_t *)ps_malloc(psLen);
  if (!ps) {
    setResult(T_PSRAM, -1, "allocation failed");
    return;
  }
  Serial.printf("Testing %u KB of PSRAM...\n", psLen / 1024);
  bool ok = memTest(ps, psLen, w, r);
  free(ps);
  setResult(T_PSRAM, ok ? 1 : -1, String(ESP.getPsramSize() / 1024 / 1024) + " MB, write " + String(w, 0) + " MB/s, read " + String(r, 0) + " MB/s");
}

// =====================================================================
// Flash storage
// =====================================================================
void testStorage() {
  Serial.println("\n=== FLASH STORAGE ===");
  Preferences prefs;
  if (prefs.begin("s3test", false)) {
    uint32_t boots = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", boots);
    bool ok = prefs.getUInt("boots", 0) == boots;
    prefs.end();
    setResult(T_NVS, ok ? 1 : -1, "boot #" + String(boots) + " (goes up every reset)");
  } else {
    setResult(T_NVS, -1, "could not open");
  }

  if (!LittleFS.begin(true)) {
    setResult(T_FS, -1, "mount failed - pick a Partition Scheme with SPIFFS");
    return;
  }
  const size_t len = 256 * 1024;
  uint8_t *buf = (uint8_t *)malloc(4096);
  File f = LittleFS.open("/test.bin", "w");
  unsigned long t = millis();
  for (size_t off = 0; off < len; off += 4096) {
    for (int i = 0; i < 4096; i++) buf[i] = (off + i) * 7;
    f.write(buf, 4096);
  }
  f.close();
  unsigned long wms = millis() - t;

  f = LittleFS.open("/test.bin", "r");
  bool ok = f && f.size() == len;
  t = millis();
  for (size_t off = 0; ok && off < len; off += 4096) {
    f.read(buf, 4096);
    for (int i = 0; i < 4096; i++) {
      if (buf[i] != (uint8_t)((off + i) * 7)) { ok = false; break; }
    }
  }
  unsigned long rms = millis() - t;
  if (f) f.close();
  LittleFS.remove("/test.bin");
  free(buf);
  setResult(T_FS, ok ? 1 : -1, "256KB file, write " + String(wms) + " ms, read " + String(rms) + " ms");
}

// =====================================================================
// CPU, frequencies, hardware timer
// =====================================================================
volatile float cpuSink;
uint32_t benchMicros(uint32_t iters) {
  unsigned long t = micros();
  float x = 1.0f;
  for (uint32_t i = 0; i < iters; i++) x = x * 1.0000001f + 0.0000001f;
  cpuSink = x;
  return micros() - t;
}

volatile uint32_t coreTime[2];
volatile bool coreDone[2];
void coreTask(void *arg) {
  int c = (int)(intptr_t)arg;
  coreTime[c] = benchMicros(3000000);
  coreDone[c] = true;
  vTaskDelete(NULL);
}

volatile uint32_t timerTicks = 0;
void IRAM_ATTR onTimer() {
  timerTicks = timerTicks + 1;
}

void testCpu() {
  Serial.println("\n=== CPU ===");
  // Both cores at once
  for (int c = 0; c < 2; c++) {
    coreDone[c] = false;
    xTaskCreatePinnedToCore(coreTask, "bench", 4096, (void *)(intptr_t)c, 1, NULL, c);
  }
  unsigned long start = millis();
  while ((!coreDone[0] || !coreDone[1]) && millis() - start < 10000) delay(10);
  if (coreDone[0] && coreDone[1]) {
    setResult(T_CPU, 1, "core0 " + String(coreTime[0] / 1000) + " ms, core1 " + String(coreTime[1] / 1000) + " ms");
  } else {
    setResult(T_CPU, -1, "a core did not finish");
  }

  // Same work at 80 / 160 / 240 MHz. Should get ~3x faster from 80 to 240.
  uint32_t t80 = 0, t160 = 0, t240 = 0;
  int freqs[] = { 80, 160, 240 };
  uint32_t *times[] = { &t80, &t160, &t240 };
  for (int i = 0; i < 3; i++) {
    setCpuFrequencyMhz(freqs[i]);
    delay(20);
    *times[i] = benchMicros(1000000);
  }
  setCpuFrequencyMhz(240);
  float ratio = (float)t80 / t240;
  setResult(T_FREQ, (ratio > 2.4 && ratio < 3.6) ? 1 : -1,
            "80MHz " + String(t80 / 1000) + " ms, 160MHz " + String(t160 / 1000) + " ms, 240MHz " + String(t240 / 1000) + " ms");

  // Hardware timer firing every 1 ms for 1 second
  hw_timer_t *timer = timerBegin(1000000);  // 1 MHz tick
  timerAttachInterrupt(timer, &onTimer);
  timerTicks = 0;
  unsigned long t0 = micros();
  timerAlarm(timer, 1000, true, 0);  // every 1000 ticks = 1 ms
  delay(1000);
  uint32_t ticks = timerTicks;
  unsigned long elapsed = micros() - t0;
  timerEnd(timer);
  float expected = elapsed / 1000.0;
  bool ok = fabs(ticks - expected) <= expected * 0.02;
  setResult(T_TIMER, ok ? 1 : -1, String(ticks) + " interrupts in " + String(elapsed / 1000) + " ms (expect ~1 per ms)");
}

// =====================================================================
// Crypto + random
// =====================================================================
void testCrypto() {
  Serial.println("\n=== HARDWARE CRYPTO + RANDOM ===");
  // SHA-256 known answer: sha256("abc")
  const uint8_t expect[32] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                               0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t *)"abc", 3, hash, 0);
  bool shaOk = memcmp(hash, expect, 32) == 0;

  const size_t len = 64 * 1024;
  uint8_t *a = (uint8_t *)malloc(len), *b = (uint8_t *)malloc(len), *c = (uint8_t *)malloc(len);
  if (!a || !b || !c) {
    setResult(T_SHA, -1, "out of memory");
    free(a); free(b); free(c);
    return;
  }
  esp_fill_random(a, len);
  unsigned long t = micros();
  mbedtls_sha256(a, len, hash, 0);
  float shaMBs = len / (float)(micros() - t);
  setResult(T_SHA, shaOk ? 1 : -1, String(shaOk ? "known answer OK" : "WRONG HASH") + ", " + String(shaMBs, 1) + " MB/s");

  // AES-256-CBC: encrypt then decrypt, must get the original back
  uint8_t key[32], iv[16], iv2[16];
  esp_fill_random(key, 32);
  esp_fill_random(iv, 16);
  memcpy(iv2, iv, 16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 256);
  t = micros();
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, iv, a, b);
  float aesMBs = len / (float)(micros() - t);
  mbedtls_aes_setkey_dec(&ctx, key, 256);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv2, b, c);
  mbedtls_aes_free(&ctx);
  bool aesOk = memcmp(a, c, len) == 0 && memcmp(a, b, len) != 0;
  setResult(T_AES, aesOk ? 1 : -1, "encrypt/decrypt 64KB, " + String(aesMBs, 1) + " MB/s");
  free(a); free(b); free(c);

  // Random: about half the bits should be 1, and every byte value should appear
  uint32_t counts[256] = { 0 };
  uint64_t ones = 0;
  const int words = 25000;  // 100 KB
  for (int i = 0; i < words; i++) {
    uint32_t r = esp_random();
    ones += __builtin_popcount(r);
    for (int k = 0; k < 4; k++) counts[(r >> (k * 8)) & 0xFF]++;
  }
  int missing = 0;
  for (int i = 0; i < 256; i++) if (!counts[i]) missing++;
  float pct = ones * 100.0 / (words * 32.0);
  setResult(T_RNG, (pct > 49.5 && pct < 50.5 && missing == 0) ? 1 : -1, String(pct, 2) + "% ones (expect ~50%)");
}

// =====================================================================
// GPIO pin check: every free pin's internal pull-up and pull-down
// =====================================================================
void testGpio() {
  Serial.println("\n=== GPIO PIN CHECK ===");
  Serial.println("Pulls each free pin up then down. Disconnect anything wired to the pins, except the test jumpers.");
  const int pins[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47 };
  String bad = "";
  int checked = 0;
  for (int p : pins) {
    if (p == rgbPin) continue;
    pinMode(p, INPUT_PULLUP);
    delayMicroseconds(200);
    int up = digitalRead(p);
    pinMode(p, INPUT_PULLDOWN);
    delayMicroseconds(200);
    int down = digitalRead(p);
    pinMode(p, INPUT);
    checked++;
    if (up != 1 || down != 0) bad += "GPIO" + String(p) + "(up=" + String(up) + ",down=" + String(down) + ") ";
  }
  if (bad.length()) setResult(T_GPIO, -1, "problem pins: " + bad + "- check nothing is connected to them");
  else setResult(T_GPIO, 1, String(checked) + " pins OK");
}

// =====================================================================
// Jumper GPIO4 <-> GPIO5: digital, ADC, PWM, interrupt
// =====================================================================
volatile uint32_t edgeCount = 0;
void IRAM_ATTR onEdge() {
  edgeCount = edgeCount + 1;
}

void testLoopback() {
  Serial.printf("\n=== GPIO4 <-> GPIO5 (jumper) ===\n");
  if (!jumperPresent(LOOP_OUT, LOOP_IN)) {
    setResult(T_LOOP, 2, "no jumper GPIO4-GPIO5");
    setResult(T_PWM, 2, "no jumper");
    setResult(T_IRQ, 2, "no jumper");
    return;
  }

  // Digital + ADC
  pinMode(LOOP_OUT, OUTPUT);
  digitalWrite(LOOP_OUT, HIGH);
  delay(2);
  int adcHigh = analogRead(LOOP_IN);
  digitalWrite(LOOP_OUT, LOW);
  delay(2);
  int adcLow = analogRead(LOOP_IN);
  bool ok = adcHigh > 3500 && adcLow < 300;
  setResult(T_LOOP, ok ? 1 : -1, "digital OK, ADC high=" + String(adcHigh) + " low=" + String(adcLow));

  // PWM at 1 kHz with 25/50/75% duty, measured on the other pin
  ledcAttach(LOOP_OUT, 1000, 8);
  pinMode(LOOP_IN, INPUT);
  String info = "";
  bool pwmOk = true;
  int duties[] = { 64, 128, 192 };
  for (int d : duties) {
    ledcWrite(LOOP_OUT, d);
    delay(20);
    unsigned long hi = pulseIn(LOOP_IN, HIGH, 10000);
    unsigned long lo = pulseIn(LOOP_IN, LOW, 10000);
    float measured = (hi + lo) ? hi * 100.0 / (hi + lo) : 0;
    float want = d * 100.0 / 256;
    float freq = (hi + lo) ? 1e6 / (hi + lo) : 0;
    if (fabs(measured - want) > 3 || fabs(freq - 1000) > 50) pwmOk = false;
    info += String(measured, 0) + "% ";
  }
  ledcDetach(LOOP_OUT);
  setResult(T_PWM, pwmOk ? 1 : -1, "duty " + info + "(expect 25 50 75) @ 1kHz");

  // Interrupt: 1000 rising edges should give 1000 interrupts
  pinMode(LOOP_OUT, OUTPUT);
  digitalWrite(LOOP_OUT, LOW);
  pinMode(LOOP_IN, INPUT_PULLDOWN);
  edgeCount = 0;
  attachInterrupt(LOOP_IN, onEdge, RISING);
  for (int i = 0; i < 1000; i++) {
    digitalWrite(LOOP_OUT, HIGH);
    delayMicroseconds(20);
    digitalWrite(LOOP_OUT, LOW);
    delayMicroseconds(20);
  }
  delay(5);
  detachInterrupt(LOOP_IN);
  pinMode(LOOP_OUT, INPUT);
  pinMode(LOOP_IN, INPUT);
  setResult(T_IRQ, edgeCount == 1000 ? 1 : -1, String(edgeCount) + "/1000 edges caught");
}

// =====================================================================
// Jumper GPIO17 <-> GPIO18: UART
// =====================================================================
void testUart() {
  Serial.println("\n=== UART GPIO17 -> GPIO18 (jumper) ===");
  if (!jumperPresent(UART_TX, UART_RX)) {
    setResult(T_UART, 2, "no jumper GPIO17-GPIO18");
    return;
  }
  const char *msg = "The quick brown fox jumps over the lazy dog 0123456789";
  size_t len = strlen(msg);
  long bauds[] = { 115200, 1000000, 2000000 };
  String info = "";
  bool allOk = true;
  for (long baud : bauds) {
    Serial1.begin(baud, SERIAL_8N1, UART_RX, UART_TX);
    delay(20);
    while (Serial1.available()) Serial1.read();
    Serial1.write((const uint8_t *)msg, len);
    Serial1.flush();
    char buf[80] = { 0 };
    size_t got = 0;
    unsigned long t = millis();
    while (got < len && millis() - t < 200) {
      if (Serial1.available()) buf[got++] = Serial1.read();
    }
    Serial1.end();
    bool ok = got == len && memcmp(buf, msg, len) == 0;
    if (!ok) allOk = false;
    info += String(baud) + (ok ? " OK  " : " FAIL  ");
  }
  pinMode(UART_TX, INPUT);
  pinMode(UART_RX, INPUT);
  setResult(T_UART, allOk ? 1 : -1, info);
}

// =====================================================================
// Jumper GPIO11 <-> GPIO13: SPI
// =====================================================================
void testSpi() {
  Serial.println("\n=== SPI MOSI GPIO11 -> MISO GPIO13 (jumper) ===");
  if (!jumperPresent(SPI_MOSI, SPI_MISO)) {
    setResult(T_SPI, 2, "no jumper GPIO11-GPIO13");
    return;
  }
  SPIClass spi(FSPI);
  spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);
  uint8_t tx[256], rx[256];
  for (int i = 0; i < 256; i++) tx[i] = i ^ 0x5A;
  uint32_t speeds[] = { 1000000, 10000000, 20000000 };
  String info = "";
  bool allOk = true;
  for (uint32_t hz : speeds) {
    memset(rx, 0, sizeof(rx));
    spi.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
    spi.transferBytes(tx, rx, 256);
    spi.endTransaction();
    bool ok = memcmp(tx, rx, 256) == 0;
    if (!ok && hz <= 10000000) allOk = false;  // 20 MHz over a loose wire may fail, that's OK
    info += String(hz / 1000000) + "MHz " + (ok ? "OK  " : "FAIL  ");
  }
  spi.end();
  setResult(T_SPI, allOk ? 1 : -1, info);
}

// =====================================================================
// I2C bus scan (SDA GPIO8, SCL GPIO9)
// =====================================================================
void testI2c() {
  Serial.println("\n=== I2C SCAN (SDA GPIO8, SCL GPIO9) ===");
  if (!Wire.begin(I2C_SDA, I2C_SCL, 100000)) {
    setResult(T_I2C, -1, "I2C driver failed to start");
    return;
  }
  String found = "";
  int n = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      found += "0x" + String(addr, HEX) + " ";
      n++;
    }
  }
  Wire.end();
  if (n) setResult(T_I2C, 1, String(n) + " device(s): " + found);
  else setResult(T_I2C, 2, "bus works, no devices connected");
}

// =====================================================================
// RGB LED
// =====================================================================
void testRgb() {
  Serial.printf("\n=== RGB LED (GPIO%d) ===\n", rgbPin);
  struct { const char *name; uint8_t r, g, b; } seq[] = {
    { "RED", 60, 0, 0 }, { "GREEN", 0, 60, 0 }, { "BLUE", 0, 0, 60 }, { "WHITE", 50, 50, 50 }
  };
  for (auto &c : seq) {
    Serial.printf("  %s\n", c.name);
    rgb(c.r, c.g, c.b);
    delay(700);
  }
  for (int h = 0; h < 360; h += 3) {
    int r = h < 120 ? 120 - h : (h >= 240 ? h - 240 : 0);
    int g = h < 120 ? h : (h < 240 ? 240 - h : 0);
    int b = (h >= 120 && h < 240) ? h - 120 : (h >= 240 ? 360 - h : 0);
    rgb(r / 2, g / 2, b / 2);
    delay(12);
  }
  rgb(0, 0, 0);
  tests[T_RGB].state = 2;
  tests[T_RGB].info = "check by eye: red, green, blue, white, rainbow";
  Serial.println("Saw RED, GREEN, BLUE, WHITE, rainbow? Then the LED works.");
  Serial.println("Dark the whole time -> type '8' to try the other LED pin (48 / 38).");
}

// =====================================================================
// BOOT button
// =====================================================================
void testButton(int seconds) {
  Serial.printf("\n=== BOOT BUTTON ===\nPress BOOT within %d seconds (LED is yellow)...\n", seconds);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  rgb(40, 30, 0);
  unsigned long start = millis();
  bool pressed = false;
  while (millis() - start < seconds * 1000UL) {
    if (digitalRead(BOOT_BTN) == LOW) { pressed = true; break; }
    delay(10);
  }
  if (pressed) {
    rgb(0, 60, 0);
    while (digitalRead(BOOT_BTN) == LOW) delay(10);
    setResult(T_BUTTON, 1, "press detected");
  } else {
    setResult(T_BUTTON, 2, "not pressed - type 'k' to retry");
  }
  rgb(0, 0, 0);
}

// =====================================================================
// Touch
// =====================================================================
void testTouch(int seconds) {
  Serial.printf("\n=== TOUCH GPIO1-14, %d seconds ===\n", seconds);
  Serial.println("Touch a pin (or a wire in it). Its number jumps up.");
  uint32_t base[15];
  for (int p = 1; p <= 14; p++) base[p] = touchRead(p);
  Serial.print("      ");
  for (int p = 1; p <= 14; p++) Serial.printf("  G%-4d", p);
  Serial.println();
  unsigned long start = millis();
  while (millis() - start < seconds * 1000UL) {
    Serial.print("delta ");
    for (int p = 1; p <= 14; p++) Serial.printf("%7ld", (long)touchRead(p) - (long)base[p]);
    Serial.println();
    delay(400);
  }
  for (int p = 1; p <= 14; p++) pinMode(p, INPUT);
}

// =====================================================================
// Wi-Fi + web page
// =====================================================================
void testWifiScan() {
  Serial.println("\n=== WI-FI SCAN ===");
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No networks found.");
    return;
  }
  Serial.printf("Found %d networks:\n", n);
  for (int i = 0; i < n && i < 15; i++) {
    Serial.printf("  %2d. %-28s %4d dBm  ch %2d\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  WiFi.scanDelete();
}

void startWifi() {
  Serial.println("\n=== WI-FI CONNECT ===");
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to \"%s\"", WIFI_SSID);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      setResult(T_WIFI, 1, "connected, " + String(WiFi.RSSI()) + " dBm, http://" + WiFi.localIP().toString());
      Serial.printf(">>> Open http://%s <<<\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println("Could not connect, starting hotspot instead.");
  }
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  setResult(T_WIFI, ok ? 1 : -1, "hotspot " + String(AP_SSID) + ", http://192.168.4.1");
  Serial.printf(">>> Join \"%s\" (password %s), open http://192.168.4.1 <<<\n", AP_SSID, AP_PASS);
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 Test</title>
<style>
body{font-family:sans-serif;margin:0;padding:14px;background:#111;color:#eee;max-width:700px}
button{padding:8px 12px;margin:3px;border:0;border-radius:4px;background:#2d6cdf;color:#fff;font-size:14px}
.box{background:#1c1c1c;padding:12px;border-radius:6px;margin-top:12px}
table{border-collapse:collapse;width:100%}td{padding:4px 8px 4px 0;vertical-align:top;font-size:14px}
.PASS{color:#5c5}.FAIL{color:#e55}.SKIP{color:#fc6}.info{color:#aaa}
input[type=color]{width:70px;height:40px;border:0;background:none}
</style></head><body>
<h2>ESP32-S3 Test</h2>
<div class="box"><b>RGB LED</b><br>
<input type="color" value="#00ff00" oninput="set(this.value)">
<button onclick="set('#ff0000')">Red</button><button onclick="set('#00ff00')">Green</button>
<button onclick="set('#0000ff')">Blue</button><button onclick="fetch('/rainbow')">Rainbow</button>
<button onclick="set('#000000')">Off</button></div>
<div class="box"><b>Wi-Fi speed</b> <button onclick="speed()">Run speed test</button> <span id="sp"></span></div>
<div class="box"><b>Live</b><table id="live"></table></div>
<div class="box"><b>Test results</b><table id="t"></table></div>
<script>
function set(h){const n=parseInt(h.slice(1),16);fetch('/rgb?r='+(n>>16&255)+'&g='+(n>>8&255)+'&b='+(n&255))}
async function speed(){const s=document.getElementById('sp');s.textContent='testing...';
 const t0=performance.now();const b=await (await fetch('/speed?x='+Date.now())).arrayBuffer();
 const mbps=(b.byteLength*8/((performance.now()-t0)/1000)/1e6).toFixed(2);
 s.textContent=mbps+' Mbit/s ('+(b.byteLength/1024)+' KB)';fetch('/speedresult?v='+mbps)}
function load(){fetch('/status').then(r=>r.json()).then(s=>{
 let h='';for(const t of s.tests)h+='<tr><td>'+t.n+'</td><td class="'+t.s+'">'+t.s+'</td><td class="info">'+t.i+'</td></tr>';
 document.getElementById('t').innerHTML=h;h='';
 for(const k in s.live)h+='<tr><td>'+k+'</td><td>'+s.live[k]+'</td></tr>';
 document.getElementById('live').innerHTML=h})}
load();setInterval(load,2000);
</script></body></html>
)rawliteral";

void handleStatus() {
  String j = "{\"tests\":[";
  for (int i = 0; i < T_COUNT; i++) {
    if (i) j += ",";
    j += "{\"n\":\"" + String(tests[i].name) + "\",\"s\":\"" + stateText(tests[i].state) + "\",\"i\":\"" + tests[i].info + "\"}";
  }
  String sig = WiFi.getMode() == WIFI_AP ? String(WiFi.softAPgetStationNum()) + " client(s)" : String(WiFi.RSSI()) + " dBm";
  j += "],\"live\":{";
  j += "\"Temperature\":\"" + String(temperatureRead(), 1) + " C\",";
  j += "\"Wi-Fi signal\":\"" + sig + "\",";
  j += "\"Free heap\":\"" + String(ESP.getFreeHeap() / 1024) + " KB\",";
  j += "\"Free PSRAM\":\"" + String(ESP.getFreePsram() / 1024) + " KB\",";
  j += "\"BOOT button\":\"" + String(digitalRead(BOOT_BTN) == LOW ? "pressed" : "released") + "\",";
  j += "\"Uptime\":\"" + String(millis() / 1000) + " s\"}}";
  server.send(200, "application/json", j);
}

void handleSpeed() {
  const size_t total = 1024 * 1024;
  static uint8_t buf[4096];
  server.setContentLength(total);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  WiFiClient client = server.client();
  size_t sent = 0;
  while (sent < total && client.connected()) {
    size_t n = client.write(buf, min((size_t)sizeof(buf), total - sent));
    if (n == 0) break;
    sent += n;
  }
}

void startWebServer() {
  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/status", handleStatus);
  server.on("/speed", handleSpeed);
  server.on("/speedresult", []() {
    tests[T_WIFI].info += " | speed " + server.arg("v") + " Mbit/s";
    Serial.println("[web] Wi-Fi speed " + server.arg("v") + " Mbit/s");
    server.send(200, "text/plain", "ok");
  });
  server.on("/rgb", []() {
    ledMode = MODE_MANUAL;
    rgb(server.arg("r").toInt(), server.arg("g").toInt(), server.arg("b").toInt());
    server.send(200, "text/plain", "ok");
  });
  server.on("/rainbow", []() {
    ledMode = MODE_RAINBOW;
    server.send(200, "text/plain", "ok");
  });
  server.begin();
  Serial.println("Web server started.");
}

// =====================================================================
// Bluetooth LE
// =====================================================================
void testBle() {
  Serial.println("\n=== BLUETOOTH LE ===");
  if (!bleReady) {
    BLEDevice::init("ESP32-S3-Test");
    bleReady = true;
  }
  BLEDevice::stopAdvertising();
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  Serial.println("Scanning 5 seconds...");
  BLEScanResults *found = scan->start(5, false);
  int n = found ? found->getCount() : 0;
  for (int i = 0; i < n && i < 10; i++) {
    BLEAdvertisedDevice d = found->getDevice(i);
    Serial.printf("  %s  %4d dBm  %s\n", d.getAddress().toString().c_str(), d.getRSSI(), d.haveName() ? d.getName().c_str() : "");
  }
  scan->clearResults();

  // Advertise so you can find the board from your phone
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  setResult(T_BLE, n > 0 ? 1 : 2, String(n) + " devices seen; advertising as ESP32-S3-Test");
  Serial.println("Check with the nRF Connect or LightBlue app on your phone: look for 'ESP32-S3-Test'.");
}

// =====================================================================
// Sleep
// =====================================================================
void testLightSleep() {
  Serial.println("\n=== LIGHT SLEEP 2 s ===");
  Serial.println("(USB serial may pause for a moment.)");
  Serial.flush();
  delay(50);
  esp_sleep_enable_timer_wakeup(2000000ULL);
  unsigned long t = millis();
  esp_light_sleep_start();
  unsigned long slept = millis() - t;
  bool ok = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && slept >= 1900 && slept < 3000;
  delay(500);  // let USB reconnect
  setResult(T_LSLEEP, ok ? 1 : -1, "slept " + String(slept) + " ms, woke by timer");
}

void testDeepSleep() {
  Serial.println("\n=== DEEP SLEEP 5 s ===");
  Serial.println("Board resets on wake. Serial Monitor will disconnect; it should reconnect by itself,");
  Serial.println("otherwise reopen it. Look for 'Woke up from deep sleep'.");
  Serial.flush();
  delay(100);
  deepSleepCount++;
  rgb(0, 0, 0);
  esp_sleep_enable_timer_wakeup(5000000ULL);
  esp_deep_sleep_start();
}

void reportWake() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    Serial.printf("\n*** Woke up from deep sleep by timer (count %d) ***\n", deepSleepCount);
    tests[T_DSLEEP].state = 1;
    tests[T_DSLEEP].info = "woke by timer, RTC memory kept (count " + String(deepSleepCount) + ")";
  }
}

// =====================================================================
// Summary + menu
// =====================================================================
void printSummary() {
  int pass = 0, fail = 0, skip = 0;
  Serial.println("\n================ SUMMARY ================");
  for (auto &t : tests) {
    Serial.printf("  %-18s %-4s  %s\n", t.name, stateText(t.state), t.info.c_str());
    if (t.state == 1) pass++;
    else if (t.state == -1) fail++;
    else skip++;
  }
  Serial.printf("  PASS %d   FAIL %d   SKIP/not run %d\n", pass, fail, skip);
  Serial.println("=========================================");
}

void printMenu() {
  Serial.println("\nType a letter + Enter:");
  Serial.println("  a  all automatic tests       x  summary");
  Serial.println("  i  chip info                 m  RAM + PSRAM");
  Serial.println("  s  flash storage             c  CPU, frequencies, timer");
  Serial.println("  y  crypto + random           o  GPIO pin check");
  Serial.println("  g  GPIO4<->5 jumper tests    u  UART 17<->18 jumper");
  Serial.println("  p  SPI 11<->13 jumper        2  I2C scan");
  Serial.println("  l  RGB LED                   8  switch RGB pin 48/38");
  Serial.println("  k  BOOT button               t  touch pins (15 s)");
  Serial.println("  w  Wi-Fi scan + connect      e  Bluetooth scan");
  Serial.println("  n  light sleep               d  deep sleep");
  Serial.println("  r  restart                   h  this menu");
}

void runAuto() {
  testChip();
  testMemory();
  testStorage();
  testCpu();
  testCrypto();
  testGpio();
  testLoopback();
  testUart();
  testSpi();
  testI2c();
  testBle();
  printSummary();
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = tolower(Serial.read());
  switch (c) {
    case 'a': runAuto(); break;
    case 'x': printSummary(); break;
    case 'i': testChip(); break;
    case 'm': testMemory(); break;
    case 's': testStorage(); break;
    case 'c': testCpu(); break;
    case 'y': testCrypto(); break;
    case 'o': testGpio(); break;
    case 'g': testLoopback(); break;
    case 'u': testUart(); break;
    case 'p': testSpi(); break;
    case '2': testI2c(); break;
    case 'l': ledMode = MODE_MANUAL; testRgb(); ledMode = MODE_STATUS; break;
    case '8':
      rgb(0, 0, 0);
      rgbPin = (rgbPin == 48) ? 38 : 48;
      Serial.printf("RGB pin is now GPIO%d\n", rgbPin);
      ledMode = MODE_MANUAL; testRgb(); ledMode = MODE_STATUS;
      break;
    case 'k': testButton(10); break;
    case 't': testTouch(15); break;
    case 'w': testWifiScan(); startWifi(); break;
    case 'e': testBle(); break;
    case 'n': testLightSleep(); break;
    case 'd': testDeepSleep(); break;
    case 'r': ESP.restart(); break;
    case 'h': printMenu(); break;
    default: return;
  }
}

// Idle LED: breathing green = no failures, breathing red = something failed
void updateLed() {
  static unsigned long last = 0;
  static int step = 0;
  if (millis() - last < 20) return;
  last = millis();
  step = (step + 1) % 360;

  if (ledMode == MODE_RAINBOW) {
    int h = step;
    int r = h < 120 ? 120 - h : (h >= 240 ? h - 240 : 0);
    int g = h < 120 ? h : (h < 240 ? 240 - h : 0);
    int b = (h >= 120 && h < 240) ? h - 120 : (h >= 240 ? 360 - h : 0);
    rgb(r / 3, g / 3, b / 3);
  } else if (ledMode == MODE_STATUS) {
    bool fail = false;
    for (auto &t : tests) if (t.state == -1) fail = true;
    uint8_t v = 2 + (uint8_t)(18 * (0.5 + 0.5 * sin(step * PI / 180)));
    fail ? rgb(v, 0, 0) : rgb(0, v, 0);
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) delay(10);  // wait for Serial Monitor
  delay(500);

  Serial.println("\n\n########## ESP32-S3 COMPLETE BOARD TEST ##########");
  reportWake();

  testChip();
  testMemory();
  testStorage();
  testCpu();
  testCrypto();
  testGpio();
  testLoopback();
  testUart();
  testSpi();
  testI2c();
  testRgb();
  testButton(8);
  testWifiScan();
  startWifi();
  startWebServer();
  testBle();
  printSummary();
  printMenu();
}

void loop() {
  server.handleClient();
  handleSerial();
  updateLed();
  delay(2);
}
