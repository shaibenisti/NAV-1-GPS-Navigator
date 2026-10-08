#include "Navigator.h"

#include <esp_heap_caps.h>
#include "Geo.h"
#include "Location.h"
#include "Settings.h"
#include "TripRecorder.h"

namespace {

using Navigator::Mode;
using Navigator::Event;

constexpr uint32_t PERIOD_MS = 1000;
constexpr float LOOKAHEAD_M = 40.0f;           // the arrow points this far along the route past the nearest point
constexpr float REJOIN_M = 30.0f;              // back on the route below this
constexpr float LEAVE_ARRIVED_M = 80.0f;       // "arrived" ends when you move this far away again
constexpr float REACQUIRE_M = 150.0f;          // nearest point not near the last one: search the whole route

Mode s_mode = Mode::None;
char s_name[48] = "";
double s_destLat = 0, s_destLon = 0;           // Place
String s_base;                                  // Route: trip base path
bool s_reverse = false;
float *s_lat = nullptr, *s_lon = nullptr, *s_cum = nullptr;   // Route points in travel order (PSRAM), metres along
int s_n = 0, s_idx = 0;
float s_total = 0;

Navigator::Status s_st = {};
uint32_t s_lastMs = 0, s_rev = 1;
float s_kmh = 0;                               // smoothed speed for the ETA
Event s_event = Event::None;

void clearStatus() {
  s_st = {};
  s_st.mode = s_mode;
  s_st.distM = -1;
  s_st.reverse = s_reverse;
  strlcpy(s_st.name, s_name, sizeof(s_st.name));
  s_kmh = 0;
}

void save() {
  String v;
  if (s_mode == Mode::Place) v = "P\t" + String(s_destLat, 7) + "\t" + String(s_destLon, 7) + "\t" + s_name;
  else if (s_mode == Mode::Route) v = String("R\t") + (s_reverse ? "1" : "0") + "\t" + s_base + "\t" + s_name;
  Settings::setNavState(v);
}

void freeRoute() {
  heap_caps_free(s_lat);
  s_lat = s_lon = s_cum = nullptr;
  s_n = 0;
  s_total = 0;
}

void computeCum() {
  s_cum[0] = 0;
  for (int i = 1; i < s_n; i++) s_cum[i] = s_cum[i - 1] + (float)Geo::distanceM(s_lat[i - 1], s_lon[i - 1], s_lat[i], s_lon[i]);
  s_total = s_cum[s_n - 1];
  s_idx = 0;
}

void reverseArrays() {
  for (int i = 0, j = s_n - 1; i < j; i++, j--) {
    float t = s_lat[i]; s_lat[i] = s_lat[j]; s_lat[j] = t;
    t = s_lon[i]; s_lon[i] = s_lon[j]; s_lon[j] = t;
  }
}

bool loadRoute(const String &base) {
  freeRoute();
  s_lat = (float *)heap_caps_malloc(sizeof(float) * Navigator::MAX_ROUTE * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_lat) return false;
  s_lon = s_lat + Navigator::MAX_ROUTE;
  s_cum = s_lon + Navigator::MAX_ROUTE;
  s_n = TripRecorder::track(base, s_lat, s_lon, Navigator::MAX_ROUTE);
  if (s_n < 2) { freeRoute(); return false; }
  return true;
}

// Point at distance `a` along the route (from segment k on)
void pointAlong(float a, int k, double &lat, double &lon) {
  a = constrain(a, 0.0f, s_total);
  while (k < s_n - 2 && s_cum[k + 1] < a) k++;
  const float len = s_cum[k + 1] - s_cum[k];
  const float t = len > 0.01f ? (a - s_cum[k]) / len : 0;
  lat = s_lat[k] + (s_lat[k + 1] - s_lat[k]) * t;
  lon = s_lon[k] + (s_lon[k + 1] - s_lon[k]) * t;
}

// Nearest point of segments [k0, k1) to (0, 0) in the local projection: segment, its parameter and the distance
float nearest(const Geo::Local &L, int k0, int k1, int &bestK, float &bestT) {
  float best = 1e12f;
  for (int k = k0; k < k1; k++) {
    double ax, ay, bx, by;
    L.toXY(s_lat[k], s_lon[k], ax, ay);
    L.toXY(s_lat[k + 1], s_lon[k + 1], bx, by);
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double t = l2 > 1e-6 ? -(ax * dx + ay * dy) / l2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double px = ax + t * dx, py = ay + t * dy;
    const float d = (float)sqrt(px * px + py * py);
    if (d < best) { best = d; bestK = k; bestT = (float)t; }
  }
  return best;
}

void compute(double lat, double lon) {
  if (s_mode == Mode::Place) {
    s_st.distM = (float)Geo::distanceM(lat, lon, s_destLat, s_destLon);
    s_st.bearingDeg = (float)Geo::bearingDeg(lat, lon, s_destLat, s_destLon);
    s_st.guideLat = s_destLat; s_st.guideLon = s_destLon;
    s_st.destLat = s_destLat; s_st.destLon = s_destLon;
    return;
  }
  // Route
  const Geo::Local L(lat, lon);
  int k = 0;
  float t = 0;
  float d = nearest(L, max(0, s_idx - 10), min(s_n - 1, s_idx + 60), k, t);
  if (d > REACQUIRE_M) d = nearest(L, 0, s_n - 1, k, t);
  s_idx = k;
  const float along = s_cum[k] + t * (s_cum[k + 1] - s_cum[k]);
  s_st.offM = d;
  s_st.distM = s_total - along;
  s_st.progress = s_total > 0 ? along / s_total : 1;
  pointAlong(along + LOOKAHEAD_M, k, s_st.guideLat, s_st.guideLon);
  s_st.bearingDeg = (float)Geo::bearingDeg(lat, lon, s_st.guideLat, s_st.guideLon);
  s_st.destLat = s_lat[s_n - 1]; s_st.destLon = s_lon[s_n - 1];
}

}  // namespace

