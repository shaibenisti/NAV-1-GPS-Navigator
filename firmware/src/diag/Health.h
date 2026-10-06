// =============================================================================
//  Health  -  rolling runtime health: 5 s windows + totals since boot for the
//  display path, touch task and UI loop. Owns the "take" side of the LvglPort,
//  TouchPort and RgbPanel window statistics, so every consumer (serial report,
//  System app, self-test) sees the same numbers.
// =============================================================================
#pragma once

#include <stdint.h>
#include "../LvglPort.h"
#include "../TouchPort.h"

namespace Health {

  struct Window {                      // one 5 s window
    LvglPort::FrameStats frames;
    TouchPort::Stats touch;
    uint32_t shows, vsyncRaces;        // RgbPanel buffer switches in the window
    uint32_t loopLvglUsMax, loopServicesUsMax;
    uint32_t vsyncHz;                  // panel refresh rate measured over the window
  };

  struct Totals {                      // since boot
    uint32_t frames;
    uint32_t renderUsMax;
    uint32_t loopLvglUsMax, loopServicesUsMax;   // after the first 15 s (boot excluded)
    uint32_t vsyncRaces;
  };

  void noteLoop(uint32_t lvglUs, uint32_t servicesUs);   // every loop
  // Per-part timing of the non-LVGL loop (status bar, app update, each service, console):
  // the slowest part since boot (after 15 s) names the culprit of a "services" stall.
  void notePart(const char *name, uint32_t us);
  const char *slowestPart();
  uint32_t slowestPartUs();
  void skipLoop();                      // the current iteration is a deliberate wait (self-test)
  bool tick();                          // every loop; true when a new window was closed
  const Window &last();
  const Totals &totals();
}
