#include "GpsParserSelfTest.h"

#include <Arduino.h>
#include "GpsParser.h"

static int g_fail = 0;

// Wrap a body as "$<body>*HH" with a correct checksum and feed it.
static void feedBody(GpsParser &p, const char *body) {
  uint8_t sum = 0;
  for (const char *c = body; *c; ++c) sum ^= (uint8_t)*c;
  char line[100];
  snprintf(line, sizeof(line), "$%s*%02X", body, sum);
  p.feed(line);
}

static void check(const char *what, bool ok) {
  if (!ok) g_fail++;
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

static bool near(double a, double b, double tol) { return fabs(a - b) <= tol; }

bool runGpsParserSelfTest() {
  g_fail = 0;
  Serial.println("GpsParser self-test:");

  // --- 1. Exactly what this NEO-8M sends indoors (captured 2026-09-27) ---
  {
    GpsParser p(2000);
    const char *noFix[] = {
      "GNRMC,,V,,,,,,,,,,N", "GNVTG,,,,,,,,,N", "GNGGA,,,,,,0,00,99.99,,,,,,",
      "GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99", "GPGSV,1,1,00", "GLGSV,1,1,00", "GNGLL,,,,,,V,N",
    };
    for (const char *b : noFix) feedBody(p, b);
    const GpsData d = p.snapshot(true);
    check("no-fix: fix=false",            !d.fix);
    check("no-fix: rmcStatus='V'",        d.rmcStatus == 'V');
    check("no-fix: fixQuality=0",         d.fixQuality == 0);
    check("no-fix: satsUsed=0",           d.satsUsed == 0);
    check("no-fix: location invalid",     !d.locValid);
    check("no-fix: time invalid (empty)", !d.timeValid);
    check("no-fix: date invalid",         !d.dateValid);
    check("no-fix: ttff not set",         d.ttffMs == 0);
    check("no-fix: 0 parser crc fails",   d.parserChecksumFail == 0);
  }

  // --- 2. A valid 3D fix: 48.1173 N, 11.516667 E, 22.4 kn, 8 sats, HDOP 0.9 ---
  {
    GpsParser p(2000);
    const char *fix[] = {
      "GNRMC,123519.00,A,4807.03800,N,01131.00000,E,22.4,84.4,270926,,,A",
      "GNGGA,123519.00,4807.03800,N,01131.00000,E,1,08,0.90,545.4,M,46.9,M,,",
      "GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00",
      "GLGSV,2,1,07,65,10,020,30,66,45,090,35,67,30,180,40,75,12,300,25",
    };
    for (const char *b : fix) feedBody(p, b);
    const GpsData d = p.snapshot(true);
    check("fix: fix=true",                d.fix);
    check("fix: rmcStatus='A'",           d.rmcStatus == 'A');
    check("fix: fixQuality=1",            d.fixQuality == 1);
    check("fix: satsUsed=8",              d.satsUsed == 8);
    check("fix: sats in view 11 GPS + 7 GLONASS", d.satsViewGps == 11 && d.satsViewGlonass == 7);
    check("fix: lat 48.117300",           d.locValid && near(d.lat, 48.1173, 1e-6));
    check("fix: lon 11.516667",           d.locValid && near(d.lng, 11.516667, 1e-6));
    check("fix: speed 41.48 km/h (22.4 kn)", d.speedValid && near(d.speedKmh, 41.4848, 0.01));
    check("fix: course 84.4",             d.courseValid && near(d.courseDeg, 84.4, 0.01));
    check("fix: altitude 545.4 m",        d.altValid && near(d.altM, 545.4, 0.01));
    check("fix: hdop 0.90",               d.hdopValid && near(d.hdop, 0.90, 0.001));
    check("fix: utc 12:35:19",            d.timeValid && d.hour == 12 && d.minute == 35 && d.second == 19);
    check("fix: date 2026-09-27",         d.dateValid && d.year == 2026 && d.month == 9 && d.day == 27);
    check("fix: ttff recorded",           d.ttffMs > 0);
    check("fix: linkUp=false forces NO FIX", !p.snapshot(false).fix);
  }

  // --- 3. A corrupted sentence must be rejected by the parser too ---
  {
    GpsParser p(2000);
    p.feed("$GNGGA,123519.00,4807.03800,N,01131.00000,E,1,08,0.90,545.4,M,46.9,M,,*00");
    const GpsData d = p.snapshot(true);
    check("bad checksum: counted, no fix", d.parserChecksumFail == 1 && !d.fix);
  }

  // --- 4. Fix loss: the receiver's real loss sequence (field test S0005, t=714.9 s) ---
  {
    GpsParser p(2000);
    feedBody(p, "GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.5,56.0,270926,,,A");
    feedBody(p, "GNGGA,123519.00,4807.03800,N,01131.00000,E,1,07,1.55,545.4,M,46.9,M,,");
    check("loss: fix before loss", p.snapshot(true).fix);
    feedBody(p, "GNRMC,123520.00,V,,,,,,,270926,,,N");
    feedBody(p, "GNGGA,123520.00,,,,,0,00,99.99,,,,,,");
    const GpsData d = p.snapshot(true);
    check("loss: RMC V ends fix immediately", !d.fix && d.rmcStatus == 'V');
    check("loss: old position kept (not a fix)", d.locValid && d.locAgeMs < 2000);
    check("loss: time still valid", d.timeValid);
    feedBody(p, "GNRMC,123521.00,A,4807.03800,N,01131.00000,E,0.3,56.0,270926,,,A");
    check("recovery: RMC A restores fix", p.snapshot(true).fix);
  }

  // --- 5. Stale position: no update for longer than the age limit ---
  {
    GpsParser p(50);                           // 50 ms limit so the test takes 80 ms
    feedBody(p, "GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.5,56.0,270926,,,A");
    const bool fresh = p.snapshot(true).fix;
    delay(80);
    check("stale: fresh fix, then NO FIX after age limit", fresh && !p.snapshot(true).fix);
  }

  // --- 6. GGA position without any RMC 'A' is not a fix ---
  {
    GpsParser p(2000);
    feedBody(p, "GNGGA,123519.00,4807.03800,N,01131.00000,E,1,08,0.90,545.4,M,46.9,M,,");
    const GpsData d = p.snapshot(true);
    check("rule: GGA-only position, no RMC A -> no fix", d.locValid && !d.fix);
  }

  // --- 7. NMEA 4.1 with GPS + Galileo (GpsConfig profile "galileo") ---
  //  RMC has a 14th field (navigation status), GSV a trailing signal ID, GSA a trailing system ID;
  //  Galileo numbers overlap GPS numbers (both have a satellite 05).
  {
    GpsParser p(2000);
    const char *s41[] = {
      "GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.5,56.0,270926,,,A,V",
      "GNGGA,123519.00,4807.03800,N,01131.00000,E,1,04,1.20,545.4,M,46.9,M,,",
      "GNGSA,A,3,12,24,,,,,,,,,,,2.10,1.20,1.70,1",
      "GNGSA,A,3,05,11,,,,,,,,,,,2.10,1.20,1.70,3",
      "GPGSV,1,1,04,05,40,100,35,12,51,319,28,24,77,196,29,30,10,020,,1",
      "GAGSV,1,1,02,05,30,200,33,11,60,050,38,7",
    };
    for (const char *b : s41) feedBody(p, b);
    const GpsData d = p.snapshot(true);
    check("4.1: 14-field RMC gives a fix", d.fix && d.rmcStatus == 'A');
    check("4.1: in view 4 GPS + 2 Galileo", d.satsViewGps == 4 && d.satsViewGalileo == 2 && d.satsInView() == 6);
    GpsSat sats[GpsParser::MAX_SATS];
    const int n = p.satellites(sats, GpsParser::MAX_SATS);
    bool noSignalIdSat = n == 6;
    for (int i = 0; i < n; i++) if (sats[i].prn == 1 || sats[i].prn == 7) noSignalIdSat = false;
    check("4.1: trailing signal ID is not a satellite (6 listed)", noSignalIdSat);
    auto used = [&](uint8_t gnss, uint8_t prn) {
      for (int i = 0; i < n; i++) if (sats[i].gnss == gnss && sats[i].prn == prn) return (int)sats[i].used;
      return -1;
    };
    check("4.1: GPS 12, 24 used; GPS 30 not", used(GNSS_GPS, 12) == 1 && used(GNSS_GPS, 24) == 1 && used(GNSS_GPS, 30) == 0);
    check("4.1: Galileo 05, 11 used", used(GNSS_GALILEO, 5) == 1 && used(GNSS_GALILEO, 11) == 1);
    check("4.1: GPS 05 not used (only Galileo 05 is)", used(GNSS_GPS, 5) == 0);
    check("4.1: Galileo SNR kept (E11 38 dB-Hz)", [&] { for (int i = 0; i < n; i++) if (sats[i].gnss == GNSS_GALILEO && sats[i].prn == 11) return sats[i].snr == 38; return false; }());
  }

  // --- 8. NMEA 4.0 GSA (no system ID): GLONASS by number range, as before ---
  {
    GpsParser p(2000);
    feedBody(p, "GLGSV,1,1,01,76,37,080,25");
    feedBody(p, "GNGSA,A,3,76,,,,,,,,,,,,4.70,3.36,3.29");
    GpsSat sats[4];
    const int n = p.satellites(sats, 4);
    check("4.0: GLONASS 76 listed and used", n == 1 && sats[0].gnss == GNSS_GLONASS && sats[0].prn == 76 && sats[0].used);
  }

  Serial.printf("GpsParser self-test: %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail == 0;
}
