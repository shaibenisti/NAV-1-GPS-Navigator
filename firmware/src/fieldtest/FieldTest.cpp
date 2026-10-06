#include "FieldTest.h"

#include <stdarg.h>
#include "../../config.h"
#include "../Display.h"
#include "../GpsCsv.h"

// ---- colours / layout ---------------------------------------------------------
static constexpr uint16_t C_BG      = RGB565_BLACK;
static constexpr uint16_t C_TEXT    = RGB565_WHITE;
static constexpr uint16_t C_DIM     = Display::rgb(120, 120, 120);
static constexpr uint16_t C_GOOD    = Display::rgb(0, 150, 0);
static constexpr uint16_t C_BAD     = Display::rgb(170, 0, 0);
static constexpr uint16_t C_NODATA  = Display::rgb(90, 60, 0);
static constexpr uint16_t C_VALUE   = Display::rgb(255, 230, 0);
static constexpr uint16_t C_BAR     = Display::rgb(30, 34, 48);

static constexpr int BAR_H    = 28;
static constexpr int BANNER_Y = 32;
static constexpr int BANNER_H = 88;

// Draws text padded with spaces to `width` characters, background filled, so a
// shorter new value fully overwrites a longer old one without clearing first.
static void field(int x, int y, uint8_t size, uint16_t fg, uint16_t bg, int width, const char *fmt, ...) {
  char buf[64];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  for (int i = n; i < width && i < (int)sizeof(buf) - 1; i++) buf[i] = ' ';
  buf[width < (int)sizeof(buf) - 1 ? width : (int)sizeof(buf) - 1] = '\0';
  Arduino_GFX *g = Display::gfx();
  g->setTextSize(size);
  g->setTextColor(fg, bg);
  g->setCursor(x, y);
  g->print(buf);
}

static void fmtAge(char *out, size_t n, uint32_t ms) {
  if (ms == UINT32_MAX) snprintf(out, n, "---");
  else if (ms < 10000)  snprintf(out, n, "%.1f s", ms / 1000.0);
  else                  snprintf(out, n, "%u s", (unsigned)(ms / 1000));
}

// ---- lifecycle ----------------------------------------------------------------

void FieldTest::begin(GpsLink &link, GpsParser &parser, bool selfTestOk) {
  _link = &link;
  _parser = &parser;

  _displayOk = Display::begin();
  Serial.printf("[FIELD] display: %s\n", _displayOk ? "OK" : "FAILED");
  if (_displayOk) drawStatic();

  const SdLog::Pins pins = { SD_CS_PIN, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN };
  if (_sd.begin(pins, SD_SPI_HZ) && _sd.openSession(SD_LOG_DIR)) {
    link.setRawEcho(&_sd.nmeaSink());
    writeSessionHeader(selfTestOk);
    Serial.printf("[FIELD] SD: logging to %s/%s.CSV + .NMEA (card %llu MB, mount attempts %d, wake-up %s)\n",
                  SD_LOG_DIR, _sd.sessionName(), _sd.cardMB(), _sd.mountAttempts(), _sd.wakeUpUsed() ? "USED" : "not needed");
  } else {
    Serial.printf("[FIELD] SD: NOT logging (mounted=%d, attempts=%d, errors=%u)\n",
                  _sd.mounted(), _sd.mountAttempts(), (unsigned)_sd.writeErrors());
  }
}

