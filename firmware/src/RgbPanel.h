// =============================================================================
//  RgbPanel  -  The 800x480 RGB panel via ESP-IDF esp_lcd, with TWO PSRAM
//  framebuffers switched at vsync (no tearing).
// -----------------------------------------------------------------------------
//  Same pins, timing and flags as the verified Arduino_GFX configuration
//  (docs/HARDWARE.md, config.h); the only difference is num_fbs = 2.
//  Arduino_GFX's panel supports one framebuffer only. Do not use both drivers
//  in the same firmware: they would claim the same LCD peripheral.
//
//  Scan-out (config.h LCD_BOUNCE_LINES > 0, the default): the panel DMA reads
//  two small internal-RAM bounce buffers that the driver refills from the shown
//  framebuffer, and esp_lcd switches framebuffers itself. With LCD_BOUNCE_LINES 0 the
//  DMA reads PSRAM directly; esp_lcd's switch to the 2nd buffer is broken there (only
//  its top ~5 rows are scanned, docs/HARDWARE.md), so LCD_OWN_FLIP retargets
//  fb0's DMA descriptors instead.
// =============================================================================
#pragma once

#include <stdint.h>

namespace RgbPanel {
  constexpr int WIDTH  = 800;
  constexpr int HEIGHT = 480;

  bool begin();                         // backlight on, panel running, both buffers black
  uint16_t *framebuffer(int index);     // 0 or 1

  // Make `fb` the displayed buffer from the next frame on, then block until the
  // panel has actually switched (next vsync; <= ~26 ms at 16 MHz PCLK).
  void show(uint16_t *fb);

  uint32_t vsyncCount();                // total vsync interrupts (refresh-rate check)
  // Bounce mode, since boot: frames, frames with a late refill (> 250 us, shows as a left-edge
  // smear), and the worst delay.
  struct RefillStats { uint32_t frames, typicalUs, late300, late600, late1000, worstLateUs; };
  RefillStats refillStats(bool reset = false);   // since boot / since the last reset

  // Diagnostics since the last call: frames shown, how many had a vsync fire while
  // esp_lcd_panel_draw_bitmap() was running, and the longest draw_bitmap() call.
  void takeShowStats(uint32_t &shows, uint32_t &races, uint32_t &drawUsMax);
}
