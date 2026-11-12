/*
  ESP32-CAM full test sketch (AI-Thinker board)

  What it tests:
    1. Chip info, flash size, PSRAM, free memory
    2. Red status LED (GPIO33) and white flash LED (GPIO4)
    3. Camera: init, sensor model, single capture, FPS benchmark
    4. microSD card: mount, write, read back, delete, save a photo
    5. Wi-Fi: scan nearby networks, connect (or start its own hotspot)
    6. Web page: live video, snapshot, flash, resolution/quality/image controls

  Arduino IDE settings:
    Board:            "AI Thinker ESP32-CAM"
    Partition scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
    Serial Monitor:   115200 baud

  Usage:
    - Put your Wi-Fi name and password below. If left empty, or if it can't
      connect, the board makes its own hotspot "ESP32-CAM-Test" (password
      12345678) and the page is at http://192.168.4.1
    - Open the Serial Monitor and type a letter to run a test (h = help).
*/

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include <WiFi.h>
#include <FS.h>
#include <SD_MMC.h>

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

// Hotspot used when Wi-Fi above is empty or fails
const char *AP_SSID = "ESP32-CAM-Test";
const char *AP_PASS = "12345678";

// ---------- AI-Thinker ESP32-CAM pins ----------
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

#define FLASH_LED 4   // white LED, HIGH = on
#define RED_LED 33    // small red LED on the back, LOW = on

// ---------- Test results ----------
struct Results {
  bool psram = false;
  bool camera = false;
  bool capture = false;
  bool sd = false;
  bool wifi = false;
  bool apMode = false;
  String sensor = "unknown";
  String sdInfo = "not tested";
  float fps = 0;
  size_t lastJpegBytes = 0;
} results;

bool flashOn = false;
int photoCounter = 0;
httpd_handle_t webServer = NULL;
httpd_handle_t streamServer = NULL;
volatile float streamFps = 0;

// =====================================================================
// 1. System info
// =====================================================================
void testSystem() {
  Serial.println("\n=== SYSTEM ===");
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flashSize = 0;
  esp_flash_get_size(NULL, &flashSize);

  Serial.printf("Chip:        %s rev %d, %d cores @ %lu MHz\n",
                ESP.getChipModel(), chip.revision, chip.cores, ESP.getCpuFreqMHz());
  Serial.printf("Flash:       %lu MB\n", flashSize / (1024 * 1024));
  Serial.printf("Free heap:   %lu bytes\n", ESP.getFreeHeap());

  results.psram = psramFound();
  if (results.psram) {
    Serial.printf("PSRAM:       OK, %lu bytes (free %lu)\n", ESP.getPsramSize(), ESP.getFreePsram());
  } else {
    Serial.println("PSRAM:       NOT FOUND (only low resolutions will work)");
  }
  Serial.printf("MAC:         %s\n", WiFi.macAddress().c_str());
}

// =====================================================================
// 2. LEDs
// =====================================================================
void setFlash(bool on) {
  flashOn = on;
  digitalWrite(FLASH_LED, on ? HIGH : LOW);
}

void testLeds() {
  Serial.println("\n=== LEDs ===");
  Serial.println("Red LED (back of board) blinking 3 times...");
  for (int i = 0; i < 3; i++) {
    digitalWrite(RED_LED, LOW);
    delay(200);
    digitalWrite(RED_LED, HIGH);
    delay(200);
  }
  Serial.println("Flash LED blinking 2 times (short, it's bright)...");
  for (int i = 0; i < 2; i++) {
    setFlash(true);
    delay(80);
    setFlash(false);
    delay(250);
  }
  Serial.println("Did you see both LEDs? If yes, LEDs OK.");
}

// =====================================================================
// 3. Camera
// =====================================================================
bool initCamera() {
  Serial.println("\n=== CAMERA INIT ===");
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;  // start at 640x480, change from web page
    config.jpeg_quality = 12;           // 0-63, lower = better quality
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED, error 0x%x\n", err);
    Serial.println("Check: ribbon cable seated & latched, correct board selected, 5V supply.");
    results.camera = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  switch (s->id.PID) {
    case OV2640_PID: results.sensor = "OV2640"; break;
    case OV3660_PID: results.sensor = "OV3660"; break;
    case OV5640_PID: results.sensor = "OV5640"; break;
    default: results.sensor = "PID 0x" + String(s->id.PID, HEX); break;
  }
  Serial.printf("Camera OK, sensor: %s\n", results.sensor.c_str());
  results.camera = true;
  return true;
}