void FieldTest::writeSessionHeader(bool selfTestOk) {
  char buf[160];
  _sd.line("# firmware field test log");
  snprintf(buf, sizeof(buf), "# session=%s firmware=%s v%s (%s) built %s %s",
           _sd.sessionName(), APP_NAME, FW_VERSION, FW_GIT_DESCRIBE, __DATE__, __TIME__);
  _sd.line(buf);
  snprintf(buf, sizeof(buf), "# chip=%s rev%d cpu=%uMHz flash=%uMB psram=%u heap_free=%u",
           ESP.getChipModel(), ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz(),
           (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
  _sd.line(buf);
  snprintf(buf, sizeof(buf), "# gps=UART1 rx=GPIO%d baud=%d fix_max_age_ms=%d parser_selftest=%s",
           GPS_RX_PIN, GPS_BAUD, GPS_FIX_MAX_AGE_MS, selfTestOk ? "PASS" : "FAIL");
  _sd.line(buf);
  snprintf(buf, sizeof(buf), "# sd card=%lluMB spi=%uHz mount_attempts=%d wake_up=%s display=%s",
           _sd.cardMB(), (unsigned)SD_SPI_HZ, _sd.mountAttempts(), _sd.wakeUpUsed() ? "USED" : "no",
           _displayOk ? "OK" : "FAILED");
  _sd.line(buf);
  _sd.line("# rows every 1 s; empty field = not valid; lines starting '# event' mark state changes");
  _sd.line(GpsCsv::header());
}

void FieldTest::update() {
  const uint32_t now = millis();
  const GpsData d = _parser->snapshot(_link->linkUp(GPS_LINK_TIMEOUT_MS));

  if (now - _lastLog >= FIELD_LOG_MS) {
    _lastLog = now;
    logEvents(d);
    if (_sd.logging()) {
      char row[256];
      _sd.line(GpsCsv::row(row, sizeof(row), now, d, _link->stats()));
    }
  }
  _sd.update(SD_FLUSH_MS);
  handleConsole();

  if (_displayOk && now - _lastScreen >= FIELD_SCREEN_MS) {
    _lastScreen = now;
    drawBanner(d);
    drawValues(d);
  }
}

// ---- console commands -------------------------------------------------------------
// ls            list /GPSLOG
// cat S0002.CSV dump a log file (framed with <<<BEGIN/END>>> markers)
// Dumping blocks the loop; GPS bytes arriving meanwhile may overflow the UART buffer.

void FieldTest::handleConsole() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (_cmdLen < sizeof(_cmd) - 1) _cmd[_cmdLen++] = c;
      continue;
    }
    _cmd[_cmdLen] = '\0';
    _cmdLen = 0;

    if (strcmp(_cmd, "ls") == 0) {
      Serial.printf("%s:\n", SD_LOG_DIR);
      _sd.list(SD_LOG_DIR, Serial);
    } else if (strncmp(_cmd, "cat ", 4) == 0) {
      char path[64];
      snprintf(path, sizeof(path), "%s/%s", SD_LOG_DIR, _cmd + 4);
      _sd.dump(path, Serial);
    } else if (_cmd[0]) {
      Serial.println("commands: ls | cat <file>");
    }
  }
}

// ---- events ---------------------------------------------------------------------

void FieldTest::event(const char *fmt, ...) {
  char msg[120], line[140];
  va_list ap; va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  snprintf(line, sizeof(line), "# event t=%.1f %s", millis() / 1000.0, msg);
  Serial.println(line);
  _sd.line(line);
}

void FieldTest::logEvents(const GpsData &d) {
  if (d.linkUp != _prevLinkUp) event("GPS link %s", d.linkUp ? "UP" : "DOWN");
  if (d.fix != _prevFix) {
    if (d.fix) event("FIX ACQUIRED sats=%u hdop=%.2f lat=%.7f lon=%.7f", (unsigned)d.satsUsed, d.hdop, d.lat, d.lng);
    else       event("FIX LOST sats=%u", (unsigned)d.satsUsed);
  }
  if (_sd.writeErrors() != _prevSdErrors) {
    Serial.printf("[FIELD] SD write errors: %u\n", (unsigned)_sd.writeErrors());
    _prevSdErrors = _sd.writeErrors();
  }
  _prevLinkUp = d.linkUp;
  _prevFix = d.fix;
}

// ---- screen ---------------------------------------------------------------------

void FieldTest::drawStatic() {
  Arduino_GFX *g = Display::gfx();
  g->fillScreen(C_BG);
  g->fillRect(0, 0, Display::WIDTH, BAR_H, C_BAR);
  field(8, 6, 2, C_TEXT, C_BAR, 30, "GPS FIELD TEST  v%s", FW_VERSION);
}

void FieldTest::drawBanner(const GpsData &d) {
  const int state = !d.linkUp ? 0 : (d.fix ? 2 : 1);
  if (state == _bannerState && d.rmcStatus == _bannerRmc && d.fixQuality == _bannerQ) return;
  _bannerState = state; _bannerRmc = d.rmcStatus; _bannerQ = d.fixQuality;

  const uint16_t bg = state == 2 ? C_GOOD : (state == 1 ? C_BAD : C_NODATA);
  Arduino_GFX *g = Display::gfx();
  g->fillRect(0, BANNER_Y, Display::WIDTH, BANNER_H, bg);
  field(24, BANNER_Y + 16, 7, C_TEXT, bg, 11, "%s", state == 2 ? "FIX" : (state == 1 ? "NO FIX" : "NO GPS DATA"));
  if (state != 0) field(560, BANNER_Y + 20, 3, C_TEXT, bg, 12, "RMC %c  Q%u", d.rmcStatus, d.fixQuality);
}

