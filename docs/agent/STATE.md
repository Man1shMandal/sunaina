# Project Sunaina - state

Last updated: 2026-11-10

## Camera arm positions (measured on the rig, 2026-11-10)

| View  | Servo angle | Camera |
|-------|-------------|--------|
| Front | 0 deg       | level with the product, looking at the front face |
| Top   | 63 deg      | arm up, looking straight down |
| Back  | 120 deg     | level with the product, looking at the back face |

- Default arm speed: 120 deg/s (mid speed, about 0.5 s between views).
- These are servo angles with the current calibration (showcase_s3, min/max pulse). If the pulse
  calibration changes, re-measure these angles.
- In code: VIEW_FRONT / VIEW_TOP / VIEW_BACK and DEFAULT_SPEED at the top of showcase_s3.ino.

## Hardware findings

### MG995 servo
- Supply 4.8-7.2 V (5-6 V normal). ~1 A moving, up to 2.5 A stall.
- Do NOT power it from the ESP32/S3 5V or VIN pin. USB gives ~0.5 A, not enough.
- Use a separate 5-6 V, 2 A+ supply. Tie its GND to the board GND.
- 470-1000 uF capacitor across servo + / - near the servo.
- Signal: 3.3 V PWM from the board is fine (S3 uses GPIO4).
- Angles / full range (learned 2026-11-10):
  - The angle you see depends on the pulse width (us) the code maps 0 and 180 deg to, not on the number 180.
  - 1000-2000 us (the common "standard servo" range) only moves the MG995 ~90 deg.
  - Full ~180 deg needs ~500-2500 us. servo_web uses this by default; servo_test.ino still uses 1000-2000.
  - Map: us = MIN_US + angle * (MAX_US - MIN_US) / 180, 50 Hz PWM (20 ms period).
  - Calibrate each servo: raw pulse slider (servo_web) or p500 / p2500 (servo_test). Push toward each end
    until it stops moving or buzzes, back off ~20 us, set as min/max, Apply, Save.
  - Buzzing / shaking / heat at an end = hitting the internal stop: stall current (~2.5 A), gear wear.
    Bring that end in (e.g. 550 / 2450). Some MG995 clones only reach ~160-170 deg.
  - Some MG995s go past 500 us at the 0 end. showcase_s3 allows 300-2700 us: find the real end with the raw
    slider, press "Use as 0 deg" / "Use as 180 deg", Apply, Save all settings.

### ESP32 / ESP32-S3 pins
- GPIOs are 3.3 V only. Never put 5 V on a GPIO.
- 5V / VIN pin is a power input (5 V). As an output it only passes USB 5 V (~0.5 A).
- Classic ESP32: never touch GPIO 6-11 (flash). Doing so resets the board in a loop.
- Classic ESP32 strapping pins to avoid: 0, 2, 5, 12, 15. 34-39 are input only.

### 1.3" I2C OLED (128x64, "1.30 IIC v3.1")
- Working on the classic ESP32: VCC->3V3, GND->GND, SDA->D21, SCL->D22, address 0x3C.
- Not working on the S3 yet (S3 firmware expects SDA GPIO8, SCL GPIO9). Same module works on the ESP32,
  so suspect wiring/contact on the S3 side. spider_anim_s3 now prints SDA/SCL line levels and tries both pin orders.
- S3 RGB LED (GPIO48) WORKS when the board is set to "ESP32S3 Dev Module" (esp32 by Espressif).
  With the auto-picked board it stayed green. Likely cause: IDE picked "Arduino Nano ESP32", whose default
  "Pin Numbering: By Arduino pin" remaps numbers (48 -> nothing, 8/9 -> GPIO17/18). Not yet confirmed which
  board was auto-picked. The same remap would explain the OLED "not found" on GPIO8/9.
- Controller: TODO confirm SH1106 or SSD1306 (main firmware assumes SSD1306).
- OLED is now mounted upside down: showcase_s3 rotates 180 deg (setRotation(2)), toggle "Flip screen".
- RGB stuck on one colour in showcase_s3 while OLED worked: LED was written from the core-0 task. Now the task only
  picks the colour and loop() (core 1) writes it, like ESPS3test. User reports still stuck after this.
  Next suspect: LED is on GPIO38 not 48 (ESPS3test has '8' to swap). showcase_s3 now has "LED test" +
  "LED pin 48/38" on the page (Serial: t = test, 8 = swap). Waiting on result.
  When RGB HAS worked: only ESPS3test uploaded with board picked manually = "ESP32S3 Dev Module" (esp32 by
  Espressif) + its settings (Flash 16MB, PSRAM OPI, USB CDC On Boot Enabled, 8M with spiffs). Same code with the
  auto-detected board = stuck green. So suspect board/settings first. showcase_s3 now #errors unless
  ARDUINO_ESP32S3_DEV and prints the board name at boot.
  User now sees constant YELLOW. Debugging with tests/led_debug_s3 (LED only).
- Blank screen but found on I2C = wrong driver or panel not powered on. Use tests/oled_raw_esp32.

## Tests (tests/)
| Sketch | Board | Purpose | Status |
|---|---|---|---|
| servo_test | S3 | MG995 sweep loop + serial commands | written, needs external supply |
| servo_web | S3 | MG995 web control: angle, speed, loop A-B + pause, sweep, stop, release, calibration | written |
| showcase_s3 | S3 | servo web control + OLED animations + RGB (Sync/Manual/Off), OLED flip, display task on core 0 | written, RGB fix untested |
| led_debug_s3 | S3 | LED only: board/core info, driver vs bit-bang, colour order, ramp, plain blink, pin scan | written |
| i2c_scan | S3 or ESP32 | scan every safe pin pair | written |
| serial_test_esp32 | ESP32 | basic serial / LED check | works |
| oled_test_esp32 | ESP32 | OLED via Adafruit SH110X, D21/D22 | detected, was blank |
| oled_raw_esp32 | ESP32 | library-free OLED diagnostic | OLED working |
| oled_test | S3 | OLED via Adafruit SH110X, GPIO8/9 | not working yet |
| spider_anim_esp32 | ESP32 | comic-style spider animation demo | written |
| spider_anim_s3 | S3 | same animation + RGB LED (GPIO48) synced, SDA GPIO8 / SCL GPIO9 | written |

Arduino IDE board names: "ESP32 Dev Module" (classic), "ESP32S3 Dev Module" (S3: Flash 16MB, PSRAM OPI,
USB CDC On Boot Enabled). ALWAYS pick the S3 board manually, never the auto-detected one. Serial 115200.

## Next
- Confirm OLED controller (SH1106 vs SSD1306) and fix USE_SH1106 / main firmware.
- Re-test OLED on the S3 with ESP32S3 Dev Module selected (spider_anim_s3).
- Servo test with external supply.
