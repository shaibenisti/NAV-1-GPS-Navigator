// =============================================================================
//  Places  -  saved points (home, the parked car, a spring...).
// -----------------------------------------------------------------------------
//  Up to CAPACITY places, kept in PSRAM and on the SD card as /data/places.json:
//    {"places": [
//      {"name": "Home", "lat": 32.1234567, "lon": 34.8123456},
//      ...
//    ]}
//  The file can be edited on a PC (read at start-up; an unreadable file is kept as places.bad).
//  Names are UTF-8 (Hebrew from the phone page, ASCII from the on-screen keyboard), 1..NAME_BYTES bytes.
//  Changes are written ~1 s later in the background (SdLog::writeFileAsync, shared with Settings).
//  UI loop only: the web server reads json() through WebService (prepared in the loop).
// =============================================================================
#pragma once

#include <Arduino.h>

class SdLog;

namespace Places {
  constexpr int CAPACITY = 50;
  constexpr int NAME_BYTES = 40;           // bytes (UTF-8), without the terminator

  struct Place { char name[NAME_BYTES + 1]; double lat, lon; };

  void begin(SdLog &sd);                 // reads the file (no card: empty list, nothing saved)
  void update();                         // UI loop: debounced background write

  int count();
  bool get(int i, Place &out);
  int add(const char *name, double lat, double lon);    // index, -1 = list full / bad name or position
  bool rename(int i, const char *name);
  bool remove(int i);
  int find(const char *name);            // exact name, -1 = none
  String defaultName();                  // "Place 4": the first free one
  bool validName(const char *name);      // 1..NAME_BYTES bytes, valid UTF-8, no control characters

  uint32_t revision();                   // changes with every edit (map pins, web list)
  String json();                         // [{"name":"..","lat":..,"lon":..},...]
  String status();                       // e.g. "/data/places.json: 4 places"
  bool fileInSync();                     // self-test: the file on the card holds the current list
}