void FieldTest::drawValues(const GpsData &d) {
  char a[16], b[16];
  const uint32_t up = millis() / 1000;
  field(560, 6, 2, C_TEXT, C_BAR, 19, "up %02u:%02u:%02u",
        (unsigned)(up / 3600), (unsigned)(up / 60 % 60), (unsigned)(up % 60));

  // Position (size 4)
  if (d.locValid) {
    field(16, 130, 4, C_VALUE, C_BG, 32, "LAT  %.7f %c", fabs(d.lat), d.lat >= 0 ? 'N' : 'S');
    field(16, 172, 4, C_VALUE, C_BG, 32, "LON  %.7f %c", fabs(d.lng), d.lng >= 0 ? 'E' : 'W');
  } else {
    field(16, 130, 4, C_DIM, C_BG, 32, "LAT  ---");
    field(16, 172, 4, C_DIM, C_BG, 32, "LON  ---");
  }
  field(16, 214, 4, C_TEXT, C_BG, 32, "SATS used %u  view %u+%u+%u",
        (unsigned)d.satsUsed, (unsigned)d.satsViewGps, (unsigned)d.satsViewGlonass, (unsigned)d.satsViewGalileo);

  // Motion / quality (size 3)
  if (d.speedValid) snprintf(a, sizeof(a), "%.1f km/h", d.speedKmh); else snprintf(a, sizeof(a), "---");
  if (d.courseValid) snprintf(b, sizeof(b), "%.0f", d.courseDeg); else snprintf(b, sizeof(b), "---");
  field(16, 264, 3, C_TEXT, C_BG, 42, "SPEED %s   COURSE %s", a, b);

  if (d.altValid) snprintf(a, sizeof(a), "%.1f m", d.altM); else snprintf(a, sizeof(a), "---");
  if (d.hdopValid) snprintf(b, sizeof(b), "%.2f", d.hdop); else snprintf(b, sizeof(b), "---");
  field(16, 298, 3, C_TEXT, C_BG, 42, "ALT %s   HDOP %s", a, b);

  if (d.timeValid) snprintf(a, sizeof(a), "%02u:%02u:%02u", d.hour, d.minute, d.second); else snprintf(a, sizeof(a), "--:--:--");
  if (d.dateValid) snprintf(b, sizeof(b), "%04u-%02u-%02u", d.year, d.month, d.day); else snprintf(b, sizeof(b), "----------");
  field(16, 332, 3, C_TEXT, C_BG, 42, "UTC %s  %s", a, b);

  fmtAge(a, sizeof(a), d.locAgeMs);
  if (d.ttffMs) snprintf(b, sizeof(b), "%u s", (unsigned)(d.ttffMs / 1000)); else snprintf(b, sizeof(b), "---");
  field(16, 366, 3, C_TEXT, C_BG, 42, "FIX AGE %s   TTFF %s", a, b);

  // Diagnostics
  const GpsLink::Stats &s = _link->stats();
  fmtAge(a, sizeof(a), _link->msSinceLastSentence());
  field(16, 400, 3, d.linkUp ? C_TEXT : C_BAD, C_BG, 42, "LINK %s  NMEA %u  bad %u  %s",
        d.linkUp ? "UP" : "DOWN", (unsigned)s.sentences, (unsigned)(s.badChecksum + d.parserChecksumFail), a);

  if (_sd.logging() && _sd.writeErrors() == 0) {
    field(16, 440, 3, C_TEXT, C_BG, 42, "SD %s  rows %u  nmea %u KB",
          _sd.sessionName(), (unsigned)_sd.csvLines(), (unsigned)(_sd.nmeaBytes() / 1024));
  } else if (_sd.logging()) {
    field(16, 440, 3, C_BAD, C_BG, 42, "SD %s  WRITE ERRORS %u", _sd.sessionName(), (unsigned)_sd.writeErrors());
  } else {
    field(16, 440, 3, C_BAD, C_BG, 42, "SD NOT LOGGING (no card?)");
  }
}
