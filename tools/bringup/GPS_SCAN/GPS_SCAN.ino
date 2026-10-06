// =============================================================================
//  GPS_SCAN.ino  -  "Which pin / which baud is the signal really on?" detective
//  Board : ESP32-8048S043C-I (ESP32-S3, CH340 on UART0 = GPIO43/44)
// -----------------------------------------------------------------------------
//  Each cycle (~33 s):
//   A) Pull probe: for pull NONE / DOWN / UP, 3 s of edge counting + % LOW on
//      the WATCH pins (44, 17, 18; input only, never driven).
//        0 edges, LOW with pull-down  -> nothing drives the pin
//        0 edges, HIGH with pull-down -> something holds it HIGH
//        steady edges in every mode   -> active signal
//   B) UART scan: Serial1 RX on GPIO17 at 9600/4800/38400/57600/115200, then
//      GPIO44 @ 9600 (4 s each). Right baud = 100% printable '$' lines.
//  Found the GPS on GPIO17 @ 9600 on 2026-09-27. Edit WATCH[] / uartScan() calls
//  to probe other pins. See docs/HARDWARE.md.
// =============================================================================

#include <Arduino.h>

static const int WATCH[]  = { 44, 17, 18 };
static const int NWATCH   = sizeof(WATCH) / sizeof(WATCH[0]);
static const uint32_t BAUDS[] = { 9600, 4800, 38400, 57600, 115200 };
static const int NBAUDS   = sizeof(BAUDS) / sizeof(BAUDS[0]);

volatile uint32_t edgeCount[NWATCH];

void IRAM_ATTR isr0() { edgeCount[0]++; }
void IRAM_ATTR isr1() { edgeCount[1]++; }
void IRAM_ATTR isr2() { edgeCount[2]++; }
void (*const ISRS[NWATCH])() = { isr0, isr1, isr2 };

static void pullProbe(const char *name, uint8_t mode) {
  for (int i = 0; i < NWATCH; i++) {
    pinMode(WATCH[i], mode);
    edgeCount[i] = 0;
    attachInterrupt(digitalPinToInterrupt(WATCH[i]), ISRS[i], CHANGE);
  }
  uint32_t lows[NWATCH] = {0}, samples = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    for (int i = 0; i < NWATCH; i++) if (digitalRead(WATCH[i]) == LOW) lows[i]++;
    samples++;
    delayMicroseconds(20);
  }
  for (int i = 0; i < NWATCH; i++) detachInterrupt(digitalPinToInterrupt(WATCH[i]));

  Serial.printf("[PULL %-4s]", name);
  for (int i = 0; i < NWATCH; i++)
    Serial.printf("  IO%d: edges=%u low%%=%.1f", WATCH[i],
                  (unsigned)edgeCount[i], 100.0f * lows[i] / samples);
  Serial.println();
}

static void uartScan(int pin, uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, pin, -1);
  while (Serial1.available()) Serial1.read();

  uint32_t bytes = 0, dollars = 0, printable = 0;
  char sample[81]; int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 4000) {
    while (Serial1.available()) {
      int c = Serial1.read();
      bytes++;
      if (c == '$') dollars++;
      bool pr = (c >= 32 && c <= 126) || c == '\r' || c == '\n';
      if (pr) printable++;
      if (n < 80) sample[n++] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
  }
  sample[n] = 0;
  Serial1.end();

  Serial.printf("[UART IO%d %6u] bytes=%u  '$'=%u  printable=%.0f%%  sample=\"%s\"\n",
                pin, (unsigned)baud, (unsigned)bytes, (unsigned)dollars,
                bytes ? 100.0f * printable / bytes : 0.0f, sample);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== GPS_SCAN: firmware running (software-only GPIO44 diagnostic) ===");
}

void loop() {
  static int cycle = 0;
  Serial.printf("\n--- cycle %d  (t=%lus) ---\n", ++cycle, millis() / 1000);
  pullProbe("NONE", INPUT);
  pullProbe("DOWN", INPUT_PULLDOWN);
  pullProbe("UP",   INPUT_PULLUP);
  for (int i = 0; i < NBAUDS; i++) uartScan(17, BAUDS[i]);
  uartScan(44, 9600);
}
