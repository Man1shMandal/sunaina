/*
  Project Sunaina - serial test on a classic ESP32
  Board: "ESP32 Dev Module". Serial Monitor: 115200, "Newline".

  Prints chip info, then a counter every second and blinks the onboard LED (GPIO2).
  Type anything + Enter and it echoes it back.
*/

#define LED 2

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(LED, OUTPUT);
  Serial.println("\nESP32 serial test OK");
  Serial.printf("Chip: %s rev %d, %d cores, %d MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %d MB, free heap: %d bytes\n", ESP.getFlashChipSize() / 1048576, ESP.getFreeHeap());
  Serial.println("Type something and press Enter.");
}

void loop() {
  static unsigned long last = 0;
  static int n = 0;
  if (millis() - last >= 1000) {
    last = millis();
    digitalWrite(LED, !digitalRead(LED));
    Serial.printf("alive %d\n", n++);
  }
  if (Serial.available()) {
    String in = Serial.readStringUntil('\n');
    in.trim();
    Serial.println("you typed: " + in);
  }
}
