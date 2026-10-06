// =============================================================================
//  GpsParser  -  Stage 2: NMEA sentences -> GpsData (TinyGPSPlus underneath)
// -----------------------------------------------------------------------------
//  Subscribes to GpsLink's checksum-verified sentences. Knows nothing about
//  displays, SD or the console: consumers call snapshot() and get a plain
//  struct, so the stage 3 dashboard can reuse this unchanged.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <TinyGPS++.h>
#include "GpsLink.h"

struct GpsData {
  // --- link / fix state ---
  bool     linkUp;           // bytes arriving (from GpsLink)
  bool     fix;              // linkUp && RMC 'A' && valid location younger than fixMaxAgeMs
  char     rmcStatus;        // 'A' valid, 'V' void, '-' not seen yet
  uint8_t  fixQuality;       // GGA field 6: 0 none, 1 GPS, 2 DGPS, 6 estimated
  uint32_t satsUsed;         // GGA satellites used in solution
  uint32_t satsViewGps;      // GPGSV satellites in view
  uint32_t satsViewGlonass;  // GLGSV satellites in view
  uint32_t satsViewGalileo;  // GAGSV satellites in view (NMEA 4.1, GPS + Galileo profile)
  uint32_t satsInView() const { return satsViewGps + satsViewGlonass + satsViewGalileo; }

  // --- position / motion (valid only when the matching flag is true) ---
  bool     locValid;
  double   lat, lng;         // degrees
  uint32_t locAgeMs;         // since last position update (UINT32_MAX = never)
  bool     speedValid;
  double   speedKmh;
  bool     courseValid;
  double   courseDeg;
  bool     altValid;
  double   altM;
  bool     hdopValid;
  double   hdop;

  // --- UTC time / date ---
  bool     timeValid, dateValid;
  uint16_t year;
  uint8_t  month, day, hour, minute, second;

  // --- diagnostics ---
  uint32_t sentencesParsed;  // sentences fed into the parser
  uint32_t parserChecksumFail;
  uint32_t sentencesWithFix;
  uint32_t ttffMs;           // millis() at first fix since boot (0 = no fix yet)
};

// One satellite from the GSV sentences (+ GSA: used in the position fix).
// A satellite is identified by (gnss, prn): with NMEA 4.1 Galileo numbers (1-36) overlap GPS numbers.
enum : uint8_t { GNSS_GPS = 0, GNSS_GLONASS = 1, GNSS_GALILEO = 2, GNSS_COUNT = 3 };
struct GpsSat {
  uint8_t prn;                // satellite number (GPS 1-32, SBAS 33-64, GLONASS 65-96, Galileo 1-36)
  int8_t  elevation;          // degrees, -1 = unknown
  int16_t azimuth;            // degrees, -1 = unknown
  int8_t  snr;                // signal dB-Hz, -1 = not tracked
  bool    used;               // in the position solution (GSA, last 2 s)
  uint8_t gnss;               // GNSS_GPS / GNSS_GLONASS / GNSS_GALILEO (from the GSV talker)
};

class GpsParser {
public:
  static constexpr int MAX_SATS = 32;
  int satellites(GpsSat *out, int max) const;  // current sky: GPS, GLONASS, Galileo, in GSV order
  explicit GpsParser(uint32_t fixMaxAgeMs = 2000);

  void attach(GpsLink &link);                 // subscribe to verified sentences
  void feed(const char *sentence);            // one checksum-verified sentence, no CR/LF
  GpsData snapshot(bool linkUp);              // cheap; call as often as needed

private:
  static void onSentence(const char *sentence, void *ctx);
  bool positionIsFix();

  TinyGPSPlus _gps;
  TinyGPSCustom _ggaQuality;
  TinyGPSCustom _rmcStatus;
  // Raw RMC time/date fields. TinyGPSPlus 1.0.3 skips empty fields but still commits its
  // pending time/date (never initialised) on every RMC: an empty field must not count as valid.
  TinyGPSCustom _rmcTime;
  TinyGPSCustom _rmcDate;
  TinyGPSCustom _gsvGps;
  TinyGPSCustom _gsvGlonass;
  TinyGPSCustom _gsvGalileo;

  uint32_t _fixMaxAgeMs;
  uint32_t _sentences = 0;
  uint32_t _ttffMs = 0;
  char     _rmcStatusChar = '-';
  bool     _rmcTimePresent = false;
  bool     _rmcDatePresent = false;
  uint8_t  _fixQuality = 0;
  uint32_t _satsViewGps = 0;
  uint32_t _satsViewGlonass = 0;
  uint32_t _satsViewGalileo = 0;

  // Satellite table: GSV comes in 1..n parts per constellation; a part-1 starts a new
  // table, the last part publishes it. GSA lists the PRNs used in the fix (NMEA 4.1: plus a
  // system ID; NMEA 4.0: the system follows from the PRN range).
  void parseGsv(const char *sentence, int c);
  void parseGsa(const char *sentence);
  GpsSat _sats[GNSS_COUNT][MAX_SATS] = {}, _build[GNSS_COUNT][MAX_SATS] = {};
  int _satCount[GNSS_COUNT] = {}, _buildCount[GNSS_COUNT] = {};
  uint8_t _usedPrn[24] = {}, _usedGnss[24] = {};
  uint32_t _usedMs[24] = {};
};