void testCapture() {
  Serial.println("\n=== SINGLE CAPTURE ===");
  if (!results.camera) {
    Serial.println("Skipped, camera not initialised.");
    return;
  }
  // First frames after init are often dark/green while auto-exposure settles
  for (int i = 0; i < 3; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  unsigned long t = millis();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture FAILED (no frame).");
    results.capture = false;
    return;
  }
  Serial.printf("Captured %ux%u JPEG, %u bytes in %lu ms\n",
                fb->width, fb->height, fb->len, millis() - t);
  results.capture = true;
  results.lastJpegBytes = fb->len;
  esp_camera_fb_return(fb);
}

void testFps() {
  Serial.println("\n=== FPS BENCHMARK (30 frames) ===");
  if (!results.camera) {
    Serial.println("Skipped, camera not initialised.");
    return;
  }
  const int frames = 30;
  size_t totalBytes = 0;
  unsigned long t = millis();
  for (int i = 0; i < frames; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Frame grab failed during benchmark.");
      return;
    }
    totalBytes += fb->len;
    esp_camera_fb_return(fb);
  }
  unsigned long ms = millis() - t;
  results.fps = frames * 1000.0 / ms;
  Serial.printf("%.1f FPS, average frame %u bytes\n", results.fps, totalBytes / frames);
}

// =====================================================================
// 4. microSD card
// =====================================================================
void testSd() {
  Serial.println("\n=== SD CARD ===");
  // 1-bit mode so GPIO4 (flash LED) isn't used as an SD data line
  SD_MMC.end();
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("No SD card / mount failed. (Card must be FAT32, max 32GB is safest.)");
    results.sd = false;
    results.sdInfo = "not found";
    pinMode(FLASH_LED, OUTPUT);
    setFlash(false);
    return;
  }
  pinMode(FLASH_LED, OUTPUT);
  setFlash(false);

  uint8_t type = SD_MMC.cardType();
  const char *typeName = type == CARD_MMC ? "MMC" : type == CARD_SD ? "SDSC" : type == CARD_SDHC ? "SDHC" : "UNKNOWN";
  uint64_t sizeMB = SD_MMC.cardSize() / (1024 * 1024);
  uint64_t usedMB = SD_MMC.usedBytes() / (1024 * 1024);
  Serial.printf("Card: %s, %llu MB, used %llu MB\n", typeName, sizeMB, usedMB);

  // Write, read back, compare, delete
  const char *path = "/esp32cam_test.txt";
  String msg = "ESP32-CAM SD test " + String(millis());
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    Serial.println("Write FAILED (could not open file).");
    results.sd = false;
    results.sdInfo = "write failed";
    return;
  }
  f.print(msg);
  f.close();

  f = SD_MMC.open(path, FILE_READ);
  String readBack = f ? f.readString() : "";
  if (f) f.close();
  SD_MMC.remove(path);

  results.sd = (readBack == msg);
  Serial.printf("Write/read back: %s\n", results.sd ? "OK" : "MISMATCH");
  results.sdInfo = String(typeName) + ", " + String((uint32_t)sizeMB) + " MB";

  // Continue photo numbering from what's already on the card
  photoCounter = 0;
  while (SD_MMC.exists("/photo_" + String(photoCounter + 1) + ".jpg")) photoCounter++;
}

String savePhotoToSd() {
  if (!results.sd) return "SD card not available";
  if (!results.camera) return "Camera not available";
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return "Capture failed";
  photoCounter++;
  String path = "/photo_" + String(photoCounter) + ".jpg";
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    esp_camera_fb_return(fb);
    return "Could not open file on SD";
  }
  f.write(fb->buf, fb->len);
  f.close();
  String out = "Saved " + path + " (" + String(fb->len) + " bytes)";
  esp_camera_fb_return(fb);
  return out;
}

