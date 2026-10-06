#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>

static constexpr int LCD_WIDTH = 800;
static constexpr int LCD_HEIGHT = 480;
static constexpr int LCD_BL = 2;

static constexpr int TOUCH_SDA = 19;
static constexpr int TOUCH_SCL = 20;
static constexpr int TOUCH_RST = 38;
static constexpr int TOUCH_INT = -1;
static constexpr uint8_t GT911_ADDR_PRIMARY = 0x5D;

// Source examples report GT911 raw coordinates around 480 x 272, mapped to 800 x 480.
static constexpr int TOUCH_MAP_X1 = 480;
static constexpr int TOUCH_MAP_X2 = 0;
static constexpr int TOUCH_MAP_Y1 = 272;
static constexpr int TOUCH_MAP_Y2 = 0;

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
  45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
  5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
  8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
  0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
  0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
  1 /* pclk_active_neg */, 16000000 /* prefer_speed */, false /* useBigEndian */,
  0 /* de_idle_high */, 0 /* pclk_idle_high */, 0 /* bounce_buffer_size_px */
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */, true /* auto_flush */
);
TAMC_GT911 touch(
  TOUCH_SDA,
  TOUCH_SCL,
  TOUCH_INT,
  TOUCH_RST,
  max(TOUCH_MAP_X1, TOUCH_MAP_X2),
  max(TOUCH_MAP_Y1, TOUCH_MAP_Y2)
);

static bool i2cAddressResponds(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static int clampScreenX(long value)
{
  return constrain(static_cast<int>(value), 0, LCD_WIDTH - 1);
}

static int clampScreenY(long value)
{
  return constrain(static_cast<int>(value), 0, LCD_HEIGHT - 1);
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-8048S043 LCD_TOUCH_TEST start");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("FAIL: gfx->begin() returned false");
    return;
  }

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  if (!i2cAddressResponds(GT911_ADDR_PRIMARY)) {
    Serial.println("FAIL: GT911 not found at 0x5D");
    gfx->fillScreen(RGB565_RED);
    gfx->setTextColor(RGB565_WHITE, RGB565_RED);
    gfx->setTextSize(3);
    gfx->setCursor(40, 80);
    gfx->print("GT911 not found at 0x5D");
    return;
  }

  touch.begin();
  touch.setRotation(ROTATION_NORMAL);

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE, RGB565_BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(30, 30);
  gfx->print("LCD + TOUCH TEST");
  gfx->setTextSize(2);
  gfx->setCursor(30, 80);
  gfx->print("Touch screen to draw circles.");

  Serial.println("PASS: LCD initialized and GT911 detected at 0x5D");
  Serial.println("Touch the panel; mapped coordinates will print below.");
}

void loop()
{
  touch.read();
  if (touch.isTouched && touch.touches > 0) {
    const int rawX = touch.points[0].x;
    const int rawY = touch.points[0].y;
    const int x = clampScreenX(map(rawX, TOUCH_MAP_X1, TOUCH_MAP_X2, 0, LCD_WIDTH - 1));
    const int y = clampScreenY(map(rawY, TOUCH_MAP_Y1, TOUCH_MAP_Y2, 0, LCD_HEIGHT - 1));

    gfx->fillCircle(x, y, 8, RGB565_YELLOW);
    gfx->drawCircle(x, y, 12, RGB565_BLUE);
    Serial.printf("TOUCH: raw=(%d,%d) mapped=(%d,%d)\r\n", rawX, rawY, x, y);
    delay(30);
  }
}
