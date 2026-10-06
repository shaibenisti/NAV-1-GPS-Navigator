#include "Health.h"

#include <Arduino.h>
#include "../RgbPanel.h"

namespace {
constexpr uint32_t WINDOW_MS = 5000;
Health::Window s_last = {};
Health::Totals s_totals = {};
uint32_t s_lvglMax = 0, s_servicesMax = 0;
uint32_t s_windowStart = 0, s_vsyncStart = 0;
bool s_skip = false;
}

void Health::skipLoop() { s_skip = true; }

namespace {
const char *s_slowName = "";
uint32_t s_slowUs = 0;
}

void Health::notePart(const char *name, uint32_t us) {
  if (us > s_slowUs && millis() > 15000 && !s_skip) { s_slowUs = us; s_slowName = name; }
}
const char *Health::slowestPart() { return s_slowName; }
uint32_t Health::slowestPartUs() { return s_slowUs; }

void Health::noteLoop(uint32_t lvglUs, uint32_t servicesUs) {
  if (s_skip) { s_skip = false; return; }
  if (lvglUs > s_lvglMax) s_lvglMax = lvglUs;
  if (servicesUs > s_servicesMax) s_servicesMax = servicesUs;
}

bool Health::tick() {
  const uint32_t now = millis();
  if (now - s_windowStart < WINDOW_MS) return false;
  const uint32_t elapsed = now - s_windowStart;
  s_windowStart = now;

  Window w = {};
  w.frames = LvglPort::takeFrameStats();
  w.touch = TouchPort::takeStats();
  uint32_t drawMax = 0;
  RgbPanel::takeShowStats(w.shows, w.vsyncRaces, drawMax);
  w.loopLvglUsMax = s_lvglMax;
  w.loopServicesUsMax = s_servicesMax;
  const uint32_t v = RgbPanel::vsyncCount();
  w.vsyncHz = (v - s_vsyncStart) * 1000 / elapsed;
  s_vsyncStart = v;
  s_last = w;

  s_totals.frames += w.frames.frames;
  if (w.frames.renderUsMax > s_totals.renderUsMax) s_totals.renderUsMax = w.frames.renderUsMax;
  s_totals.vsyncRaces += w.vsyncRaces;
  if (now > 15000) {
    if (s_lvglMax > s_totals.loopLvglUsMax) s_totals.loopLvglUsMax = s_lvglMax;
    if (s_servicesMax > s_totals.loopServicesUsMax) s_totals.loopServicesUsMax = s_servicesMax;
  }
  s_lvglMax = s_servicesMax = 0;
  return true;
}

const Health::Window &Health::last() { return s_last; }
const Health::Totals &Health::totals() { return s_totals; }
