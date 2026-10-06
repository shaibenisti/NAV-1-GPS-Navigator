#include "Location.h"

#include "../../config.h"

namespace {
GpsLink *s_link = nullptr;
GpsParser *s_parser = nullptr;
bool s_replay = false;
}

void Location::begin(GpsLink &link, GpsParser &parser) {
  s_link = &link;
  s_parser = &parser;
}

GpsData Location::snapshot() { return s_parser->snapshot(s_replay || s_link->linkUp(GPS_LINK_TIMEOUT_MS)); }
int Location::satellites(GpsSat *out, int max) { return s_parser ? s_parser->satellites(out, max) : 0; }
uint32_t Location::timeToFirstFixS() { const GpsData d = snapshot(); return d.ttffMs ? d.ttffMs / 1000 : 0; }
void Location::setReplay(bool on) { s_replay = on; }
bool Location::replaying() { return s_replay; }

const GpsLink::Stats &Location::linkStats() { return s_link->stats(); }

Location::Quality Location::quality(const GpsData &d) {
  if (!d.linkUp) return Quality::NoSignal;
  if (d.fix) return Quality::Fix;
  return d.locValid ? Quality::FixLost : Quality::Acquiring;
}

const char *Location::hdopWord(double h) {
  if (h < 1) return "ideal";
  if (h < 2) return "excellent";
  if (h < 5) return "good";
  if (h < 10) return "moderate";
  if (h < 20) return "fair";
  return "poor";
}
