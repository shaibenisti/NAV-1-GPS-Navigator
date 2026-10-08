// =============================================================================
//  Backlight  -  display brightness (PWM on LCD_BL_PIN) and dim-when-idle (M8).
// -----------------------------------------------------------------------------
//  - Brightness 10..100 % (Settings, default 100 = the previous always-on look).
//  - Optional dimming after N s without touch (LVGL inactivity time), default
//    never. Dimmed = DIM_PCT (or less, never brighter than the chosen level).
//  - The touch that wakes a dimmed screen only wakes it: it is not passed to LVGL
//    (no accidental button press), up to the finger's release.
//  Never draws.
// =============================================================================
#pragma once

#include <Arduino.h>

namespace Backlight {
  void begin();                          // after the panel is on
  void update();                         // UI loop

  void setBrightness(int pct, bool save = true);   // 10..100; save = persist (false while a slider moves)
  int brightness();
  void forceOff(bool off);               // backlight fully off regardless of level/dimming (firmware update)
  void setDimAfter(uint32_t seconds);    // 0 = never, persisted
  uint32_t dimAfter();
  bool dimmed();
  bool pwmOk();                          // PWM attached (else the backlight is simply on)
  void wake();
  void keepAwake(bool on);               // an app that is read at a glance (Drive, Navigate): no dimming while it is open

  // LvglPort touch gate: false = do not pass this touch to LVGL.
  bool touchGate(bool pressed);
}
