#include "RgbPanel.h"

#include <Arduino.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp32s3/rom/cache.h>
#include <soc/gdma_struct.h>
#include "../config.h"

namespace {

esp_lcd_panel_handle_t s_panel = nullptr;
uint16_t *s_fb[2] = {};
SemaphoreHandle_t s_vsync = nullptr;
volatile uint32_t s_vsyncCount = 0;
uint32_t s_shows = 0, s_races = 0, s_drawUsMax = 0;

// ---- Own page flip (LCD_OWN_FLIP) ------------------------------------------------------------
// esp_lcd's switch to its 2nd framebuffer does not work here (IDF 5.5.4, restart-in-vsync):
// measured 2026-09-29 with "scan", after draw_bitmap(fb1) the LCD DMA reads fb1's first ~5
// rows, then continues in fb0's descriptor chain - every 2nd frame is never shown and the next
// one is drawn straight into the scanned buffer (visible flicker when screens change).
// So the driver stays on fb0's chain, which it follows reliably, and NAV-1 flips by pointing
// that chain's descriptors at the buffer to show: first the top ones (already scanned this frame),
// the rest in the vsync interrupt, long before the DMA reaches them.
constexpr int MAX_DSCR = 256, EARLY = 3;          // EARLY: descriptors retargeted before the vsync
uint32_t *s_dscr[MAX_DSCR];                       // fb0's chain, in scan order
uint32_t s_off[MAX_DSCR];                         // each descriptor's offset in the frame
int s_nDscr = 0, s_gdma = -1;
volatile uintptr_t s_flipTo = 0;                  // buffer whose lower part the next vsync switches to
bool s_ownFlip = false;

void IRAM_ATTR retarget(int from, int to, uintptr_t base) {
  for (int i = from; i < to; i++) s_dscr[i][1] = base + s_off[i];
}

// Late bounce refills: one timestamp per frame. The driver's refill that wraps to row 0 comes a
// fixed time after the vsync; a refill held up by bus contention moves it later, and the panel then shows
// the previous strip's pixels at the start of rows (the left-edge smear). Histogram of that time in 50 us
// steps; RgbPanel::refillStats() measures lateness against the typical frame (10th percentile) - the old
// "fastest frame" reference was fooled by one fast frame at boot (2026-09-30).
constexpr uint32_t HIST_FROM_US = 18000, HIST_STEP_US = 50, HIST_N = 240;   // 18.0 .. 30.0 ms
volatile uint32_t s_vsUs = 0, s_hist[HIST_N] = {};
bool IRAM_ATTR onFrameDone(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  const uint32_t dt = (uint32_t)esp_timer_get_time() - s_vsUs;
  if (s_vsUs && dt < 40000) {
    const uint32_t b = dt <= HIST_FROM_US ? 0 : min<uint32_t>((dt - HIST_FROM_US) / HIST_STEP_US, HIST_N - 1);
    s_hist[b] = s_hist[b] + 1;
  }
  return false;
}

bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  s_vsUs = (uint32_t)esp_timer_get_time();
  if (s_flipTo) {                                 // DMA just restarted at the chain head
    retarget(EARLY, s_nDscr, s_flipTo);
    s_flipTo = 0;
  }
  s_vsyncCount = s_vsyncCount + 1;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_vsync, &woken);
  return woken == pdTRUE;
}

// Offset in the frame of the descriptor the LCD DMA reads right now (-1 = not in a frame buffer).
int32_t scanOffset() {
  const uint32_t *d = (const uint32_t *)GDMA.channel[s_gdma].out.dscr;
  if (!d) return -1;
  const uintptr_t b = d[1], bytes = (uintptr_t)RgbPanel::WIDTH * RgbPanel::HEIGHT * 2;
  for (int i = 0; i < 2; i++)
    if (b >= (uintptr_t)s_fb[i] && b < (uintptr_t)s_fb[i] + bytes) return (int32_t)(b - (uintptr_t)s_fb[i]);
  return -1;
}

