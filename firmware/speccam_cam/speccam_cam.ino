/*
  SpecCam - camera node (AI-Thinker ESP32-CAM)

  The CAM is the "eyes". It takes commands from the ESP32-S3 over a UART wire
  and sends back JPEG frames. No Wi-Fi here: one wire, predictable timing.

  Wiring (CAM -> S3):
    GPIO14 (TX)  -> S3 GPIO18 (RX)
    GPIO15 (RX)  <- S3 GPIO17 (TX)
    GND          -  GND
    5V           -  5V   (S3 board's 5V pin can power it)
  Leave the SD card slot empty (pins 14/15 are shared with it).

  Commands (one per line, from the S3):
    PING                 -> "OK PONG <sensor id>"
    CAP                  -> binary frame: A5 5A 'F' len(4, LE) sum(4, LE) <jpeg bytes>
    SET <key> <value>    -> "OK <key> <value>" or "ERR <key>"
       keys: framesize quality autoexp aec autogain gain brightness contrast
             saturation awb hmirror vflip flash(0-255)

  Arduino IDE: Board "AI Thinker ESP32-CAM", Partition "Huge APP".
*/

#include "esp_camera.h"

#define LINK_TX 14
#define LINK_RX 15
#define LINK_BAUD 2000000
#define FLASH_PIN 4

// AI-Thinker camera pins
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

HardwareSerial &camLink = Serial2;
bool camOk = false;
char line[96];
size_t lineLen = 0;

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_QVGA;  // 320x240: fast over the wire, enough for inspection
  c.jpeg_quality = 10;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (psramFound()) {
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }
  if (esp_camera_init(&c) != ESP_OK) return false;
  return true;
}

void setFlash(int v) {
  ledcWrite(FLASH_PIN, constrain(v, 0, 255));
}

void sendFrame() {
  if (!camOk) {
    camLink.print("ERR NOCAM\n");
    return;
  }
  // Drop the buffered frame so settings changes (exposure, flash) are visible
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  if (!fb) {
    camLink.print("ERR CAP\n");
    return;
  }
  uint32_t len = fb->len, sum = 0;
  for (size_t i = 0; i < fb->len; i++) sum += fb->buf[i];
  uint8_t hdr[11] = { 0xA5, 0x5A, 'F',
                      (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                      (uint8_t)sum, (uint8_t)(sum >> 8), (uint8_t)(sum >> 16), (uint8_t)(sum >> 24) };
  camLink.write(hdr, sizeof(hdr));
  camLink.write(fb->buf, fb->len);
  camLink.flush();
  esp_camera_fb_return(fb);
}

bool applySetting(const char *k, int v) {
  if (!strcmp(k, "flash")) {
    setFlash(v);
    return true;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return false;
  if (!strcmp(k, "framesize")) return s->set_framesize(s, (framesize_t)v) == 0;
  if (!strcmp(k, "quality")) return s->set_quality(s, v) == 0;
  if (!strcmp(k, "autoexp")) return s->set_exposure_ctrl(s, v) == 0;
  if (!strcmp(k, "aec")) {
    s->set_exposure_ctrl(s, 0);
    return s->set_aec_value(s, v) == 0;
  }
  if (!strcmp(k, "autogain")) return s->set_gain_ctrl(s, v) == 0;
  if (!strcmp(k, "gain")) {
    s->set_gain_ctrl(s, 0);
    return s->set_agc_gain(s, v) == 0;
  }
  if (!strcmp(k, "brightness")) return s->set_brightness(s, v) == 0;
  if (!strcmp(k, "contrast")) return s->set_contrast(s, v) == 0;
  if (!strcmp(k, "saturation")) return s->set_saturation(s, v) == 0;
  if (!strcmp(k, "awb")) return s->set_whitebal(s, v) == 0;
  if (!strcmp(k, "hmirror")) return s->set_hmirror(s, v) == 0;
  if (!strcmp(k, "vflip")) return s->set_vflip(s, v) == 0;
  return false;
}

void handleLine(char *cmd) {
  if (!strcmp(cmd, "CAP")) {
    sendFrame();
  } else if (!strcmp(cmd, "PING")) {
    sensor_t *s = camOk ? esp_camera_sensor_get() : NULL;
    camLink.printf("OK PONG %s 0x%x\n", camOk ? "CAM" : "NOCAM", s ? s->id.PID : 0);
  } else if (!strncmp(cmd, "SET ", 4)) {
    char key[16];
    int val;
    if (sscanf(cmd + 4, "%15s %d", key, &val) == 2 && applySetting(key, val)) {
      camLink.printf("OK %s %d\n", key, val);
      Serial.printf("set %s = %d\n", key, val);
    } else {
      camLink.printf("ERR %s\n", cmd + 4);
    }
  } else if (strlen(cmd)) {
    camLink.print("ERR UNKNOWN\n");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nSpecCam camera node");

  ledcAttachChannel(FLASH_PIN, 5000, 8, 7);  // channel 7 so it doesn't clash with the camera clock
  setFlash(0);

  camOk = initCamera();
  Serial.println(camOk ? "Camera OK" : "Camera FAILED (check ribbon cable)");

  camLink.setRxBufferSize(1024);
  camLink.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  camLink.print("OK BOOT\n");
}

void loop() {
  while (camLink.available()) {
    char ch = camLink.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[lineLen] = 0;
      handleLine(line);
      lineLen = 0;
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = ch;
    }
  }
  delay(1);
}
