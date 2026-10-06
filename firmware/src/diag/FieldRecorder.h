// =============================================================================
//  FieldRecorder  -  outdoor field test: records GPS + system health to SD.
// -----------------------------------------------------------------------------
//  Started/stopped from Tools > Field test or the console ("ft start|stop").
//  Each test: /FIELD/<id>/  (id = FT-YYYYMMDD-HHMMSS local time, or FT-Nnnnn
//  if the clock is not set yet)
//    samples.csv   one row per second (columns: see SAMPLE_HEADER)
//    events.csv    t_ms,utc,event,detail   (START, FIRST_FIX, FIX_LOST, FIX_REGAINED,
//                  WIFI, BLE, LOW_MEMORY, LOOP_STALL, SD_ERROR, TOUCH_ERROR, RESET, STOP)
//    summary.json  written at STOP: durations, GPS stats, errors, verdict
//  A running test is remembered in NVS: after a reset/crash the same test resumes
//  and gets a RESET event with the reset reason. PC side: tools/scripts/fieldtest.ps1.
//  Developer tool (Tools app); writes go through the SdLog writer task.
// =============================================================================
#pragma once

#include <Arduino.h>

class SdLog;

namespace FieldRecorder {
  void begin(SdLog &sd);                 // resumes a test interrupted by a reset
  void update();                         // call every loop; samples once per second

  bool start(uint32_t targetS = 0, String *error = nullptr);   // targetS 0 = until stop()
  uint32_t targetS();                    // planned duration (0 = open-ended)
  bool completed();                      // stopped automatically at the planned duration
  void stop();
  bool running();
  String id();
  uint32_t durationS();                  // this test, including time before a resume
  uint32_t samples();
  String lastEvent();                    // "FIX_LOST ..." for the Tools screen
  void status(Print &out);               // console "ft status"
}
