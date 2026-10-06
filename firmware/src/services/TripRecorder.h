// =============================================================================
//  TripRecorder  -  records trips to the SD card.
// -----------------------------------------------------------------------------
//  One trip = /data/trips/<YYYY>/<YYYY-MM-DD_HHMMSS>.{gpx,csv,json}
//  (clock not set yet: /data/trips/undated/TRIP-Nnnnn.*)
//    .gpx   standard GPX 1.1 track, one <trkpt> per recorded point (closed on stop)
//    .csv   t_s,utc,lat,lon,ele_m,speed_kmh,course_deg,sats,hdop
//    .json  summary written on stop (the Trips list reads it)
//  Points: only with a GPS fix; a new point when moved >= 3 m or every 10 s.
//  A recording survives a reset (resumed from NVS, like the field test).
//  Writes go through the SdLog writer task. Never draws.
// =============================================================================
#pragma once

#include <Arduino.h>

class SdLog;

namespace TripRecorder {

  struct Summary {                       // one finished trip (from its .json)
    String name;                         // file base name, e.g. 2026-09-28_214500
    String title;                        // "28.09.2026 21:45" (local time) or the name
    String base;                         // path without extension, e.g. /data/trips/2026/2026-09-28_214500
    uint32_t durationS, movingS, points;
    float distanceKm, maxKmh, avgKmh;
  };

  void begin(SdLog &sd);                 // resumes a trip interrupted by a reset
  void update();                         // call every loop; samples once per second

  bool start(String *error = nullptr);
  void stop();
  bool recording();
  bool saving();                         // files of the last trip still being written (summary not yet on the card)
  bool waitingForFix();                  // recording, but no fix yet / fix lost

  uint32_t durationS();
  uint32_t movingS();
  float distanceKm();
  float maxKmh();
  float currentKmh();
  uint32_t points();
  String name();

  int list(Summary *out, int max);       // newest first, from the .json files
  // Track points of a finished trip (from its .csv), evenly thinned to at most `max`.
  int track(const String &base, float *lat, float *lon, int max);
}
