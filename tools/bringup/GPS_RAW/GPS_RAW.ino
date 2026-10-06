// =============================================================================
//  GPS_RAW.ino  -  Absolute-minimum GPS raw byte passthrough
//  Board : ESP32-8048S043C-I (ESP32-S3, CH340 USB-UART on COM port)
//  GPS   : NEO-8M, GPS TX -> GPIO17 (verified by GPS_SCAN), GPS RX not connected, 9600 8N1
// -----------------------------------------------------------------------------
//  Serial  (UART0 -> CH340 -> PC) @ 115200 : debug console
//  Serial1 (UART1, RX only on GPIO17) @ 9600 : GPS input
//
//  Every byte received from the GPS is written unchanged to the console.
//  If nothing arrives for 3 s, a heartbeat line is printed so you can tell
//  "firmware running, no GPS data" apart from "firmware not running".
// =============================================================================

#include <Arduino.h>

#define GPS_RX_PIN   17   // measured: GPS TX is actually on GPIO17, not 44
#define GPS_BAUD     9600

static const unsigned long HEARTBEAT_MS = 3000;

unsigned long lastByteMs = 0;
unsigned long lastBeatMs = 0;
uint32_t totalBytes = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" GPS_RAW: firmware running");
  Serial.printf(" Reading Serial1 RX=GPIO%d @ %d baud (TX unused)\n", GPS_RX_PIN, GPS_BAUD);
  Serial.println(" Raw GPS bytes follow. Expect lines like $GNRMC...");
  Serial.println("==============================================");

  // RX only: TX = -1 so GPIO43 (console TX) is never touched.
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, -1);

  lastByteMs = lastBeatMs = millis();
}

void loop() {
  while (Serial1.available()) {
    Serial.write((uint8_t)Serial1.read());
    totalBytes++;
    lastByteMs = millis();
  }

  unsigned long now = millis();
  if (now - lastByteMs >= HEARTBEAT_MS && now - lastBeatMs >= HEARTBEAT_MS) {
    Serial.printf("\n[alive %lus] no GPS bytes in last %lus (total bytes=%u)\n",
                  now / 1000, HEARTBEAT_MS / 1000, (unsigned)totalBytes);
    lastBeatMs = now;
  }
}
