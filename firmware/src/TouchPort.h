// =============================================================================
//  TouchPort  -  GT911 touch (docs/HARDWARE.md) sampled by its own task,
//  independent of how long LVGL takes to render a frame.
// -----------------------------------------------------------------------------
//  Before: LVGL read the GT911 once per rendered frame. Frames that change the
//  whole screen take 100-185 ms, so taps that started and ended inside one frame
//  were lost, releases arrived late and swipes were measured from 1-2 samples.
//
//  Now: a task on core 0 polls the GT911 every TOUCH_POLL_MS and queues every
//  press and release (moves are merged into one pending move, so LVGL still sees
//  one position per read, as before). LvglPort drains the queue on each LVGL read
//  (continue_reading), so no press/release is ever lost, whatever the frame time.
//
//  GT911 reads honour the "buffer ready" bit (0x814E bit 7): no new report means
//  "no change", never "released" (TAMC_GT911::read() reported a release there,
//  which would give phantom releases when polling faster than the 100 Hz reports).
//  Mapping = the verified one (TOUCH_DIAG / LCD_TOUCH_TEST).
// =============================================================================
#pragma once

#include <stdint.h>

namespace TouchPort {

  struct Event {
    int16_t x, y;                // panel coordinates (800 x 480; LvglPort rotates them for portrait)
    bool pressed;
    uint32_t us;                 // micros() of the GT911 report
  };

  struct Stats {                 // accumulated since the last takeStats()
    uint32_t polls;              // I2C polls of the status register
    uint32_t reports;            // new GT911 reports
    uint32_t i2cErrors;
    uint32_t overflows;          // queue full: event dropped (state still converges)
    uint32_t stuckReleases;      // pressed but no report for TOUCH_STUCK_POLLS: released
    uint32_t pollUsMax;          // longest poll (status + point + ack)
    uint32_t queueMax;           // deepest queue seen
  };

  // GT911 reset + configuration (TAMC_GT911 begin(), verified in M1), then starts
  // the polling task. Call after the display is running (same order as the tools).
  bool begin();
  bool present();                // GT911 answered at 0x5D

  // Oldest queued change. Returns false (and the current state in `e`) if the queue
  // is empty; `more` = further events are waiting.
  bool pop(Event &e, bool &more);

  Stats takeStats();
  Stats totals();                // since boot (not reset by takeStats)
}