// Records fb0's descriptor chain: catch the DMA on the chain head (offset 0), then follow `next`.
__attribute__((unused)) bool captureChain() {   // used when LCD_BOUNCE_LINES == 0
  for (int i = 0; i < 5; i++) if (GDMA.channel[i].out.peri_sel.sel == 5) s_gdma = i;   // 5 = LCD_CAM
  if (s_gdma < 0) return false;
  const uintptr_t f0 = (uintptr_t)s_fb[0], bytes = (uintptr_t)RgbPanel::WIDTH * RgbPanel::HEIGHT * 2;
  uint32_t *head = nullptr;
  for (const uint32_t t0 = millis(); !head && millis() - t0 < 500;) {
    uint32_t *d = (uint32_t *)GDMA.channel[s_gdma].out.dscr;
    if (d && d[1] == f0) head = d;
  }
  if (!head) return false;
  s_nDscr = 0;
  for (uint32_t *d = head; d && s_nDscr < MAX_DSCR; d = (uint32_t *)d[2]) {
    if (d[1] < f0 || d[1] >= f0 + bytes) break;       // left fb0: end of the frame
    if (s_nDscr && d == head) break;                    // circular chain
    s_dscr[s_nDscr] = d;
    s_off[s_nDscr] = d[1] - f0;
    s_nDscr++;
  }
  return s_nDscr > EARLY;
}

}  // namespace

bool RgbPanel::begin() {
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH);

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = WIDTH;
  cfg.timings.v_res = HEIGHT;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.hsync_idle_low = 1;     // hsync polarity 0
  cfg.timings.flags.vsync_idle_low = 1;     // vsync polarity 0
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.timings.flags.pclk_idle_high = 0;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;                          // <- the only change vs Arduino_GFX
  // Bounce buffers (LCD_BOUNCE_LINES > 0): the panel DMA reads two small internal-RAM buffers that
  // the driver refills from the current framebuffer in an interrupt - its framebuffer switch does
  // not depend on the DMA descriptor chain, and PSRAM bus contention cannot starve the panel.
  cfg.bounce_buffer_size_px = LCD_BOUNCE_LINES * WIDTH;
  cfg.dma_burst_size = 64;                  // = Arduino_GFX's psram_trans_align (same union field)
  cfg.hsync_gpio_num = LCD_HSYNC_PIN;
  cfg.vsync_gpio_num = LCD_VSYNC_PIN;
  cfg.de_gpio_num = LCD_DE_PIN;
  cfg.pclk_gpio_num = LCD_PCLK_PIN;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  const int data[16] = { LCD_B0_PIN, LCD_B1_PIN, LCD_B2_PIN, LCD_B3_PIN, LCD_B4_PIN,
                         LCD_G0_PIN, LCD_G1_PIN, LCD_G2_PIN, LCD_G3_PIN, LCD_G4_PIN, LCD_G5_PIN,
                         LCD_R0_PIN, LCD_R1_PIN, LCD_R2_PIN, LCD_R3_PIN, LCD_R4_PIN };
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
  cfg.flags.disp_active_low = 1;
  cfg.flags.fb_in_psram = 1;

  s_vsync = xSemaphoreCreateBinary();
  if (!s_vsync) return false;
  if (esp_lcd_new_rgb_panel(&cfg, &s_panel) != ESP_OK) return false;

  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  if (LCD_BOUNCE_LINES > 0) cbs.on_frame_buf_complete = onFrameDone;   // late-refill counter
  if (esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr) != ESP_OK) return false;
  if (esp_lcd_panel_reset(s_panel) != ESP_OK) return false;
  if (esp_lcd_panel_init(s_panel) != ESP_OK) return false;

  void *fb0 = nullptr, *fb1 = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1) != ESP_OK) return false;
  s_fb[0] = (uint16_t *)fb0;
  s_fb[1] = (uint16_t *)fb1;
  const size_t bytes = (size_t)WIDTH * HEIGHT * 2;
  for (int i = 0; i < 2; i++) {
    memset(s_fb[i], 0, bytes);
    Cache_WriteBack_Addr((uint32_t)s_fb[i], bytes);
  }
  // The panel is fed from PSRAM by GDMA and must never wait: when its DMA loses the bus to
  // other GDMA channels (NAV-1's buffer copies, the SD card's SPI) the rest of that frame is
  // shifted sideways (seen on video 2026-09-29 as doubled tiles after taps). All channels had
  // priority 0; the LCD channel gets the highest.
  for (int i = 0; i < 5; i++)
    if (GDMA.channel[i].out.peri_sel.sel == 5) { GDMA.channel[i].out.pri.tx_pri = 9; s_gdma = i; }
