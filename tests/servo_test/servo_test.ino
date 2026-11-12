/*
  Project Sunaina - servo test (ESP32-S3)

  Wiring:
    Servo signal (orange/yellow) -> GPIO4
    Servo + (red)                -> 5V   (better: separate 5V supply)
    Servo - (brown/black)        -> GND  (must share GND with the S3)

  On boot: centre (90), then it sweeps 0 -> 180 -> 0 in a loop.
  Type anything to stop the loop.

  Serial Monitor (115200), type + Enter:
    45        go to 45 degrees (0-180), moves smoothly
    l         loop: sweep 0 -> 180 -> 0 over and over (any key stops it)
    s         sweep once
    c         centre (90)
    p1500     send a raw pulse in microseconds (500-2500), to find your servo's true limits
    j         jitter test: hold 90 for 10 s, watch the horn
*/

#define SERVO_PIN 4
#define MIN_US 1000   // MG995 safe start; widen with the p command if it doesn't reach the ends
#define MAX_US 2000

int angle = 90;
bool looping = false;

void writeUs(int us) {
  us = constrain(us, 400, 2600);
  ledcWrite(SERVO_PIN, (uint32_t)us * 16383 / 20000);  // 50 Hz, 14-bit
}

void writeAngle(int a) {
  writeUs(MIN_US + (long)constrain(a, 0, 180) * (MAX_US - MIN_US) / 180);
}

void moveTo(int target, int msPerDeg = 8) {
  target = constrain(target, 0, 180);
  while (angle != target) {
    if (looping && Serial.available()) {   // a key press stops the loop mid-move
      looping = false;
      Serial.println("Loop stopped");
      break;
    }
    angle += (target > angle) ? 1 : -1;
    writeAngle(angle);
    delay(msPerDeg);
  }
  writeAngle(angle);
  Serial.printf("at %d deg\n", angle);
}

void sweep() {
  Serial.println("Sweep 0 -> 180 -> 0");
  moveTo(0);
  delay(400);
  moveTo(180, 12);
  delay(400);
  moveTo(0, 12);
  delay(400);
  moveTo(90);
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\nProject Sunaina - servo test on GPIO4");
  ledcAttach(SERVO_PIN, 50, 14);
  writeAngle(angle);
  delay(500);
  Serial.println("Type an angle (0-180), l = loop, s = sweep, c = centre, p1500 = raw pulse, j = jitter test");
  Serial.println("Looping 0 -> 180 -> 0. Type anything to stop.");
  looping = true;
}

void loop() {
  if (looping) {
    moveTo(0, 12);
    if (looping) delay(400);
    if (looping) moveTo(180, 12);
    if (looping) delay(400);
    return;
  }
  if (!Serial.available()) return;
  String in = Serial.readStringUntil('\n');
  in.trim();
  if (!in.length()) return;

  if (in == "l") {
    looping = true;
    Serial.println("Looping 0 -> 180 -> 0. Type anything to stop.");
  }
  else if (in == "s") sweep();
  else if (in == "c") moveTo(90);
  else if (in == "j") {
    moveTo(90);
    Serial.println("Holding 90 for 10 s. The horn should not twitch or buzz loudly.");
    delay(10000);
    Serial.println("Done. Twitching = weak power or no common GND.");
  } else if (in[0] == 'p') {
    int us = in.substring(1).toInt();
    writeUs(us);
    Serial.printf("pulse %d us\n", us);
  } else if (isDigit(in[0])) {
    moveTo(in.toInt());
  } else {
    Serial.println("?");
  }
}
