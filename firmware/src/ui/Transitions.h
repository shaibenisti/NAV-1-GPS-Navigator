// =============================================================================
//  Transitions  -  Lightweight screen transitions
// -----------------------------------------------------------------------------
//  Built for this hardware: no animation redraws the full screen per frame.
//    zoomOpen : an opaque, square card in the app's colour grows from a tile to
//               full screen (write-only fills; the card covers what's beneath,
//               so LVGL draws nothing under it), then `done` is called.
//    pulse    : a short outline flash on one object (e.g. the tile an app was
//               opened from, after returning Home); redraws only that object.
//  Page changes and Back are instant (no helper needed).
// =============================================================================
#pragma once

#include <lvgl.h>

namespace Transitions {
  using DoneCb = void (*)(void *user);

  // `screen` must be the active screen containing the tile; `from` is in screen
  // coordinates (e.g. lv_obj_get_coords(tile)). The card is deleted after `done`.
  void zoomOpen(lv_obj_t *screen, const lv_area_t &from, lv_color_t color,
                uint32_t durationMs, DoneCb done, void *user);

  bool zoomRunning();

  void pulse(lv_obj_t *obj, lv_color_t color, uint32_t durationMs);
}
