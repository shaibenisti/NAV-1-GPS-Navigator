// =============================================================================
//  NAV1.ino  -  Main firmware, ESP32-8048S043C-I + NEO-8M
//  Docs: docs/ARCHITECTURE.md
// -----------------------------------------------------------------------------
//  This file only wires modules together. Pins/settings live in config.h;
//  hardware facts live in docs/HARDWARE.md.
// =============================================================================

#include "config.h"
#include "src/BootReport.h"
#include "src/GpsLink.h"
#include "src/GpsParser.h"
#include "src/GpsParserSelfTest.h"
#include "src/shell/Shell.h"            // NAV-1 shell: launcher, status bar, apps, services
#include "src/diag/Diag.h"

GpsLink   gpsLink;
GpsParser gpsParser(GPS_FIX_MAX_AGE_MS);

static void printStatus() {
  const GpsLink::Stats &s = gpsLink.stats();
  const GpsData d = gpsParser.snapshot(gpsLink.linkUp(GPS_LINK_TIMEOUT_MS));

  Serial.printf("[LINK %lus] %s bytes=%u nmea_ok=%u bad_crc=%u overflow=%u parser_crc_fail=%u\n",
                millis() / 1000, d.linkUp ? "UP" : "DOWN",
                (unsigned)s.bytes, (unsigned)s.sentences, (unsigned)s.badChecksum,
                (unsigned)s.overflows, (unsigned)d.parserChecksumFail);

  Serial.printf("[GPS] %s rmc=%c q=%u sats used=%u view=%u+%u",
                d.fix ? "FIX" : "NO FIX", d.rmcStatus, d.fixQuality,
                (unsigned)d.satsUsed, (unsigned)d.satsViewGps, (unsigned)d.satsViewGlonass);
  if (d.locValid) Serial.printf(" lat=%.6f lon=%.6f age=%ums", d.lat, d.lng, (unsigned)d.locAgeMs);
  else            Serial.print(" lat/lon=invalid");
  if (d.speedValid) Serial.printf(" spd=%.1fkm/h", d.speedKmh);
  if (d.altValid)   Serial.printf(" alt=%.1fm", d.altM);
  if (d.hdopValid)  Serial.printf(" hdop=%.2f", d.hdop);
  if (d.timeValid)  Serial.printf(" utc=%02u:%02u:%02u", d.hour, d.minute, d.second);
  else              Serial.print(" utc=invalid");
  if (d.dateValid)  Serial.printf(" %04u-%02u-%02u", d.year, d.month, d.day);
  if (d.ttffMs)     Serial.printf(" ttff=%us", (unsigned)(d.ttffMs / 1000));
  Serial.println();
}

void setup() {
  Serial.setTxBufferSize(CONSOLE_TX_BUFFER);   // before begin(): prints no longer stall the UI loop
  Serial.setRxBufferSize(4096);                // "sd put" uploads arrive in 32-line (~3 KB) blocks
  Serial.begin(CONSOLE_BAUD);
  delay(300);
  printBootReport();
  bool selfTestOk = true;
#if GPS_PARSER_SELFTEST
  selfTestOk = runGpsParserSelfTest();
#endif

  gpsLink.begin(GPS_UART, GPS_RX_PIN, GPS_BAUD, GPS_RX_BUFFER);
  gpsParser.attach(gpsLink);
#if GPS_ECHO_RAW
  gpsLink.setRawEcho(&Serial);
#endif
  Serial.printf("GPS: UART1 RX=GPIO%d @ %d baud, raw echo %s\n",
                GPS_RX_PIN, GPS_BAUD, GPS_ECHO_RAW ? "ON" : "OFF");

  Shell::begin(gpsLink, gpsParser, selfTestOk);     // LVGL + touch + SD + radios; takes over raw echo
}

void loop() {
  gpsLink.update();
  Shell::update();

  static uint32_t lastStatus = 0;
  if (!Diag::reportsOn()) return;                    // quiet console; "report on" enables it
  if (millis() - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = millis();
    printStatus();
  }
}
