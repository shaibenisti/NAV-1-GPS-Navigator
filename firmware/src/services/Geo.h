// =============================================================================
//  Geo  -  distances and bearings on the earth (header only).
//  Haversine for distances, initial great-circle bearing; a local flat projection (metres east /
//  north of a reference point) for the short-range geometry of route following.
// =============================================================================
#pragma once

#include <math.h>
#include <stdio.h>

namespace Geo {
  constexpr double R_EARTH = 6371008.8;                   // mean radius, metres
  constexpr double DEG = M_PI / 180.0;

  inline double distanceM(double lat1, double lon1, double lat2, double lon2) {
    const double dLat = (lat2 - lat1) * DEG, dLon = (lon2 - lon1) * DEG;
    const double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * DEG) * cos(lat2 * DEG) * sin(dLon / 2) * sin(dLon / 2);
    return 2 * R_EARTH * atan2(sqrt(a), sqrt(1 - a));
  }

  // 0..360, 0 = north, clockwise
  inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double y = sin((lon2 - lon1) * DEG) * cos(lat2 * DEG);
    const double x = cos(lat1 * DEG) * sin(lat2 * DEG) - sin(lat1 * DEG) * cos(lat2 * DEG) * cos((lon2 - lon1) * DEG);
    double b = atan2(y, x) / DEG;
    return b < 0 ? b + 360 : b;
  }

  // Local flat projection around (lat0, lon0): metres east (x) and north (y). Good to a few km.
  struct Local {
    double lat0, lon0, kx, ky;
    Local(double lat, double lon) : lat0(lat), lon0(lon), kx(111320.0 * cos(lat * DEG)), ky(110540.0) {}
    void toXY(double lat, double lon, double &x, double &y) const { x = (lon - lon0) * kx; y = (lat - lat0) * ky; }
    void toLL(double x, double y, double &lat, double &lon) const { lat = lat0 + y / ky; lon = lon0 + x / kx; }
  };

  inline const char *cardinal(double deg) {
    static const char *const N[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return N[(int)((deg + 22.5) / 45.0) & 7];
  }

  // "850 m", "1.24 km", "12.5 km", "128 km"
  inline void formatDistance(double m, char *out, size_t n) {
    if (m < 0) snprintf(out, n, "--");
    else if (m < 1000) snprintf(out, n, "%.0f m", m < 10 ? m : round(m / 10) * 10);
    else if (m < 10000) snprintf(out, n, "%.2f km", m / 1000);
    else if (m < 100000) snprintf(out, n, "%.1f km", m / 1000);
    else snprintf(out, n, "%.0f km", m / 1000);
  }
}
