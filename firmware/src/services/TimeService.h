// =============================================================================
//  TimeService  -  system clock and local time.
// -----------------------------------------------------------------------------
//  - UTC from GPS (RMC date + time; available before a position fix); re-set when
//    the system clock drifts more than 2 s from GPS.
//  - SNTP (pool.ntp.org) once while Wi-Fi is connected, if GPS has not set it.
//  - Local time via a POSIX TZ string (DST rules included), zone chosen in
//    Settings > Device and stored in NVS.
//  Never draws.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <time.h>

namespace TimeService {
  void begin();
  void update();                         // call every loop (does work once per second)

  bool valid();                          // system clock set from GPS or NTP
  const char *source();                  // "GPS", "NTP" or "none"
  bool local(struct tm &out);            // false if the clock is not set
  bool utc(struct tm &out);
  uint32_t lastSyncAgeS();               // since the last GPS/NTP set (UINT32_MAX = never)

  int zoneCount();
  const char *zoneName(int i);
  int zone();                            // index into the zone list
  void setZone(int i);                   // persisted
}
