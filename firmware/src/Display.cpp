#include "Display.h"

#include "../config.h"

// Verbatim from the verified LCD_TEST / LCD_TOUCH_TEST / DRAW_PAD configuration.
static Arduino_ESP32RGBPanel *s_panel = new Arduino_ESP32RGBPanel(
  LCD_DE_PIN, LCD_VSYNC_PIN, LCD_HSYNC_PIN, LCD_PCLK_PIN,
  45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
  5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
  8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
  0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
  0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
  1 /* pclk_active_neg */, LCD_PCLK_HZ /* prefer_speed */, false /* useBigEndian */,
  0 /* de_idle_high */, 0 /* pclk_idle_high */, 0 /* bounce_buffer_size_px */
);

static Arduino_RGB_Display *s_gfx = new Arduino_RGB_Display(
  Display::WIDTH, Display::HEIGHT, s_panel, 0 /* rotation */, true /* auto_flush */
);

bool Display::begin() {
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH);
  if (!s_gfx->begin()) return false;
  s_gfx->fillScreen(RGB565_BLACK);
  return true;
}

Arduino_GFX *Display::gfx() { return s_gfx; }

uint16_t *Display::framebuffer() { return s_gfx->getFramebuffer(); }
