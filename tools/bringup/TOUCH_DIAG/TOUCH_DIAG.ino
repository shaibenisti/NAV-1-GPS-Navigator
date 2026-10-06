// =============================================================================
//  TOUCH_DIAG.ino  -  GT911 touch diagnostic (ESP32-8048S043C-I)
// -----------------------------------------------------------------------------
//  Reads the GT911 with a minimal own I2C driver (NOT TAMC_GT911: no config
//  rewrite, true raw data) and shows what the production firmware would make of
//  it (TAMC ROTATION_NORMAL + map() in LvglPort == raw * 799/480, raw * 479/272).
//
//  1. Boot: GT911 reset, address probe, product ID, firmware, stored config
//     (X/Y max, touch count, module switches, refresh rate).
//  2. Accuracy: 9 targets (corners, edges, centre). Tap each; the error between
//     target and mapped touch is logged. Summary on screen + serial.
//  3. Free mode: crosshair follows the finger; live raw/mapped, report rate,
//     I2C errors, dropouts (finger still down but one report says "no touch"),
//     report gaps > 40 ms, raw min/max seen.
//  Serial 115200, lines start with [TD]. Serial "again" restarts the targets.
//  Display: verified Arduino_GFX single-buffer config (docs/HARDWARE.md).
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

// ---- verified pins / config ----------------------------------------------------
static constexpr int W = 800, H = 480, LCD_BL = 2;
static constexpr int TOUCH_SDA = 19, TOUCH_SCL = 20, TOUCH_RST = 38;
static constexpr int RAW_W = 480, RAW_H = 272;          // mapping assumed by the firmware
static uint8_t g_addr = 0x5D;

// Declared before any function: Arduino inserts auto-prototypes above the first function.
struct TouchSample { bool valid; bool touched; int n; int rx, ry, size; };

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,
  45, 48, 47, 21, 14,
  5, 6, 7, 15, 16, 4,
  8, 3, 46, 9, 1,
  0, 8, 4, 8, 0, 8, 4, 8,
  1, 16000000, false, 0, 0, 0);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(W, H, rgbpanel, 0, true);

// ---- GT911 minimal driver ----------------------------------------------------------
static uint32_t g_i2cErrors = 0;