#if LCD_BOUNCE_LINES > 0
  Serial.printf("[LCD] bounce buffers: 2 x %d rows in internal RAM, esp_lcd switches the framebuffers\n", LCD_BOUNCE_LINES);
#endif
#if LCD_OWN_FLIP && LCD_BOUNCE_LINES == 0
  s_ownFlip = captureChain();
  Serial.printf("[LCD] page flip: %s (%d descriptors)\n", s_ownFlip ? "own (descriptor retarget)" : "esp_lcd (chain not found)",
                s_nDscr);
#endif
  return true;
}

uint16_t *RgbPanel::framebuffer(int index) { return s_fb[index & 1]; }

void RgbPanel::show(uint16_t *fb) {
  if (s_ownFlip) {
    // Top descriptors: only once the scan is past them in this frame (else this frame's top
    // rows would already come from the new buffer). Then the vsync interrupt does the rest.
    const int32_t early = (int32_t)s_off[EARLY];
    for (const uint32_t t0 = micros(); micros() - t0 < 5000;) {
      const int32_t o = scanOffset();
      if (o < 0 || o >= early) break;
      delayMicroseconds(20);
    }
    xSemaphoreTake(s_vsync, 0);                                   // drop a stale vsync
    const uint32_t t0 = micros();
    const int32_t before = scanOffset();
    retarget(0, EARLY, (uintptr_t)fb);
    s_flipTo = (uintptr_t)fb;
    s_shows++;
    const uint32_t dt = micros() - t0;
    if (dt > s_drawUsMax) s_drawUsMax = dt;
    // A "race" = a flip that could show a mixed frame: the top descriptors were retargeted while
    // the DMA was still in them, or the vsync came and went without applying the rest.
    if (before >= 0 && before < (int32_t)s_off[EARLY]) s_races++;
    if (xSemaphoreTake(s_vsync, pdMS_TO_TICKS(60)) != pdTRUE || s_flipTo) s_races++;
    return;
  }
  xSemaphoreTake(s_vsync, 0);                                     // drop a stale vsync
  const uint32_t v0 = s_vsyncCount, t0 = micros();
  // Passing one of the driver's own framebuffers makes esp_lcd switch the scan-out
  // to it (no copy).
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, fb);
  const uint32_t dt = micros() - t0;
  s_shows++;
  if (s_vsyncCount != v0) s_races++;                              // a vsync fired during the call
  if (dt > s_drawUsMax) s_drawUsMax = dt;
  // The driver applies the new buffer in the NEXT vsync interrupt (CONFIG_LCD_RGB_RESTART_IN_VSYNC).
  // A vsync that fired while draw_bitmap() was running may not have switched yet; returning
  // on it let LVGL draw into the buffer still being scanned out (measured: 1 in 59 frames).
  // Drop it and wait for a vsync that certainly happened after the request.
  xSemaphoreTake(s_vsync, 0);
  xSemaphoreTake(s_vsync, pdMS_TO_TICKS(60));                     // wait for the switch
}

void RgbPanel::takeShowStats(uint32_t &shows, uint32_t &races, uint32_t &drawUsMax) {
  shows = s_shows; races = s_races; drawUsMax = s_drawUsMax;
  s_shows = s_races = s_drawUsMax = 0;
}

RgbPanel::RefillStats RgbPanel::refillStats(bool reset) {
  uint32_t h[HIST_N];
  for (uint32_t i = 0; i < HIST_N; i++) { h[i] = s_hist[i]; if (reset) s_hist[i] = 0; }
  RefillStats r = {};
  for (uint32_t i = 0; i < HIST_N; i++) r.frames += h[i];
  if (!r.frames) return r;
  uint32_t acc = 0, p10 = 0;
  for (uint32_t i = 0; i < HIST_N; i++) { acc += h[i]; if (acc * 10 >= r.frames) { p10 = i; break; } }
  r.typicalUs = HIST_FROM_US + p10 * HIST_STEP_US;
  for (uint32_t i = 0; i < HIST_N; i++) {
    if (!h[i]) continue;
    const uint32_t lateUs = i > p10 ? (i - p10) * HIST_STEP_US : 0;
    if (lateUs >= 300) r.late300 += h[i];
    if (lateUs >= 600) r.late600 += h[i];
    if (lateUs >= 1000) r.late1000 += h[i];
    r.worstLateUs = lateUs;
  }
  return r;
}

uint32_t RgbPanel::vsyncCount() { return s_vsyncCount; }
