// =============================================================================
//  Display  -  The 800x480 RGB panel, configured exactly as verified in
//  docs/HARDWARE.md (16 MHz pixel clock, backlight GPIO2 active HIGH).
//  Owns the Arduino_GFX objects; screens only draw through gfx().
// =============================================================================
#pragma once

#include <Arduino_GFX_Library.h>
#include "../config.h"

namespace Display {
  constexpr int WIDTH  = 800;
  constexpr int HEIGHT = 480;

  // UI size as LVGL sees it: portrait (LCD_PORTRAIT) or the panel's own landscape
  constexpr int UI_W = LCD_PORTRAIT ? HEIGHT : WIDTH;
  constexpr int UI_H = LCD_PORTRAIT ? WIDTH : HEIGHT;

  bool begin();                 // backlight on + panel init; false if gfx->begin() fails
  Arduino_GFX *gfx();
  uint16_t *framebuffer();      // the panel's PSRAM framebuffer (800 x 480 RGB565)

  // RGB565 from 8-bit components
  constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
}