static bool gtRead(uint16_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(g_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) { g_i2cErrors++; return false; }
  if (Wire.requestFrom(g_addr, n) != n) { g_i2cErrors++; return false; }
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool gtWrite8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(g_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  if (Wire.endTransmission() != 0) { g_i2cErrors++; return false; }
  return true;
}

// One poll: returns valid=false if no new report is ready (or on I2C error).
static TouchSample gtPoll() {
  TouchSample s = {};
  uint8_t st;
  if (!gtRead(0x814E, &st, 1)) return s;
  if (!(st & 0x80)) return s;                     // buffer not ready: no new report
  s.valid = true;
  s.n = st & 0x0F;
  s.touched = s.n > 0;
  if (s.touched) {
    uint8_t p[8];
    if (gtRead(0x814F, p, 8)) {
      s.rx = p[1] | (p[2] << 8);
      s.ry = p[3] | (p[4] << 8);
      s.size = p[5] | (p[6] << 8);
    } else {
      s.valid = false;
    }
  }
  gtWrite8(0x814E, 0);                              // release the buffer for the next report
  return s;
}

static int mapX(int rx) { return constrain((int)((long)rx * (W - 1) / RAW_W), 0, W - 1); }
static int mapY(int ry) { return constrain((int)((long)ry * (H - 1) / RAW_H), 0, H - 1); }

// ---- drawing helpers -------------------------------------------------------------------
static void text(int x, int y, uint8_t size, uint16_t fg, const char *fmt, ...) {
  char b[96];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  gfx->setTextSize(size);
  gfx->setTextColor(fg, RGB565_BLACK);
  gfx->setCursor(x, y);
  gfx->print(b);
}

static void crosshair(int x, int y, uint16_t c) {
  gfx->drawFastHLine(x - 20, y, 41, c);
  gfx->drawFastVLine(x, y - 20, 41, c);
  gfx->drawCircle(x, y, 10, c);
}

// ---- accuracy targets ----------------------------------------------------------------------
static const int M = 40;   // margin from the edges
static const int TGT_X[9] = { M, W / 2, W - 1 - M, M, W / 2, W - 1 - M, M, W / 2, W - 1 - M };
static const int TGT_Y[9] = { M, M, M, H / 2, H / 2, H / 2, H - 1 - M, H - 1 - M, H - 1 - M };
static int g_target = 0;                // 0..8 targets, 9 = free mode
static int g_errX[9], g_errY[9];

static void showTarget() {
  gfx->fillScreen(RGB565_BLACK);
  text(220, 200, 2, RGB565_WHITE, "Tap the centre of target %d / 9", g_target + 1);
  crosshair(TGT_X[g_target], TGT_Y[g_target], RGB565_YELLOW);
  gfx->fillCircle(TGT_X[g_target], TGT_Y[g_target], 3, RGB565_YELLOW);
}

static void showSummary() {
  gfx->fillScreen(RGB565_BLACK);
  float sx = 0, sy = 0; int maxE = 0, maxI = 0;
  Serial.println("[TD] ===== accuracy summary (mapped - target, px) =====");
  for (int i = 0; i < 9; i++) {
    const int e = (int)sqrtf((float)g_errX[i] * g_errX[i] + (float)g_errY[i] * g_errY[i]);
    sx += g_errX[i]; sy += g_errY[i];
    if (e > maxE) { maxE = e; maxI = i; }
    Serial.printf("[TD] target %d (%3d,%3d): dx=%+4d dy=%+4d  |e|=%d\n", i + 1, TGT_X[i], TGT_Y[i], g_errX[i], g_errY[i], e);
    // draw where each tap landed vs. its target
    crosshair(TGT_X[i], TGT_Y[i], RGB565_DARKGREY);
    gfx->fillCircle(TGT_X[i] + g_errX[i], TGT_Y[i] + g_errY[i], 5, e <= 15 ? RGB565_GREEN : (e <= 30 ? RGB565_YELLOW : RGB565_RED));
  }
  Serial.printf("[TD] mean offset dx=%+.1f dy=%+.1f | worst |e|=%d px at target %d\n", sx / 9, sy / 9, maxE, maxI + 1);
  text(20, 20, 2, RGB565_WHITE, "Mean offset dx=%+.1f dy=%+.1f px   worst %d px (target %d)", sx / 9, sy / 9, maxE, maxI + 1);
  text(20, 440, 2, RGB565_LIGHTGREY, "Dots: green<=15px yellow<=30px red>30px. Touch to start free mode.");
}

// ---- state for statistics ---------------------------------------------------------------------
static bool g_down = false;
static uint32_t g_lastReportMs = 0, g_upCandidateMs = 0;
static uint32_t g_reportsSec = 0, g_pollsSec = 0, g_dropouts = 0, g_gaps = 0, g_maxGap = 0;
static int g_minRx = 9999, g_maxRx = -1, g_minRy = 9999, g_maxRy = -1;
static int g_lastX = -1, g_lastY = -1;
static int g_pressRx = 0, g_pressRy = 0, g_pressSamples = 0;
static long g_sumRx = 0, g_sumRy = 0;

static void dumpConfig() {
  uint8_t id[4] = {}, fw[2] = {}, res[4] = {};
  gtRead(0x8140, id, 4);
  gtRead(0x8144, fw, 2);
  gtRead(0x8146, res, 4);
  uint8_t cfg[16] = {};
  gtRead(0x8047, cfg, 16);
  Serial.printf("[TD] GT911 at 0x%02X: product \"%c%c%c%c\" fw 0x%02X%02X, resolution regs X=%u Y=%u\n", g_addr,
                id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?', id[3] ? id[3] : ' ',
                fw[1], fw[0], res[0] | (res[1] << 8), res[2] | (res[3] << 8));
  Serial.printf("[TD] config: version=0x%02X Xmax=%u Ymax=%u touches=%u module_sw1=0x%02X module_sw2=0x%02X refresh=%u (=%u ms)\n",
                cfg[0], cfg[1] | (cfg[2] << 8), cfg[3] | (cfg[4] << 8), cfg[5] & 0x0F, cfg[6], cfg[7],
                cfg[15] & 0x0F, 5 + (cfg[15] & 0x0F));
  Serial.printf("[TD] firmware mapping assumes raw %dx%d -> screen %dx%d\n", RAW_W, RAW_H, W, H);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[TD] ===== TOUCH_DIAG =====");
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  if (!gfx->begin()) Serial.println("[TD] gfx->begin FAILED");
  gfx->fillScreen(RGB565_BLACK);

  // GT911 reset (INT not driven: it is not connected to a known-free pin)
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);  delay(10);
  digitalWrite(TOUCH_RST, HIGH); delay(60);
  Wire.setBufferSize(256);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  for (uint8_t a : { (uint8_t)0x5D, (uint8_t)0x14 }) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { g_addr = a; break; }
  }
  dumpConfig();
  showTarget();
}