void Navigator::begin() {
  const String v = Settings::navState();
  if (v.length() < 3) return;
  const int a = v.indexOf('\t', 2), b = a > 0 ? v.indexOf('\t', a + 1) : -1;
  if (a < 0 || b < 0) return;
  if (v[0] == 'P') {
    const double lat = v.substring(2, a).toDouble(), lon = v.substring(a + 1, b).toDouble();
    goTo(v.substring(b + 1).c_str(), lat, lon);
  } else if (v[0] == 'R') {
    follow(v.substring(a + 1, b), v.substring(b + 1).c_str(), v[2] == '1', false);
  }
  Serial.printf("[NAV] resumed: %s '%s'\n", modeName(), s_name);
}

bool Navigator::goTo(const char *name, double lat, double lon) {
  if (fabs(lat) > 90 || fabs(lon) > 180 || (lat == 0 && lon == 0)) return false;
  freeRoute();
  s_mode = Mode::Place;
  strlcpy(s_name, name && *name ? name : "Destination", sizeof(s_name));
  s_destLat = lat; s_destLon = lon;
  s_reverse = false;
  s_base = "";
  clearStatus();
  s_lastMs = 0;                                            // compute at the next update
  s_rev++;
  save();
  return true;
}

bool Navigator::follow(const String &base, const char *title, bool forceReverse, bool autoDirection) {
  if (!loadRoute(base)) return false;
  bool rev = forceReverse;
  if (autoDirection && !forceReverse) {                    // standing nearer to the trip's end: take me back
    const GpsData d = Location::snapshot();
    double lat = 0, lon = 0;
    bool have = d.fix && d.locValid;
    if (have) { lat = d.lat; lon = d.lng; }
    else have = Settings::lastPos(lat, lon);
    if (have) rev = Geo::distanceM(lat, lon, s_lat[s_n - 1], s_lon[s_n - 1]) < Geo::distanceM(lat, lon, s_lat[0], s_lon[0]);
  }
  if (rev) reverseArrays();
  computeCum();
  s_mode = Mode::Route;
  s_base = base;
  s_reverse = rev;
  strlcpy(s_name, title && *title ? title : "Route", sizeof(s_name));
  clearStatus();
  s_st.destLat = s_lat[s_n - 1]; s_st.destLon = s_lon[s_n - 1];
  s_lastMs = 0;
  s_rev++;
  save();
  return true;
}

void Navigator::reverse() {
  if (s_mode != Mode::Route || s_n < 2) return;
  reverseArrays();
  computeCum();
  s_reverse = !s_reverse;
  clearStatus();
  s_lastMs = 0;
  s_rev++;
  save();
}

void Navigator::stop() {
  freeRoute();
  s_mode = Mode::None;
  s_name[0] = 0;
  s_base = "";
  s_reverse = false;
  clearStatus();
  s_event = Event::None;
  s_rev++;
  save();
}

void Navigator::update() {
  if (s_mode == Mode::None || (s_lastMs && millis() - s_lastMs < PERIOD_MS)) return;
  s_lastMs = millis();
  const GpsData d = Location::snapshot();
  s_st.fix = Location::quality(d) == Location::Quality::Fix && d.locValid;
  if (!s_st.fix) return;                                   // keep the last numbers; the apps show "no fix"
  if (d.speedValid) s_kmh = s_kmh * 0.7f + (float)d.speedKmh * 0.3f;
  compute(d.lat, d.lng);

  if (!s_st.arrived && s_st.distM >= 0 && s_st.distM < ARRIVE_M && (s_mode == Mode::Place || s_st.offM < OFF_ROUTE_M)) {
    s_st.arrived = true;
    s_st.offRoute = false;
    s_event = Event::Arrived;
  } else if (s_st.arrived && s_st.distM > LEAVE_ARRIVED_M) {
    s_st.arrived = false;
  }
  if (s_mode == Mode::Route && !s_st.arrived) {
    if (!s_st.offRoute && s_st.offM > OFF_ROUTE_M) { s_st.offRoute = true; s_event = Event::OffRoute; }
    else if (s_st.offRoute && s_st.offM < REJOIN_M) { s_st.offRoute = false; s_event = Event::BackOnRoute; }
  }
  s_st.etaS = s_kmh >= 3.0f && !s_st.arrived ? (uint32_t)(s_st.distM / (s_kmh / 3.6f)) : 0;
}

bool Navigator::active() { return s_mode != Mode::None; }
Navigator::Mode Navigator::mode() { return s_mode; }
Navigator::Status Navigator::status() { return s_st; }
uint32_t Navigator::revision() { return s_rev; }

int Navigator::route(const float **lat, const float **lon) {
  if (s_mode != Mode::Route || s_n < 2) return 0;
  *lat = s_lat;
  *lon = s_lon;
  return s_n;
}

String Navigator::routeCsvUrl() {
  if (s_mode != Mode::Route || !s_base.startsWith("/data/trips/")) return String();
  return "/trips/" + s_base.substring(12) + ".csv";
}

Navigator::Event Navigator::takeEvent() {
  const Event e = s_event;
  s_event = Event::None;
  return e;
}

const char *Navigator::modeName() {
  return s_mode == Mode::Place ? "place" : s_mode == Mode::Route ? (s_reverse ? "route (back)" : "route") : "off";
}
