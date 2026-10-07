#include "GpsParser.h"

#include <string.h>

// The NEO-8M on this device emits GN-talker RMC/GGA (handled natively by
// TinyGPSPlus 1.0.3) plus GPGSV/GLGSV for satellites in view (see docs/HARDWARE.md).
// With the GPS + Galileo profile (GpsConfig) it sends NMEA 4.1: GPGSV + GAGSV, each GSV with a
// trailing signal ID, each GSA with a trailing system ID, RMC with a navigation-status field.
// Both versions are parsed (replays of older sessions are NMEA 4.0).
GpsParser::GpsParser(uint32_t fixMaxAgeMs)
  : _ggaQuality(_gps, "GNGGA", 6),
    _rmcStatus(_gps, "GNRMC", 2),
    _rmcTime(_gps, "GNRMC", 1),
    _rmcDate(_gps, "GNRMC", 9),
    _gsvGps(_gps, "GPGSV", 3),
    _gsvGlonass(_gps, "GLGSV", 3),
    _gsvGalileo(_gps, "GAGSV", 3),
    _fixMaxAgeMs(fixMaxAgeMs) {}

void GpsParser::attach(GpsLink &link) {
  link.setSentenceHandler(&GpsParser::onSentence, this);
}

void GpsParser::onSentence(const char *sentence, void *ctx) {
  static_cast<GpsParser *>(ctx)->feed(sentence);
}

void GpsParser::feed(const char *sentence) {
  for (const char *p = sentence; *p; ++p) _gps.encode(*p);
  _gps.encode('\r');                          // TinyGPSPlus commits a sentence on CR/LF
  _gps.encode('\n');
  _sentences++;

  // Custom fields: latch values that are only valid right after their sentence.
  if (_rmcStatus.isUpdated()) {
    const char *v = _rmcStatus.value();
    _rmcStatusChar = v[0] ? v[0] : '-';
  }
  if (_rmcTime.isUpdated())    _rmcTimePresent = _rmcTime.value()[0] != '\0';
  if (_rmcDate.isUpdated())    _rmcDatePresent = _rmcDate.value()[0] != '\0';
  if (_ggaQuality.isUpdated()) _fixQuality = (uint8_t)atoi(_ggaQuality.value());
  if (_gsvGps.isUpdated())     _satsViewGps = (uint32_t)atoi(_gsvGps.value());
  if (_gsvGlonass.isUpdated()) _satsViewGlonass = (uint32_t)atoi(_gsvGlonass.value());
  if (_gsvGalileo.isUpdated()) _satsViewGalileo = (uint32_t)atoi(_gsvGalileo.value());
  if (!strncmp(sentence, "$GPGSV", 6)) parseGsv(sentence, GNSS_GPS);
  else if (!strncmp(sentence, "$GLGSV", 6)) parseGsv(sentence, GNSS_GLONASS);
  else if (!strncmp(sentence, "$GAGSV", 6)) parseGsv(sentence, GNSS_GALILEO);
  else if (!strncmp(sentence + 3, "GSA", 3)) parseGsa(sentence);

  if (_ttffMs == 0 && positionIsFix()) _ttffMs = millis();
}

namespace {
// Splits a sentence copy at ',' (empty fields kept, checksum cut off). Returns the field count.
int splitFields(const char *sentence, char *buf, size_t cap, char **f, int max) {
  strlcpy(buf, sentence, cap);
  if (char *star = strchr(buf, '*')) *star = 0;
  int n = 0;
  f[n++] = buf;
  for (char *p = buf; *p && n < max; p++)
    if (*p == ',') { *p = 0; f[n++] = p + 1; }
  return n;
}
int field(const char *s, int dflt) { return *s ? atoi(s) : dflt; }
}  // namespace

// $GPGSV,<parts>,<part>,<in view>,{<prn>,<elev>,<azim>,<snr>} x up to 4[,<signal id>]
// NMEA 4.1 appends the signal ID: only complete 4-field blocks are satellites.
void GpsParser::parseGsv(const char *sentence, int c) {
  char buf[100];
  char *f[24];
  const int n = splitFields(sentence, buf, sizeof(buf), f, 24);
  if (n < 4) return;
  const int parts = field(f[1], 1), part = field(f[2], 1);
  if (part == 1) _buildCount[c] = 0;
  for (int i = 4; i + 3 < n && _buildCount[c] < MAX_SATS; i += 4) {
    if (!*f[i]) continue;
    GpsSat &s = _build[c][_buildCount[c]++];
    s.prn = (uint8_t)atoi(f[i]);
    s.elevation = (int8_t)(i + 1 < n ? field(f[i + 1], -1) : -1);
    s.azimuth = (int16_t)field(f[i + 2], -1);
    s.snr = (int8_t)field(f[i + 3], -1);
    s.gnss = (uint8_t)c;
    s.used = false;
  }
  if (part >= parts) {                        // last part: publish the table
    memcpy(_sats[c], _build[c], sizeof(GpsSat) * _buildCount[c]);
    _satCount[c] = _buildCount[c];
  }
}

