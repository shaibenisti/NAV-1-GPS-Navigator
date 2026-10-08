// =============================================================================
//  Navigator  -  guidance to a destination, in a straight line or along a recorded trip.
// -----------------------------------------------------------------------------
//  Two modes:
//    Place  "go to": direction and distance to a point (a saved place, a point picked on the map or
//           sent from the phone). Straight line - there is no road routing on the device.
//    Route  follow a recorded trip (its .csv), forwards or back to its start ("back-track"): the
//           arrow points at the route a little ahead of the nearest point on it (so it leads back to
//           the route when off it), the distance is the rest of the route.
//  The guidance survives a restart (NVS). Arrival (within ARRIVE_M) and leaving / rejoining the
//  route are reported once each through takeEvent() (the shell shows them as a toast).
//  UI loop only; computed once per second from Location::snapshot().
// =============================================================================
#pragma once

#include <Arduino.h>

namespace Navigator {
  enum class Mode : uint8_t { None, Place, Route };
  enum class Event : uint8_t { None, Arrived, OffRoute, BackOnRoute };

  constexpr float ARRIVE_M = 25.0f;      // destination reached
  constexpr float OFF_ROUTE_M = 45.0f;   // further than this from the route: off route (back on below 30 m)
  constexpr int MAX_ROUTE = 600;         // route points (the trip is evenly thinned to this)

  struct Status {
    Mode mode;
    bool fix;                            // a GPS position (else the numbers below are the last known)
    bool arrived;
    bool offRoute;
    bool reverse;                        // Route: driven from the trip's end back to its start
    char name[48];                       // destination name / trip title (UTF-8)
    double destLat, destLon;             // the destination (Route: the end the route leads to)
    double guideLat, guideLon;           // where the arrow points
    float distM;                         // to the destination (Route: along the route); < 0 = not known yet
    float bearingDeg;                    // from the position to the guide point (true north, 0..360)
    float offM;                          // Route: distance from the route
    float progress;                      // Route: 0..1 of the route done
    uint32_t etaS;                       // 0 = not known (not moving)
  };

  void begin();                          // resumes the guidance active before a restart
  void update();                         // UI loop (works once per second)

  bool goTo(const char *name, double lat, double lon);
  // Follow a trip (TripRecorder base path). Without `forceReverse` the direction is chosen by the
  // nearer end: standing at the trip's end means "take me back".
  bool follow(const String &base, const char *title, bool forceReverse = false, bool autoDirection = true);
  void reverse();                        // Route: the other way
  void stop();

  bool active();
  Mode mode();
  Status status();
  int route(const float **lat, const float **lon);   // Route points in travel order (map overlay); 0 = none
  String routeCsvUrl();                  // Route: the trip's .csv on the web server ("/trips/2026/<name>.csv"), else ""
  uint32_t revision();                   // changes when the target or the route changes
  Event takeEvent();                     // once per event
  const char *modeName();
}
