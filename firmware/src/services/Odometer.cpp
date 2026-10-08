#include "Odometer.h"

#include "Geo.h"
#include "Location.h"

namespace {
double s_m = 0;
uint32_t s_movingS = 0, s_t0 = 0, s_lastMs = 0;
float s_max = 0;
bool s_havePrev = false;
double s_pLat = 0, s_pLon = 0;
}

void Odometer::update() {
  if (millis() - s_lastMs < 1000) return;
  s_lastMs = millis();
  const GpsData d = Location::snapshot();
  if (Location::quality(d) != Location::Quality::Fix || !d.locValid) { s_havePrev = false; return; }
  const float kmh = d.speedValid ? (float)d.speedKmh : 0.0f;
  if (kmh < MOVE_KMH) return;                             // standing: no jitter collected (the last point is kept)
  s_movingS++;
  if (kmh > s_max) s_max = kmh;
  if (s_havePrev) {                                       // at most what the speed allows in this second (+ 5 m of GPS
    const double m = Geo::distanceM(s_pLat, s_pLon, d.lat, d.lng);   // noise): a jump - fix after a gap, a fast replay -
    s_m += min(m, kmh / 3.6 * 1.5 + 5.0);                   // is not counted as driven distance
  }
  s_pLat = d.lat; s_pLon = d.lng; s_havePrev = true;
}

void Odometer::reset() { s_m = 0; s_movingS = 0; s_max = 0; s_t0 = millis(); s_havePrev = false; }
float Odometer::distanceKm() { return (float)(s_m / 1000.0); }
uint32_t Odometer::movingS() { return s_movingS; }
uint32_t Odometer::sinceS() { return (millis() - s_t0) / 1000; }
float Odometer::maxKmh() { return s_max; }
float Odometer::avgKmh() { return s_movingS >= 20 ? (float)(s_m / 1000.0) / (s_movingS / 3600.0f) : 0.0f; }
