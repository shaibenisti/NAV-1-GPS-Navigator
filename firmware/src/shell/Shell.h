// =============================================================================
//  Shell  -  the NAV-1 system shell.
// -----------------------------------------------------------------------------
//  Owns the UI loop: paged launcher (light transitions), status bar, app host
//  (apps/App.h: create/update/destroy), the service ticks (SD logging, Wi-Fi,
//  BLE, time) and the serial console (diag/Diag.h + UI commands).
//  Developer probes kept from the M1 spike: "bench", "flash", "swipe", "pulse".
// =============================================================================
#pragma once

#include "../GpsLink.h"
#include "../GpsParser.h"

namespace Shell {
  void begin(GpsLink &link, GpsParser &parser, bool parserSelfTestOk);
  void update();
  // Leave the open app and open another one (no tile animation). Called from an app's click handler.
  void switchApp(const char *name);
}