void loop() {
  static uint32_t secT0 = millis();
  const uint32_t now = millis();
  TouchSample s = gtPoll();
  g_pollsSec++;

  if (s.valid) {
    g_reportsSec++;
    if (g_down && g_lastReportMs && now - g_lastReportMs > 40) { g_gaps++; g_maxGap = max(g_maxGap, now - g_lastReportMs); }
    g_lastReportMs = now;

    if (s.touched) {
      if (!g_down && g_upCandidateMs && now - g_upCandidateMs < 80) g_dropouts++;   // "released" < 80 ms ago
      g_upCandidateMs = 0;
      if (!g_down) { g_down = true; g_pressRx = s.rx; g_pressRy = s.ry; g_sumRx = g_sumRy = 0; g_pressSamples = 0; }
      g_sumRx += s.rx; g_sumRy += s.ry; g_pressSamples++;
      g_minRx = min(g_minRx, s.rx); g_maxRx = max(g_maxRx, s.rx);
      g_minRy = min(g_minRy, s.ry); g_maxRy = max(g_maxRy, s.ry);

      if (g_target >= 10) {                       // free mode: follow the finger
        const int x = mapX(s.rx), y = mapY(s.ry);
        if (g_lastX >= 0) crosshair(g_lastX, g_lastY, RGB565_BLACK);
        gfx->drawPixel(x, y, RGB565_CYAN);
        crosshair(x, y, RGB565_GREEN);
        g_lastX = x; g_lastY = y;
        text(10, 10, 2, RGB565_WHITE, "raw %4d,%4d  ->  screen %4d,%4d  size %3d   ", s.rx, s.ry, x, y, s.size);
      }
    } else if (g_down) {                          // released
      g_down = false;
      g_upCandidateMs = now;
      const int ax = (int)(g_sumRx / max(1, g_pressSamples)), ay = (int)(g_sumRy / max(1, g_pressSamples));
      Serial.printf("[TD] tap: first raw %d,%d -> %d,%d | avg raw %d,%d -> %d,%d | %d reports\n",
                    g_pressRx, g_pressRy, mapX(g_pressRx), mapY(g_pressRy), ax, ay, mapX(ax), mapY(ay), g_pressSamples);
      if (g_target < 9) {                         // accuracy phase: score the first contact point
        g_errX[g_target] = mapX(g_pressRx) - TGT_X[g_target];
        g_errY[g_target] = mapY(g_pressRy) - TGT_Y[g_target];
        Serial.printf("[TD] target %d at %d,%d: error dx=%+d dy=%+d px\n", g_target + 1, TGT_X[g_target], TGT_Y[g_target],
                      g_errX[g_target], g_errY[g_target]);
        g_target++;
        if (g_target < 9) showTarget(); else showSummary();
      } else if (g_target == 9) {                 // summary shown: next tap starts free mode
        g_target = 10;
        gfx->fillScreen(RGB565_BLACK);
        text(10, 450, 2, RGB565_LIGHTGREY, "Free mode: draw anywhere. Serial 'again' = restart targets.");
      }
    }
  }

  if (now - secT0 >= 1000) {
    secT0 = now;
    if (g_down || g_reportsSec > 1 || g_i2cErrors)
      Serial.printf("[TD] 1s: reports=%u polls=%u i2c_err=%u dropouts=%u gaps>40ms=%u (max %u ms) raw x %d..%d y %d..%d\n",
                    (unsigned)g_reportsSec, (unsigned)g_pollsSec, (unsigned)g_i2cErrors, (unsigned)g_dropouts,
                    (unsigned)g_gaps, (unsigned)g_maxGap, g_minRx, g_maxRx, g_minRy, g_maxRy);
    if (g_target >= 10)
      text(10, 40, 2, RGB565_YELLOW, "reports/s %3u  i2c err %u  dropouts %u  gaps %u   ",
           (unsigned)g_reportsSec, (unsigned)g_i2cErrors, (unsigned)g_dropouts, (unsigned)g_gaps);
    g_reportsSec = g_pollsSec = 0;
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "again") { g_target = 0; g_lastX = -1; showTarget(); Serial.println("[TD] targets restarted"); }
  }
  delay(2);   // poll at ~400 Hz (well above the GT911 report rate)
}
