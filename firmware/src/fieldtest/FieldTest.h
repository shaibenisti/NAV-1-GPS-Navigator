// =============================================================================
//  FieldTest  -  TEMPORARY stage 2B outdoor validation (screen + SD log)
// -----------------------------------------------------------------------------
//  Shows parsed GpsData in large text and logs a CSV row per second plus the
//  raw NMEA stream to the SD card. Consumes only public module APIs
//  (GpsLink, GpsParser, Display, SdLog, GpsCsv); nothing depends on it.
//  To remove after validation: delete src/fieldtest/ and the FIELD_TEST lines
//  in NAV1.ino / config.h.
// =============================================================================
#pragma once

#include "../GpsLink.h"
#include "../GpsParser.h"
#include "../SdLog.h"

class FieldTest {
public:
  void begin(GpsLink &link, GpsParser &parser, bool selfTestOk);
  void update();

private:
  void writeSessionHeader(bool selfTestOk);
  void logEvents(const GpsData &d);
  void event(const char *fmt, ...);
  void drawStatic();
  void drawBanner(const GpsData &d);
  void drawValues(const GpsData &d);
  void handleConsole();          // "ls" / "cat <file>" to read logs over USB

  char _cmd[48];
  size_t _cmdLen = 0;

  GpsLink *_link = nullptr;
  GpsParser *_parser = nullptr;
  SdLog _sd;
  bool _displayOk = false;

  uint32_t _lastScreen = 0, _lastLog = 0;
  int _bannerState = -1;         // 0 no data, 1 no fix, 2 fix
  char _bannerRmc = 0;
  uint8_t _bannerQ = 255;
  bool _prevLinkUp = false, _prevFix = false;
  uint32_t _prevSdErrors = 0;
};
