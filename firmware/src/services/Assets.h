// =============================================================================
//  Assets  -  optional images from the SD card.
// -----------------------------------------------------------------------------
//  /assets/icons/<App name>.bin      home tile icon, up to 128 x 128
//  /assets/wallpapers/home.bin       home background, up to 480 x 800 (portrait, centered)
//  Files are LVGL 9 binary images (12-byte header + pixels), RGB565 / RGB565A8
//  (with alpha) / ARGB8888, made on the PC with tools/scripts/imgconv.ps1.
//  Loaded once after the card is mounted into PSRAM. A missing, invalid or
//  oversized file is skipped and the built-in look (symbol icons, colour
//  background) stays: the device looks and works the same without a card.
// =============================================================================
#pragma once

#include <lvgl.h>

class SdLog;

namespace Assets {
  void begin(SdLog &sd, const char *const *appNames, int appCount);
  const lv_image_dsc_t *icon(int app);   // nullptr = built-in symbol
  const lv_image_dsc_t *wallpaper();     // nullptr = built-in background
  int loadedCount();
  int rejectedCount();                   // files present but not usable (see the boot log)
}