// =====================================================================
// 5. Wi-Fi
// =====================================================================
void testWifiScan() {
  Serial.println("\n=== WI-FI SCAN ===");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No networks found. (Is the antenna jumper set to the onboard antenna?)");
    return;
  }
  Serial.printf("Found %d networks:\n", n);
  for (int i = 0; i < n && i < 15; i++) {
    Serial.printf("  %2d. %-28s  %4d dBm  ch %2d\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  WiFi.scanDelete();
}

void startWifi() {
  Serial.println("\n=== WI-FI CONNECT ===");
  WiFi.setSleep(false);  // smoother video
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
      results.wifi = true;
      Serial.printf("Connected. Signal %d dBm\n", WiFi.RSSI());
      Serial.printf(">>> Open http://%s in your browser <<<\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println("Could not connect, starting hotspot instead.");
  } else {
    Serial.println("No Wi-Fi set in code, starting hotspot.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  results.apMode = true;
  results.wifi = true;
  Serial.printf("Hotspot \"%s\" (password %s)\n", AP_SSID, AP_PASS);
  Serial.printf(">>> Join it, then open http://%s <<<\n", WiFi.softAPIP().toString().c_str());
}

// =====================================================================
// 6. Web server
// =====================================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM Test</title>
<style>
body{font-family:sans-serif;margin:0;padding:12px;background:#111;color:#eee;max-width:900px}
img{width:100%;background:#222;border-radius:6px}
button,select{padding:8px 12px;margin:3px;border:0;border-radius:4px;background:#2d6cdf;color:#fff;font-size:14px}
label{display:block;margin:8px 0 2px}input[type=range]{width:100%}
.row{display:flex;flex-wrap:wrap;gap:4px;align-items:center}
.box{background:#1c1c1c;padding:10px;border-radius:6px;margin-top:10px}
td{padding:2px 10px 2px 0}.ok{color:#5c5}.bad{color:#e55}#msg{color:#fc6;min-height:1.2em}
</style></head><body>
<h2>ESP32-CAM Test</h2>
<img id="view" alt="camera">
<div class="row">
<button onclick="startStream()">Start stream</button>
<button onclick="stopStream()">Stop stream</button>
<button onclick="snap()">Snapshot</button>
<button onclick="cmd('flash',flash?0:1).then(()=>flash=!flash)">Flash on/off</button>
<button onclick="save()">Save to SD</button>
<a id="dl" style="color:#9cf;margin-left:6px">Download snapshot</a>
</div>
<div id="msg"></div>
<div class="box">
<label>Resolution</label>
<select onchange="cmd('framesize',this.value)">
<option value="5">QVGA 320x240</option><option value="8" selected>VGA 640x480</option>
<option value="9">SVGA 800x600</option><option value="10">XGA 1024x768</option>
<option value="11">HD 1280x720</option><option value="12">SXGA 1280x1024</option>
<option value="13">UXGA 1600x1200</option></select>
<label>JPEG quality (left = better)</label><input type="range" min="4" max="40" value="12" onchange="cmd('quality',this.value)">
<label>Brightness</label><input type="range" min="-2" max="2" value="0" onchange="cmd('brightness',this.value)">
<label>Contrast</label><input type="range" min="-2" max="2" value="0" onchange="cmd('contrast',this.value)">
<label>Saturation</label><input type="range" min="-2" max="2" value="0" onchange="cmd('saturation',this.value)">
<label>Special effect</label>
<select onchange="cmd('effect',this.value)"><option value="0">None</option><option value="1">Negative</option>
<option value="2">Grayscale</option><option value="3">Red tint</option><option value="4">Green tint</option>
<option value="5">Blue tint</option><option value="6">Sepia</option></select>
<div class="row" style="margin-top:8px">
<label><input type="checkbox" onchange="cmd('hmirror',this.checked?1:0)"> Mirror</label>
<label><input type="checkbox" onchange="cmd('vflip',this.checked?1:0)"> Flip</label>
<label><input type="checkbox" onchange="cmd('colorbar',this.checked?1:0)"> Test colour bars</label>
</div></div>
<div class="box"><b>Self-test results</b><table id="st"></table></div>
<script>
const host=location.hostname;let flash=false;
const v=document.getElementById('view'),m=document.getElementById('msg');
function startStream(){v.src='http://'+host+':81/stream';m.textContent='Streaming...'}
function stopStream(){v.src='';m.textContent='Stream stopped'}
function snap(){stopStream();const u='/capture?t='+Date.now();v.src=u;
 const d=document.getElementById('dl');d.href=u;d.download='esp32cam.jpg';m.textContent='Snapshot taken'}
function cmd(k,val){return fetch('/control?var='+k+'&val='+val).then(r=>r.text()).then(t=>m.textContent=t)}
function save(){fetch('/save').then(r=>r.text()).then(t=>m.textContent=t)}
function status(){fetch('/status').then(r=>r.json()).then(s=>{
 const row=(k,v,ok)=>'<tr><td>'+k+'</td><td class="'+(ok===undefined?'':ok?'ok':'bad')+'">'+v+'</td></tr>';
 document.getElementById('st').innerHTML=
  row('Chip',s.chip)+row('PSRAM',s.psram?'OK':'missing',s.psram)+
  row('Camera',s.camera?'OK ('+s.sensor+')':'FAILED',s.camera)+row('Capture',s.capture?'OK':'FAILED',s.capture)+
  row('Benchmark FPS',s.fps.toFixed(1))+row('Stream FPS',s.streamFps.toFixed(1))+
  row('SD card',s.sdInfo,s.sd)+row('Wi-Fi',s.wifi,true)+row('Signal',s.rssi+' dBm')+
  row('Free heap',s.heap+' B')+row('Free PSRAM',s.freePsram+' B')+row('Uptime',s.uptime+' s')})}
status();setInterval(status,3000);startStream();
</script></body></html>
)rawliteral";

esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  results.lastJpegBytes = fb->len;
  esp_camera_fb_return(fb);
  return res;
}

esp_err_t controlHandler(httpd_req_t *req) {
  char query[64], var[24], val[12];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    return ESP_FAIL;
  }
  int v = atoi(val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(var, "flash")) setFlash(v);
  else if (!s) res = -1;
  else if (!strcmp(var, "framesize")) res = s->set_framesize(s, (framesize_t)v);
  else if (!strcmp(var, "quality")) res = s->set_quality(s, v);
  else if (!strcmp(var, "brightness")) res = s->set_brightness(s, v);
  else if (!strcmp(var, "contrast")) res = s->set_contrast(s, v);
  else if (!strcmp(var, "saturation")) res = s->set_saturation(s, v);
  else if (!strcmp(var, "effect")) res = s->set_special_effect(s, v);
  else if (!strcmp(var, "hmirror")) res = s->set_hmirror(s, v);
  else if (!strcmp(var, "vflip")) res = s->set_vflip(s, v);
  else if (!strcmp(var, "colorbar")) res = s->set_colorbar(s, v);
  else res = -1;

  String msg = String(var) + " = " + String(val) + (res == 0 ? " OK" : " FAILED");
  Serial.println("[web] " + msg);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, msg.c_str());
}

esp_err_t saveHandler(httpd_req_t *req) {
  String msg = savePhotoToSd();
  Serial.println("[web] " + msg);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, msg.c_str());
}

esp_err_t statusHandler(httpd_req_t *req) {
  String wifi = results.apMode ? "Hotspot " + String(AP_SSID) : "Connected to " + String(WIFI_SSID);
  int rssi = results.apMode ? 0 : WiFi.RSSI();
  String json = "{";
  json += "\"chip\":\"" + String(ESP.getChipModel()) + "\",";
  json += "\"psram\":" + String(results.psram ? "true" : "false") + ",";
  json += "\"camera\":" + String(results.camera ? "true" : "false") + ",";
  json += "\"capture\":" + String(results.capture ? "true" : "false") + ",";
  json += "\"sensor\":\"" + results.sensor + "\",";
  json += "\"fps\":" + String(results.fps, 1) + ",";
  json += "\"streamFps\":" + String(streamFps, 1) + ",";
  json += "\"sd\":" + String(results.sd ? "true" : "false") + ",";
  json += "\"sdInfo\":\"" + results.sdInfo + "\",";
  json += "\"wifi\":\"" + wifi + "\",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"freePsram\":" + String(ESP.getFreePsram()) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json.c_str());
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

esp_err_t streamHandler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  Serial.println("[web] stream started");

  char part[64];
  unsigned long last = millis();
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[web] stream: capture failed");
      res = ESP_FAIL;
      break;
    }
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;

    unsigned long now = millis();
    float instant = 1000.0 / max(1UL, now - last);
    streamFps = streamFps * 0.9 + instant * 0.1;  // smoothed
    last = now;
  }
  streamFps = 0;
  Serial.println("[web] stream stopped");
  return res;
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t uris[] = {
    { "/", HTTP_GET, indexHandler, NULL },
    { "/capture", HTTP_GET, captureHandler, NULL },
    { "/control", HTTP_GET, controlHandler, NULL },
    { "/save", HTTP_GET, saveHandler, NULL },
    { "/status", HTTP_GET, statusHandler, NULL },
  };
  if (httpd_start(&webServer, &config) == ESP_OK) {
    for (auto &u : uris) httpd_register_uri_handler(webServer, &u);
    Serial.println("Web page server started on port 80");
  }

  // Stream runs on its own server so the page buttons still respond while streaming
  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t streamUri = { "/stream", HTTP_GET, streamHandler, NULL };
  if (httpd_start(&streamServer, &config) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &streamUri);
    Serial.println("Stream server started on port 81");
  }
}

