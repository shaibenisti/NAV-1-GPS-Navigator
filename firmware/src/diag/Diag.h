// =============================================================================
//  Diag  -  developer diagnostics: status sections, self-test, console commands.
// -----------------------------------------------------------------------------
//  Sections (serial "diag <name>", System app): sys mem gps wifi ble touch display sd
//  Self-test (serial "selftest [areas]", Tools app button, tools/scripts/validate.ps1):
//    one line per check   [CHECK] <area> <PASS|WARN|FAIL> <detail>
//    measurements         [METRICS] key=value ...      (validate.ps1 baseline drift checks)
//    closing line         [SELFTEST] done pass=N warn=N fail=N
//  The validation script parses exactly these two formats - keep them stable.
// =============================================================================
#pragma once

#include <Arduino.h>
#include <lvgl.h>

class GpsLink;
class GpsParser;
class SdLog;

namespace Diag {
  // Runs one UI-loop iteration without the console (GPS, LVGL, services), so a
  // test that waits (Wi-Fi scan, refresh-rate count) never starves the device.
  using Pump = void (*)();

  void begin(GpsLink &link, GpsParser &parser, SdLog &sd, bool parserSelfTestOk, Pump pump);

  void print(Print &out, const char *section);   // "all" or one section name

  // Short health overview for the Tools app: one plain-language row per part of the device.
  enum Level : uint8_t { OK, ATTENTION, PROBLEM, OFF };
  struct HealthRow { const char *name; Level level; char text[96]; };
  int healthRows(HealthRow *rows, int max);
  // Returns the number of FAILs. only = "" (all) or areas, e.g. "gps,wifi":
  // system memory gps touch display sd wifi ble. Ends with "[METRICS] key=value ...".
  int selfTest(Print &out, const char *only = "");
  void setBootMs(uint32_t ms);                    // start-up time, reported as boot_ms
  bool command(const char *line, Print &out);     // false = not a Diag command
  bool reportsOn();                               // periodic serial reports ("report on|off")
  void setStatusIconsLabel(lv_obj_t *label);    // status-bar icons (checked by the wifi self-test)
  bool busy();                                    // a self-test is running (the UI must not delete screens)
}
