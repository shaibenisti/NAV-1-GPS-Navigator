// =============================================================================
//  LvglPort  -  LVGL 9 on the verified hardware: RGB panel (docs/HARDWARE.md)
//  as the display, GT911 (§5, via TouchPort) as the pointer input.
// -----------------------------------------------------------------------------
//  Render modes (config.h LVGL_RENDER_MODE):
//    PARTIAL: LVGL renders dirty areas into an internal-RAM buffer, copied into
//             the Arduino_GFX framebuffer with the verified draw16bitRGBBitmap().
//    DIRECT : LVGL renders straight into the single Arduino_GFX framebuffer.
//             Fast, but the panel shows frames while they are drawn (tearing).
//    DOUBLE : RgbPanel with 2 PSRAM framebuffers; LVGL draws the hidden one and
//             it is shown at vsync. (M1 decision; see RgbPanel.h limitation)
//  Touch is sampled by TouchPort's own task and queued; every LVGL read drains
//  the queue, so input never depends on how long a frame takes.
//  Also collects frame and touch timing so the UI can be measured.
// =============================================================================
#pragma once

#include <lvgl.h>

namespace LvglPort {

  struct FrameStats {            // accumulated since the last takeFrameStats()
    uint32_t frames;             // rendered frames (RENDER_START..READY pairs)
    uint64_t renderUsSum;
    uint32_t renderUsMax;
    uint64_t flushUsSum;         // time inside the flush callback (write-back + vsync wait)
    uint32_t flushCalls;
    uint64_t rotateUsSum;        // portrait: time rotating strips into the panel framebuffer (inside the flush)
    uint32_t writebackUsMax;     // DOUBLE: whole-buffer cache write-back before a buffer is shown
    uint32_t syncs;              // DOUBLE: areas copied from the shown into the back buffer
    uint32_t syncDmaAreas;       //         ... of which by GDMA
    uint64_t syncUsSum;
    uint32_t syncUsMax;          //         longest single area copy
    uint32_t syncDmaUsMax;       //         longest GDMA area copy
    uint32_t syncMaxKB;          //         largest area (KB)
    uint32_t touchEvents;        // press/move/release events delivered to LVGL
    uint32_t touchBatchMax;      // most events delivered in one LVGL read
  };

  // Called after every rendered frame (from inside lv_timer_handler).
  using FrameListener = void (*)(uint32_t renderStartUs, uint32_t renderEndUs);
  // Called for every touch read; false = LVGL sees "released" (Backlight: the waking touch).
  using TouchGate = bool (*)(bool pressed);
  void setTouchGate(TouchGate gate);

  bool begin(int mode);          // mode = LVGL_RENDER_PARTIAL / _DIRECT / _DOUBLE (config.h)
  uint32_t update();             // lv_timer_handler(); returns ms until it needs to run again

  lv_display_t *display();
  lv_indev_t *touch();           // the GT911 pointer input device
  bool touchPresent();           // GT911 answered at 0x5D during begin()
  int mode();

  // Diagnostic: after each shown frame, count sampled pixels (every 8th row) whose PSRAM
  // content (what the panel shows) differs from the CPU's cached view.
  void setStaleCheck(bool on);
  uint32_t lastStalePixels();
  const char *modeName();
  const uint16_t *shownFramebuffer();   // the buffer currently scanned out
  uint16_t *backFramebuffer();          // DOUBLE mode: the buffer LVGL renders the next frame into

  FrameStats takeFrameStats();
  void setFrameListener(FrameListener cb);

  // Touch latency: time from the GT911 report that first showed a press to the
  // end of the first frame rendered after it. 0 = no new measurement.
  uint32_t takeTouchLatencyUs();
}
