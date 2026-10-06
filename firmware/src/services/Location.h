// =============================================================================
//  Location  -  read access to the GPS for apps and services (the start of the
//  LocationService: GpsLink + GpsParser stay the implementation).
// =============================================================================
#pragma once

#include "../GpsLink.h"
#include "../GpsParser.h"

namespace Location {
  void begin(GpsLink &link, GpsParser &parser);
  GpsData snapshot();                    // fix rule D8 applied
  int satellites(GpsSat *out, int max);  // current sky (GSV + used in fix from GSA)
  uint32_t timeToFirstFixS();            // since start-up, 0 = no fix yet
  const GpsLink::Stats &linkStats();
  void setReplay(bool on);               // developer GPS replay: the link counts as up
  bool replaying();

  enum class Quality : uint8_t { NoSignal, Acquiring, Fix, FixLost };
  Quality quality(const GpsData &d);     // NoSignal = no UART data; FixLost = had a position, lost the fix
  const char *hdopWord(double hdop);     // "ideal" .. "poor"
}