// =====================================================================
// Summary + serial menu
// =====================================================================
void printSummary() {
  auto line = [](const char *name, bool ok, String extra) {
    Serial.printf("  %-10s %-5s %s\n", name, ok ? "OK" : "FAIL", extra.c_str());
  };
  Serial.println("\n========== SUMMARY ==========");
  line("PSRAM", results.psram, "");
  line("Camera", results.camera, results.sensor);
  line("Capture", results.capture, String(results.lastJpegBytes) + " bytes");
  line("FPS", results.fps > 0, String(results.fps, 1));
  line("SD card", results.sd, results.sdInfo);
  line("Wi-Fi", results.wifi, results.apMode ? "hotspot mode" : "connected");
  Serial.println("=============================");
}

void printMenu() {
  Serial.println("\nType a letter + Enter:");
  Serial.println("  a  run ALL tests again     i  system info");
  Serial.println("  l  LED test                f  flash LED on/off");
  Serial.println("  c  single capture          b  FPS benchmark");
  Serial.println("  s  SD card test            p  save photo to SD");
  Serial.println("  w  Wi-Fi scan              n  network info");
  Serial.println("  r  restart board           h  this menu");
}

void runAllTests() {
  testSystem();
  testLeds();
  testCapture();
  testFps();
  testSd();
  printSummary();
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = tolower(Serial.read());
  switch (c) {
    case 'a': runAllTests(); break;
    case 'i': testSystem(); break;
    case 'l': testLeds(); break;
    case 'f':
      setFlash(!flashOn);
      Serial.printf("Flash %s\n", flashOn ? "ON" : "OFF");
      break;
    case 'c': testCapture(); break;
    case 'b': testFps(); break;
    case 's': testSd(); break;
    case 'p': Serial.println(savePhotoToSd()); break;
    case 'w': testWifiScan(); break;
    case 'n':
      if (results.apMode) Serial.printf("Hotspot %s, IP %s, %d client(s)\n", AP_SSID, WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum());
      else Serial.printf("SSID %s, IP %s, signal %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case 'r': ESP.restart(); break;
    case 'h': printMenu(); break;
    default: return;  // ignore newline etc.
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n##### ESP32-CAM TEST #####");

  pinMode(RED_LED, OUTPUT);
  digitalWrite(RED_LED, HIGH);
  pinMode(FLASH_LED, OUTPUT);
  setFlash(false);

  testSystem();
  testLeds();
  initCamera();
  testCapture();
  testFps();
  testSd();
  WiFi.mode(WIFI_STA);
  testWifiScan();
  startWifi();
  startWebServer();
  printSummary();
  printMenu();
}

void loop() {
  handleSerial();

  // Heartbeat on the red LED: slow blink = all good, fast blink = camera failed
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  unsigned long interval = results.camera ? 1000 : 150;
  if (millis() - lastBlink > interval) {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(RED_LED, ledState ? LOW : HIGH);
  }
  delay(10);
}
