/*
  Project Sunaina - MG995 servo web control (ESP32-S3)

  Board: "ESP32S3 Dev Module" (pick it manually!). Serial: 115200.

  Wiring:
    Servo signal (orange) -> GPIO4
    Servo + (red)         -> external 5-6 V, 2 A+ supply   (NOT the S3 5V pin)
    Servo - (brown)       -> supply GND AND S3 GND (common ground)

  Wi-Fi:
    By default the S3 makes its own hotspot:
      network "Sunaina-Servo", password "sunaina123", then open http://192.168.4.1
    To use your home Wi-Fi instead, fill HOME_SSID / HOME_PASS below.
    The IP address is printed in Serial Monitor. http://sunaina-servo.local also works on most laptops.

  Web page:
    angle slider, speed (deg/s), 0 / 90 / 180, sweep once, loop between A and B
    with a pause at each end, stop (hold where it is), release (servo goes limp),
    calibration: min/max pulse, raw pulse slider, save to flash.

  Serial still works: type an angle (0-180), l = loop, s = sweep, x = stop, c = centre.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------- Wi-Fi ----------
// Home Wi-Fi lives in wifi_secrets.h (git-ignored). Copy wifi_secrets.example.h to start.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef HOME_WIFI_SSID
#define HOME_WIFI_SSID ""
#define HOME_WIFI_PASS ""
#endif
const char *HOME_SSID = HOME_WIFI_SSID;            // leave empty to use the hotspot
const char *HOME_PASS = HOME_WIFI_PASS;
const char *AP_SSID   = "Sunaina-Servo";
const char *AP_PASS   = "sunaina123";  // at least 8 characters

#define SERVO_PIN 4

WebServer server(80);
Preferences prefs;

// ---------- servo state ----------
int   minUs = 500, maxUs = 2500;    // MG995 full ~180 deg. Buzzing at an end? nudge in (e.g. 550 / 2450)
float pos = 90;                     // where the servo is now (degrees)
int   target = 90;                  // where it is going
float speedDps = 90;                // speed in degrees per second
bool  looping = false;
int   loopA = 0, loopB = 180;
int   pauseMs = 300;                // wait at each end of the loop
int   sweepStage = 0;               // 0 = no sweep running
bool  attached = false;
int   rawUs = -1;                   // >= 0 while in raw pulse mode
int   lastUs = -1;
unsigned long lastMicros = 0, holdUntil = 0;

const int SWEEP[] = {0, 180, 0, 90};

void writeUs(int us) {
  us = constrain(us, 400, 2600);
  if (us == lastUs) return;
  lastUs = us;
  ledcWrite(SERVO_PIN, (uint32_t)us * 16383 / 20000);   // 50 Hz, 14-bit
}

int angleToUs(float a) {
  return minUs + (long)(constrain(a, 0.0f, 180.0f) * (maxUs - minUs) / 180.0f);
}

void attachServo() {
  if (attached) return;
  ledcAttach(SERVO_PIN, 50, 14);
  attached = true;
  lastUs = -1;
}

void releaseServo() {   // stop sending pulses: servo goes limp
  looping = false;
  sweepStage = 0;
  if (!attached) return;
  ledcDetach(SERVO_PIN);
  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);
  attached = false;
}

void goTo(int a) {
  attachServo();
  rawUs = -1;
  target = constrain(a, 0, 180);
}

void stopHere() {
  looping = false;
  sweepStage = 0;
  target = round(pos);
}

void startLoop() {
  goTo(loopA);
  sweepStage = 0;
  looping = true;
}

void startSweep() {
  looping = false;
  goTo(SWEEP[0]);
  sweepStage = 1;
}

// Called every loop(): moves pos toward target at speedDps.
void updateMotion() {
  unsigned long now = micros();
  float dt = (now - lastMicros) / 1e6;
  lastMicros = now;
  if (!attached || rawUs >= 0) return;
  if (millis() < holdUntil) return;

  float step = speedDps * dt;
  if (fabs(target - pos) <= step) pos = target;
  else pos += (target > pos) ? step : -step;
  writeUs(angleToUs(pos));

  if (pos != target) return;
  if (looping) {
    target = (target == loopB) ? loopA : loopB;
    holdUntil = millis() + pauseMs;
  } else if (sweepStage) {
    if (sweepStage < 4) { target = SWEEP[sweepStage++]; holdUntil = millis() + 300; }
    else sweepStage = 0;
  }
}

// ---------- settings ----------
void loadSettings() {
  prefs.begin("servo", true);
  minUs = prefs.getInt("min", 500);
  maxUs = prefs.getInt("max", 2500);
  if (minUs == 1000 && maxUs == 2000) { minUs = 500; maxUs = 2500; }   // old default only gave ~90 deg
  speedDps = prefs.getFloat("speed", 90);
  pauseMs = prefs.getInt("pause", 300);
  loopA = prefs.getInt("la", 0);
  loopB = prefs.getInt("lb", 180);
  prefs.end();
}

void saveSettings() {
  prefs.begin("servo", false);
  prefs.putInt("min", minUs);
  prefs.putInt("max", maxUs);
  prefs.putFloat("speed", speedDps);
  prefs.putInt("pause", pauseMs);
  prefs.putInt("la", loopA);
  prefs.putInt("lb", loopB);
  prefs.end();
}

// ---------- web page ----------
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sunaina Servo</title>
<style>
:root{--bg:#f6f6f4;--card:#fff;--fg:#1d1d1f;--mute:#6e6e73;--line:#e2e2e0;--acc:#c8102e;--btn:#efefed}
@media (prefers-color-scheme:dark){:root{--bg:#141415;--card:#1e1e20;--fg:#f2f2f2;--mute:#9a9aa0;--line:#2e2e31;--acc:#ff4d5e;--btn:#2a2a2d}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:520px;margin:auto;padding:16px}
h1{font-size:18px;margin:4px 0 12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.lbl{display:flex;justify-content:space-between;color:var(--mute);font-size:13px;margin:8px 0 4px}
.lbl b{color:var(--fg);font-variant-numeric:tabular-nums}
input[type=range]{width:100%;accent-color:var(--acc)}
input[type=number]{width:80px;padding:6px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg)}
button{flex:1;min-width:64px;padding:10px;border:0;border-radius:8px;background:var(--btn);color:var(--fg);font:inherit;cursor:pointer}
button.pri{background:var(--acc);color:#fff}
#stat{font-size:13px;color:var(--mute);text-align:center;font-variant-numeric:tabular-nums}
svg{display:block;width:100%;max-width:300px;margin:0 auto}
</style></head><body><main>
<h1>Sunaina servo</h1>

<div class="card">
  <svg viewBox="0 0 200 112">
    <path d="M10 100 A90 90 0 0 1 190 100" style="fill:none;stroke:var(--line)" stroke-width="8" stroke-linecap="round"/>
    <circle id="tgt" r="5" style="fill:var(--mute)"/>
    <line id="needle" x1="100" y1="100" x2="100" y2="14" style="stroke:var(--acc)" stroke-width="4" stroke-linecap="round"/>
    <circle cx="100" cy="100" r="6" style="fill:var(--fg)"/>
  </svg>
  <div id="stat">connecting...</div>
</div>

<div class="card">
  <div class="lbl">Angle <b id="angv">90</b></div>
  <input id="ang" type="range" min="0" max="180" value="90">
  <div class="lbl">Speed (deg/s) <b id="spdv">90</b></div>
  <input id="spd" type="range" min="5" max="500" value="90">
  <div class="row" style="margin-top:10px">
    <button onclick="go(0)">0</button><button onclick="go(90)">90</button><button onclick="go(180)">180</button>
  </div>
</div>

<div class="card">
  <div class="row">
    <button class="pri" onclick="cmd('loop')">Loop</button>
    <button onclick="cmd('sweep')">Sweep once</button>
    <button onclick="cmd('stop')">Stop</button>
    <button onclick="cmd('release')">Release</button>
  </div>
  <div class="lbl">Loop from / to (deg), pause at ends (ms)</div>
  <div class="row">
    <input id="la" type="number" min="0" max="180" value="0">
    <input id="lb" type="number" min="0" max="180" value="180">
    <input id="pz" type="number" min="0" max="5000" step="50" value="300">
    <button onclick="setp('loopA',$('la').value+'&loopB='+$('lb').value+'&pause='+$('pz').value)">Set</button>
  </div>
</div>

<div class="card">
  <div class="lbl">Calibration: pulse for 0 and 180 deg (us)</div>
  <div class="row">
    <input id="mn" type="number" min="400" max="2600" step="10" value="500">
    <input id="mx" type="number" min="400" max="2600" step="10" value="2500">
    <button onclick="setp('min',$('mn').value+'&max='+$('mx').value)">Apply</button>
    <button onclick="cmd('save')">Save</button>
  </div>
  <div class="lbl">Raw pulse (find the real ends, then stop before it buzzes) <b id="rawv">1500</b></div>
  <input id="raw" type="range" min="500" max="2500" step="10" value="1500">
</div>

<script>
const $=id=>document.getElementById(id);
const send=u=>fetch(u).catch(()=>{});
const setp=(k,v)=>send('/set?'+k+'='+v);
const cmd=c=>send('/cmd?do='+c);
const go=a=>{$('ang').value=a;$('angv').textContent=a;setp('angle',a)};
function live(el,out,fn){let t=0;el.oninput=()=>{out.textContent=el.value;const n=Date.now();if(n-t>80){t=n;fn(el.value)}};el.onchange=()=>fn(el.value)}
live($('ang'),$('angv'),v=>setp('angle',v));
live($('spd'),$('spdv'),v=>setp('speed',v));
live($('raw'),$('rawv'),v=>send('/cmd?do=raw&us='+v));
function pt(a,r){const t=Math.PI*(1-a/180);return[100+r*Math.cos(t),100-r*Math.sin(t)]}
let first=true;
async function poll(){
  try{
    const s=await (await fetch('/status')).json();
    const [x,y]=pt(s.pos,86);$('needle').setAttribute('x2',x);$('needle').setAttribute('y2',y);
    const [tx,ty]=pt(s.target,90);$('tgt').setAttribute('cx',tx);$('tgt').setAttribute('cy',ty);
    const mode=!s.attached?'released':s.raw>=0?'raw '+s.raw+' us':s.looping?'looping':s.sweep?'sweeping':s.pos==s.target?'holding':'moving';
    $('stat').textContent=`${s.pos.toFixed(1)} deg  ->  ${s.target}  |  ${s.us} us  |  ${mode}`;
    if(first){first=false;
      $('spd').value=s.speed;$('spdv').textContent=Math.round(s.speed);
      $('ang').value=s.target;$('angv').textContent=s.target;
      $('la').value=s.loopA;$('lb').value=s.loopB;$('pz').value=s.pause;
      $('mn').value=s.min;$('mx').value=s.max;}
  }catch(e){$('stat').textContent='not connected';}
  setTimeout(poll,250);
}
poll();
</script></main></body></html>)HTML";

void handleStatus() {
  char buf[300];
  snprintf(buf, sizeof(buf),
    "{\"pos\":%.1f,\"target\":%d,\"speed\":%.0f,\"looping\":%s,\"sweep\":%s,\"attached\":%s,"
    "\"loopA\":%d,\"loopB\":%d,\"pause\":%d,\"min\":%d,\"max\":%d,\"us\":%d,\"raw\":%d}",
    pos, target, speedDps, looping ? "true" : "false", sweepStage ? "true" : "false",
    attached ? "true" : "false", loopA, loopB, pauseMs, minUs, maxUs, lastUs, rawUs);
  server.send(200, "application/json", buf);
}

void handleSet() {
  if (server.hasArg("speed")) speedDps = constrain(server.arg("speed").toFloat(), 1.0f, 600.0f);
  if (server.hasArg("loopA")) loopA = constrain(server.arg("loopA").toInt(), 0, 180);
  if (server.hasArg("loopB")) loopB = constrain(server.arg("loopB").toInt(), 0, 180);
  if (server.hasArg("pause")) pauseMs = constrain(server.arg("pause").toInt(), 0, 5000);
  if (server.hasArg("min"))   minUs = constrain(server.arg("min").toInt(), 400, 2600);
  if (server.hasArg("max"))   maxUs = constrain(server.arg("max").toInt(), 400, 2600);
  if (server.hasArg("angle")) { looping = false; sweepStage = 0; goTo(server.arg("angle").toInt()); }
  if (minUs >= maxUs) maxUs = minUs + 10;
  server.send(200, "text/plain", "ok");
}

void handleCmd() {
  String c = server.arg("do");
  if      (c == "loop")    startLoop();
  else if (c == "sweep")   startSweep();
  else if (c == "stop")    stopHere();
  else if (c == "release") releaseServo();
  else if (c == "save")    saveSettings();
  else if (c == "raw") {
    looping = false; sweepStage = 0;
    attachServo();
    rawUs = constrain(server.arg("us").toInt(), 400, 2600);
    writeUs(rawUs);
  } else { server.send(400, "text/plain", "unknown"); return; }
  server.send(200, "text/plain", "ok");
}

void startWifi() {
  if (strlen(HOME_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOME_SSID, HOME_PASS);
    Serial.printf("Connecting to %s", HOME_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Open http://%s\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println("Home Wi-Fi failed, starting hotspot instead.");
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("Join Wi-Fi \"%s\" (password %s), then open http://%s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}

void handleSerial() {
  if (!Serial.available()) return;
  String in = Serial.readStringUntil('\n');
  in.trim();
  if (!in.length()) return;
  if      (in == "l") startLoop();
  else if (in == "s") startSweep();
  else if (in == "x") stopHere();
  else if (in == "c") { looping = false; sweepStage = 0; goTo(90); }
  else if (isDigit(in[0])) { looping = false; sweepStage = 0; goTo(in.toInt()); }
  else Serial.println("?  angle / l / s / x / c");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nProject Sunaina - servo web control, signal on GPIO4");
  loadSettings();
  attachServo();
  writeUs(angleToUs(pos));

  startWifi();
  if (MDNS.begin("sunaina-servo")) Serial.println("Also at http://sunaina-servo.local");

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  server.on("/cmd", handleCmd);
  server.begin();
  lastMicros = micros();
}

void loop() {
  server.handleClient();
  handleSerial();
  updateMotion();
}
