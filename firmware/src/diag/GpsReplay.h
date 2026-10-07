// =============================================================================
//  GpsReplay  -  developer tool: replay a recorded NMEA session from the SD card
//  (/GPSLOG/Snnnn.NMEA) into the GPS parser, so GPS-dependent features (GPS app,
//  Trips, Map, Compass) can be exercised indoors and in automated validation.
// -----------------------------------------------------------------------------
//  Console: "gps replay <path> [speed]" (speed 1..20, x real time), "gps live".
//  While replaying, the live receiver is detached from the parser, the link counts
//  as up, and TimeService ignores GPS time (the recording is from the past).
//  Paced by RMC sentences: one navigation epoch per second / speed.
// =============================================================================
#pragma once

#include <Arduino.h>

class SdLog;
class GpsLink;
class GpsParser;

namespace GpsReplay {
  void begin(SdLog &sd, GpsLink &link, GpsParser &parser);
  bool start(const char *path, int speed, String *error = nullptr);
  void stop();                          // back to the live receiver
  void update();                        // call every loop
  bool active();
  void status(Print &out);
}