// $GNGSA,<mode>,<fix type>,<prn> x 12,<pdop>,<hdop>,<vdop>[,<system id>]
// NMEA 4.1 system ID: 1 GPS (also SBAS/QZSS), 2 GLONASS, 3 Galileo, 4 BeiDou (ignored).
void GpsParser::parseGsa(const char *sentence) {
  char buf[100];
  char *f[20];
  const int n = splitFields(sentence, buf, sizeof(buf), f, 20);
  const int sys = n > 18 ? field(f[18], 0) : 0;
  if (sys == 4) return;
  const uint32_t now = millis();
  for (int i = 3; i < 15 && i < n; i++) {
    if (!*f[i]) continue;
    const uint8_t prn = (uint8_t)atoi(f[i]);
    const uint8_t gnss = sys == 3 ? GNSS_GALILEO : sys == 2 ? GNSS_GLONASS : sys == 1 ? GNSS_GPS
                       : (prn >= 65 && prn <= 96 ? GNSS_GLONASS : GNSS_GPS);   // NMEA 4.0: by PRN range
    int slot = -1, oldest = 0;
    for (int k = 0; k < 24; k++) {
      if (_usedPrn[k] == prn && _usedGnss[k] == gnss) { slot = k; break; }
      if (now - _usedMs[k] > now - _usedMs[oldest]) oldest = k;
    }
    if (slot < 0) slot = oldest;
    _usedPrn[slot] = prn;
    _usedGnss[slot] = gnss;
    _usedMs[slot] = now;
  }
}

int GpsParser::satellites(GpsSat *out, int max) const {
  int n = 0;
  const uint32_t now = millis();
  for (int c = 0; c < GNSS_COUNT; c++)
    for (int i = 0; i < _satCount[c] && n < max; i++) {
      GpsSat s = _sats[c][i];
      for (int k = 0; k < 24; k++)
        if (_usedPrn[k] == s.prn && _usedGnss[k] == s.gnss && now - _usedMs[k] < 2000) s.used = true;
      out[n++] = s;
    }
  return n;
}

// A fix needs all three: a parsed position, the receiver's own RMC "A" (active)
// flag, and a fresh update. Measured outdoors: the NEO-8M sends RMC "V" the
// moment it loses the fix, while the last position stays "valid" in TinyGPSPlus;
// age-only logic kept a stale fix for 4 s. Normal position age is 0.7-0.8 s.
bool GpsParser::positionIsFix() {
  return _rmcStatusChar == 'A' && _gps.location.isValid() && _gps.location.age() < _fixMaxAgeMs;
}

GpsData GpsParser::snapshot(bool linkUp) {
  GpsData d = {};
  d.linkUp          = linkUp;
  d.rmcStatus       = _rmcStatusChar;
  d.fixQuality      = _fixQuality;
  d.satsUsed        = _gps.satellites.isValid() ? _gps.satellites.value() : 0;
  d.satsViewGps     = _satsViewGps;
  d.satsViewGlonass = _satsViewGlonass;
  d.satsViewGalileo = _satsViewGalileo;

  d.locValid = _gps.location.isValid();
  d.locAgeMs = d.locValid ? _gps.location.age() : UINT32_MAX;
  d.lat      = d.locValid ? _gps.location.lat() : 0.0;
  d.lng      = d.locValid ? _gps.location.lng() : 0.0;
  d.fix      = linkUp && positionIsFix();

  d.speedValid  = _gps.speed.isValid();
  d.speedKmh    = d.speedValid ? _gps.speed.kmph() : 0.0;
  d.courseValid = _gps.course.isValid();
  d.courseDeg   = d.courseValid ? _gps.course.deg() : 0.0;
  d.altValid    = _gps.altitude.isValid();
  d.altM        = d.altValid ? _gps.altitude.meters() : 0.0;
  d.hdopValid   = _gps.hdop.isValid();
  d.hdop        = d.hdopValid ? _gps.hdop.hdop() : 0.0;

  d.timeValid = _gps.time.isValid() && _rmcTimePresent;
  d.dateValid = _gps.date.isValid() && _rmcDatePresent && _gps.date.year() >= 2020;
  d.hour   = _gps.time.hour();
  d.minute = _gps.time.minute();
  d.second = _gps.time.second();
  d.year   = _gps.date.year();
  d.month  = _gps.date.month();
  d.day    = _gps.date.day();

  d.sentencesParsed    = _sentences;
  d.parserChecksumFail = _gps.failedChecksum();
  d.sentencesWithFix   = _gps.sentencesWithFix();
  d.ttffMs             = _ttffMs;
  return d;
}
