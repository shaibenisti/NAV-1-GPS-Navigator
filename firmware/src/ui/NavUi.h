// =============================================================================
//  NavUi  -  pieces the navigation screens share (Navigate, Drive, Map, Compass).
//    arrow()        an object that draws a direction arrow (rotated by setArrow)
//    heading()      direction of travel from the GPS course, with the Compass app's hysteresis
//    relative()     where the arrow points: towards the guide point, relative to the direction of
//                   travel while moving, north up while standing still
//    formatEta()    "12 min", "1 h 05"
//    keyboard()     a full-screen text dialog (name of a place)
// =============================================================================
#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <stddef.h>

namespace NavUi {
  lv_obj_t *arrow(lv_obj_t *parent, int size, uint32_t color);
  void setArrow(lv_obj_t *arrow, float angleDeg, bool live);   // 0 = up; live = false: dimmed

  bool heading(float &deg);              // true = moving (deg valid)
  // Arrow angle for a bearing (true north): returns true when relative to the direction of travel
  bool relative(float bearingDeg, float &angleDeg);
  void formatEta(uint32_t s, char *out, size_t n);

  // Text dialog over the whole screen: title, one line of text (ASCII keyboard), OK / cancel.
  // done(text) is called with the text on OK, nullptr on cancel; the dialog closes itself.
  void keyboard(lv_obj_t *anyChild, const char *title, const char *initial, int maxLen, void (*done)(const char *text));
  bool keyboardOpen();
  void keyboardClose();                  // app destroy(): drop it without a callback
}
