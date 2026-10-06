#include "LvglPort.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp32s3/rom/cache.h>
#include <esp_async_memcpy.h>
#include <freertos/semphr.h>
#include "../config.h"
#include "Display.h"
#include "RgbPanel.h"
#include "TouchPort.h"

// The project's lv_conf.h must be the one installed next to the lvgl library.
#if !defined(NAV1_LV_CONF_REV) || NAV1_LV_CONF_REV != 5
#error "Installed lv_conf.h is not firmware revision 5. Copy firmware/config/lv_conf.h into the Arduino libraries folder."
#endif

namespace {

lv_display_t *s_disp = nullptr;
lv_indev_t *s_indev = nullptr;
LvglPort::TouchGate s_touchGate = nullptr;
int s_mode = LVGL_RENDER_PARTIAL;
uint16_t *s_shown = nullptr;        // framebuffer currently on screen (double mode)

LvglPort::FrameStats s_stats = {};
LvglPort::FrameListener s_listener = nullptr;
uint32_t s_renderStartUs = 0;

bool s_wasPressed = false;
bool s_pressPending = false;
uint32_t s_pressUs = 0;
uint32_t s_latencyUs = 0;
uint32_t s_touchBatch = 0;

uint32_t tickMs() { return millis(); }

void logPrint(lv_log_level_t, const char *buf) { Serial.print(buf); }

// ---- GDMA copies (PSRAM framebuffers) ------------------------------------------------
// Blocking memcpy on the GDMA engine; returns false if the copy was not done (the caller
// then copies with the CPU). Measured for a full-screen PSRAM->PSRAM copy with the panel
// running: CPU ~50 ms, GDMA 32-byte bursts ~42 ms, 64-byte bursts ~27 ms. A 64-byte burst
// needs a 64-byte aligned destination; esp_lcd places the 2nd framebuffer only 32-byte
// aligned, so copies into it use a 32-byte-burst channel.
async_memcpy_handle_t s_dma64 = nullptr, s_dma32 = nullptr;
SemaphoreHandle_t s_dmaDone = nullptr;
bool s_dmaReady = false, s_dmaBroken = false;

bool IRAM_ATTR onDmaDone(async_memcpy_handle_t, async_memcpy_event_t *, void *) {
  BaseType_t w = pdFALSE;
  xSemaphoreGiveFromISR(s_dmaDone, &w);
  return w == pdTRUE;
}

bool dmaInit() {
  if (s_dmaReady || s_dmaBroken) return s_dmaReady;
  async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
  // One transaction at a time (dmaCopy() blocks). The driver keeps a descriptor list per
  // backlog slot sized to the largest copy it served (~8 KB internal RAM for a full screen);
  // the default backlog of 8 grew internal RAM use by ~8 KB per screen change (2026-09-28).
  cfg.backlog = 1;
  s_dmaDone = xSemaphoreCreateBinary();
  cfg.dma_burst_size = 64;
  bool ok = s_dmaDone && esp_async_memcpy_install_gdma_ahb(&cfg, &s_dma64) == ESP_OK;
  cfg.dma_burst_size = 32;
  ok = ok && esp_async_memcpy_install_gdma_ahb(&cfg, &s_dma32) == ESP_OK;
  if (!ok) {
    s_dmaBroken = true;
    Serial.println("[LVGL] GDMA copy unavailable: CPU copies");
    return false;
  }
  s_dmaReady = true;
  return true;
}

bool dmaCopy(void *dst, const void *src, size_t n) {
  if (!dmaInit()) return false;
  if (((uintptr_t)dst | (uintptr_t)src | n) % 32) return false;
  async_memcpy_handle_t h = ((uintptr_t)dst % 64 == 0 && n % 64 == 0) ? s_dma64 : s_dma32;
  xSemaphoreTake(s_dmaDone, 0);
  if (esp_async_memcpy(h, dst, (void *)src, n, onDmaDone, nullptr) != ESP_OK) return false;
  if (xSemaphoreTake(s_dmaDone, pdMS_TO_TICKS(200)) != pdTRUE) {
    s_dmaBroken = true;                // never seen; don't risk it again
    Serial.println("[LVGL] GDMA copy timed out: CPU copies from now on");
    return false;
  }
  return true;
}

// ---- display ------------------------------------------------------------------

void flushPartial(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  const uint32_t t0 = micros();
  Display::gfx()->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)px,
                                     lv_area_get_width(a), lv_area_get_height(a));
  s_stats.flushUsSum += micros() - t0;
  s_stats.flushCalls++;
  lv_display_flush_ready(disp);
}

void flushDirect(lv_display_t *disp, const lv_area_t *a, uint8_t *) {
  // LVGL already wrote the pixels into the framebuffer; push the touched rows
  // out of the CPU cache so the panel's DMA sees them.
  const uint32_t t0 = micros();
  uint16_t *row0 = Display::framebuffer() + (int32_t)a->y1 * Display::WIDTH;
  const uint32_t bytes = (uint32_t)(a->y2 - a->y1 + 1) * Display::WIDTH * 2;
  Cache_WriteBack_Addr((uint32_t)row0, bytes);
  s_stats.flushUsSum += micros() - t0;
  s_stats.flushCalls++;
  lv_display_flush_ready(disp);
}

// ---- stale-pixel check (diagnostic) ------------------------------------------------
// Reads sample rows of the shown framebuffer straight from PSRAM with the GDMA engine
// (bypasses the CPU cache = what the panel scans out) and compares them with the CPU's
// cached view. Differences = pixels the CPU wrote that the panel does not show yet.
bool s_staleCheck = false;
uint32_t s_staleLast = 0;
uint16_t *s_rowBuf = nullptr;

uint32_t countStale(const uint16_t *fb) {
  if (!s_rowBuf) {
    s_rowBuf = (uint16_t *)heap_caps_aligned_alloc(64, RgbPanel::WIDTH * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_rowBuf) return UINT32_MAX;
  }
  uint32_t diff = 0;
  for (int y = 0; y < RgbPanel::HEIGHT; y += 8) {
    const uint16_t *row = fb + y * RgbPanel::WIDTH;
    if (!dmaCopy(s_rowBuf, row, RgbPanel::WIDTH * 2)) return UINT32_MAX;
    Cache_Invalidate_Addr((uint32_t)s_rowBuf, RgbPanel::WIDTH * 2);
    for (int x = 0; x < RgbPanel::WIDTH; x++) diff += s_rowBuf[x] != row[x];
  }
  return diff;
}

// Double-buffered direct mode: LVGL renders into the hidden buffer (px_map), one call per
// dirty area. On the last area of the frame, make that buffer the visible one and wait for
// vsync, so the panel never shows a half-drawn frame and LVGL never draws into the buffer
// being scanned out.
//
// Cache write-back: the panel scans PSRAM, the CPU writes through its cache. Before each
// frame the back buffer is also brought up to date with the previous frame's changes
// (syncDouble), outside the areas passed to this callback. Writing back only the flushed
// areas left such pixels in the cache, so the panel showed stale PSRAM content there until
// the cache evicted it ("pixels dancing" after a full-screen hand-off, 2026-09-28). The
// whole buffer is written back before it is shown; clean lines cost almost nothing.
void flushDouble(lv_display_t *disp, const lv_area_t *, uint8_t *px) {
  const uint32_t t0 = micros();
  uint16_t *fb = (uint16_t *)px;
  if (lv_display_flush_is_last(disp)) {
    Cache_WriteBack_Addr((uint32_t)fb, RgbPanel::WIDTH * RgbPanel::HEIGHT * 2);
    const uint32_t wb = micros() - t0;
    if (wb > s_stats.writebackUsMax) s_stats.writebackUsMax = wb;
    RgbPanel::show(fb);
    s_shown = fb;
    if (s_staleCheck) s_staleLast = countStale(fb);
  }
  s_stats.flushUsSum += micros() - t0;
  s_stats.flushCalls++;
  lv_display_flush_ready(disp);
}

// Double-buffered direct mode: before rendering a frame into the back buffer, LVGL copies
// the areas changed in the previous frame from the shown buffer, so both stay identical.
// LVGL's own copy is a CPU memcpy PSRAM->PSRAM (~48 ms for a full screen, e.g. the frame
// after an app or Home screen appears). Full-width bands (all full-screen changes) go
// through GDMA instead (~27-43 ms, and the CPU cache stays clean); narrow areas stay on
// the CPU (their lines are written back in flushDouble).
constexpr int32_t SYNC_DMA_MIN_ROWS = 16;

void syncDouble(lv_display_t *disp, const lv_area_t *a) {
  const uint32_t t0 = micros();
  bool viaDma = false;
  uint16_t *dst = (uint16_t *)lv_display_get_buf_active(disp)->data;
  const uint16_t *src = s_shown;
  if (src && dst != src) {
    constexpr int32_t W = RgbPanel::WIDTH;
    const int32_t rows = a->y2 - a->y1 + 1;
    bool done = false;
    // Not with bounce buffers: a GDMA PSRAM->PSRAM copy starves the bounce refill (its CPU reads of
    // the shown buffer). Measured 2026-09-29, full-screen copies for 2 s at idle: GDMA 64-byte bursts
    // -> 0-1 of ~79 frames refilled on time, 32-byte bursts -> 2-4 late (up to 1.1 ms), CPU -> none.
    // Seen as a clock-wide smear down the left edge right after entering/exiting an app.
    if (LCD_BOUNCE_LINES == 0 && a->x1 == 0 && a->x2 == W - 1 && rows >= SYNC_DMA_MIN_ROWS) {
      uint16_t *d = dst + a->y1 * W;
      const uint16_t *s = src + a->y1 * W;
      const uint32_t bytes = (uint32_t)rows * W * 2;
      Cache_WriteBack_Addr((uint32_t)s, bytes);   // source is clean (written back when shown): no-op
      Cache_WriteBack_Addr((uint32_t)d, bytes);   // no dirty line may later overwrite the DMA data
      done = dmaCopy(d, s, bytes);
      Cache_Invalidate_Addr((uint32_t)d, bytes);  // the CPU must read the new PSRAM content
      viaDma = done;
      if (done) s_stats.syncDmaAreas++;
    }
    if (!done) {
      const uint32_t bytes = (uint32_t)(a->x2 - a->x1 + 1) * 2;
      for (int32_t y = a->y1; y <= a->y2; y++) memcpy(dst + y * W + a->x1, src + y * W + a->x1, bytes);
    }
  }
  const uint32_t dt = micros() - t0;
  s_stats.syncs++;
  s_stats.syncUsSum += dt;
  if (dt > s_stats.syncUsMax) s_stats.syncUsMax = dt;
  if (viaDma && dt > s_stats.syncDmaUsMax) s_stats.syncDmaUsMax = dt;
  const uint32_t kb = lv_area_get_size(a) * 2 / 1024;
  if (kb > s_stats.syncMaxKB) s_stats.syncMaxKB = kb;
  lv_display_sync_ready(disp);
}

#if LCD_PORTRAIT
// ---- Portrait (LCD_PORTRAIT) ------------------------------------------------------------------
// LVGL renders the 480 x 800 portrait UI in PARTIAL strips. The panel scans 800 x 480 and is held with
// its x = 799 edge on top, so panel pixel (x, y) shows portrait pixel (u = y, v = 799 - x). Both PSRAM
// framebuffers keep the panel's own layout (the refill path is untouched); the flush rotates
// each strip into the hidden one (rows of the panel are written sequentially, the strip is read with
// a stride from the small buffer), and the sync callback copies the areas LVGL says the hidden buffer
// lacks (changed last frame, not redrawn now) from the shown one. A refill-time rotation was measured
// and rejected: 570 us per 8-row strip against 408 us available.
constexpr int PW = RgbPanel::WIDTH;                 // panel row length in pixels

inline uint16_t *hiddenFb() { return s_shown == RgbPanel::framebuffer(0) ? RgbPanel::framebuffer(1) : RgbPanel::framebuffer(0); }

void flushPortrait(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
  const uint32_t t0 = micros();
  uint16_t *dst = hiddenFb();
  const uint16_t *src = (const uint16_t *)px;
  const int32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
  const int32_t stride = lv_display_get_buf_active(disp)->header.stride / 2;
  // Blocks of 16 portrait rows (v): their 16 source rows stay in the data cache while every column (u) of
  // the strip is visited, and each panel row receives 16 consecutive pixels (one cache line per write).
  const uint32_t tr = micros();
  uint16_t *rowBase = dst + a->x1 * PW + (PW - 1 - a->y1);              // panel (x, y) of portrait (u = x1, v = y1)
  const bool pairs = ((a->x1 | w) & 1) == 0;
  for (int32_t j0 = 0; j0 < h; j0 += 16) {
    const int32_t jn = min<int32_t>(16, h - j0);
    if (pairs) {
      for (int32_t i = 0; i < w; i += 2) {
        uint16_t *d0 = rowBase + i * PW - j0, *d1 = d0 + PW;
        const uint32_t *s = (const uint32_t *)(src + j0 * stride + i);
        for (int32_t j = 0; j < jn; j++, s += stride >> 1) {
          const uint32_t two = *s;                                      // u and u + 1 of portrait row v
          d0[-j] = (uint16_t)two;
          d1[-j] = (uint16_t)(two >> 16);
        }
      }
    } else {
      for (int32_t i = 0; i < w; i++) {
        uint16_t *d0 = rowBase + i * PW - j0;
        const uint16_t *s = src + j0 * stride + i;
        for (int32_t j = 0; j < jn; j++, s += stride) d0[-j] = *s;
      }
    }
  }
  s_stats.rotateUsSum += micros() - tr;
  if (lv_display_flush_is_last(disp)) {
    Cache_WriteBack_Addr((uint32_t)dst, RgbPanel::WIDTH * RgbPanel::HEIGHT * 2);
    const uint32_t wb = micros() - t0;
    if (wb > s_stats.writebackUsMax) s_stats.writebackUsMax = wb;
    RgbPanel::show(dst);
    s_shown = dst;
    if (s_staleCheck) s_staleLast = countStale(dst);
  }
  s_stats.flushUsSum += micros() - t0;
  s_stats.flushCalls++;
  lv_display_flush_ready(disp);
}

void syncPortrait(lv_display_t *disp, const lv_area_t *a) {
  const uint32_t t0 = micros();
  uint16_t *dst = hiddenFb();
  const uint16_t *src = s_shown;
  if (src && dst != src) {
    const int32_t x0 = PW - 1 - a->y2, bytes = (a->y2 - a->y1 + 1) * 2;   // panel columns of portrait rows y1..y2
    for (int32_t u = a->x1; u <= a->x2; u++) memcpy(dst + u * PW + x0, src + u * PW + x0, bytes);
  }
  const uint32_t dt = micros() - t0;
  s_stats.syncs++;
  s_stats.syncUsSum += dt;
  if (dt > s_stats.syncUsMax) s_stats.syncUsMax = dt;
  const uint32_t kb = lv_area_get_size(a) * 2 / 1024;
  if (kb > s_stats.syncMaxKB) s_stats.syncMaxKB = kb;
  lv_display_sync_ready(disp);
}
#endif

void onRenderStart(lv_event_t *) { s_renderStartUs = micros(); }

void onRenderReady(lv_event_t *) {
  const uint32_t end = micros();
  const uint32_t dur = end - s_renderStartUs;
  s_stats.frames++;
  s_stats.renderUsSum += dur;
  if (dur > s_stats.renderUsMax) s_stats.renderUsMax = dur;
  if (s_pressPending) {
    s_latencyUs = end - s_pressUs;
    s_pressPending = false;
  }
  if (s_listener) s_listener(s_renderStartUs, end);
}

// ---- touch ----------------------------------------------------------------------------
// Delivers every queued press / move / release (continue_reading) so a tap or swipe that
// happened during a long frame still reaches LVGL completely and in order.
void touchRead(lv_indev_t *, lv_indev_data_t *data) {
  TouchPort::Event e;
  bool more = false;
  const bool got = TouchPort::pop(e, more);
#if LCD_PORTRAIT
  data->point.x = e.y;                                  // panel (x, y) shows portrait (u = y, v = 799 - x)
  data->point.y = RgbPanel::WIDTH - 1 - e.x;
#else
  data->point.x = e.x;
  data->point.y = e.y;
#endif
  data->state = e.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->continue_reading = more;
  if (s_touchGate && !s_touchGate(e.pressed)) data->state = LV_INDEV_STATE_RELEASED;   // e.g. the touch that wakes the display

  if (got) {
    s_stats.touchEvents++;
    s_touchBatch++;
    if (e.pressed && !s_wasPressed) {
      s_pressUs = e.us;
      s_pressPending = true;
    }
    s_wasPressed = e.pressed;
  }
  if (!more) {
    if (s_touchBatch > s_stats.touchBatchMax) s_stats.touchBatchMax = s_touchBatch;
    s_touchBatch = 0;
  }
}

}  // namespace

// ---- public -----------------------------------------------------------------------

bool LvglPort::begin(int mode) {
  s_mode = mode;
  if (mode == LVGL_RENDER_DOUBLE) {
    if (!RgbPanel::begin()) return false;         // Arduino_GFX panel is NOT started
  } else if (!Display::begin()) {
    return false;
  }

  lv_init();
  lv_tick_set_cb(tickMs);
  lv_log_register_print_cb(logPrint);

  s_disp = lv_display_create(Display::UI_W, Display::UI_H);
#if LCD_PORTRAIT
  if (mode == LVGL_RENDER_DOUBLE) {
    const uint32_t bytes = Display::UI_W * LVGL_PARTIAL_LINES * 2;
    void *buf = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);   // internal: render + rotate stay off the PSRAM bus
    if (!buf) return false;
    lv_display_set_flush_cb(s_disp, flushPortrait);
    lv_display_set_buffers(s_disp, buf, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_sync_cb(s_disp, syncPortrait);
    s_shown = RgbPanel::framebuffer(0);
  } else
#endif
  if (mode == LVGL_RENDER_DOUBLE) {
    lv_display_set_flush_cb(s_disp, flushDouble);
    lv_display_set_buffers(s_disp, RgbPanel::framebuffer(0), RgbPanel::framebuffer(1),
                           RgbPanel::WIDTH * RgbPanel::HEIGHT * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_sync_cb(s_disp, syncDouble);
    s_shown = RgbPanel::framebuffer(0);
  } else if (mode == LVGL_RENDER_DIRECT) {
    lv_display_set_flush_cb(s_disp, flushDirect);
    lv_display_set_buffers(s_disp, Display::framebuffer(), nullptr,
                           Display::WIDTH * Display::HEIGHT * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
  } else {
    const uint32_t bytes = Display::WIDTH * LVGL_PARTIAL_LINES * 2;
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return false;
    lv_display_set_flush_cb(s_disp, flushPartial);
    lv_display_set_buffers(s_disp, buf, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  }
  // LVGL 9 makes the bottom layer opaque WHITE for formats without alpha (RGB565).
  // It is drawn whenever no single screen covers an area, i.e. in every frame of
  // a screen transition. With the panel scanning the framebuffer while LVGL draws,
  // that white fill was visible as 2-3 full-screen white flashes per transition
  // (M1, 2026-09-27). Screens are opaque and slide transitions always cover the
  // whole display, so the bottom layer must paint nothing.
  lv_obj_set_style_bg_opa(lv_display_get_layer_bottom(s_disp), LV_OPA_TRANSP, 0);

  lv_display_add_event_cb(s_disp, onRenderStart, LV_EVENT_RENDER_START, nullptr);
  lv_display_add_event_cb(s_disp, onRenderReady, LV_EVENT_RENDER_READY, nullptr);

  // GT911 after the panel (same order as the verified sketches); sampled by its own task.
  if (!TouchPort::begin()) return false;

  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, touchRead);
  return true;
}

uint32_t LvglPort::update() { return lv_timer_handler(); }

lv_display_t *LvglPort::display() { return s_disp; }
lv_indev_t *LvglPort::touch() { return s_indev; }
void LvglPort::setTouchGate(TouchGate gate) { s_touchGate = gate; }
bool LvglPort::touchPresent() { return TouchPort::present(); }
int LvglPort::mode() { return s_mode; }

void LvglPort::setStaleCheck(bool on) { s_staleCheck = on; }
uint32_t LvglPort::lastStalePixels() { return s_staleLast; }

const char *LvglPort::modeName() {
  return s_mode == LVGL_RENDER_DOUBLE ? "DOUBLE" : (s_mode == LVGL_RENDER_DIRECT ? "DIRECT" : "PARTIAL");
}

uint16_t *LvglPort::backFramebuffer() {
  if (s_mode != LVGL_RENDER_DOUBLE) return nullptr;
  return s_shown == RgbPanel::framebuffer(0) ? RgbPanel::framebuffer(1) : RgbPanel::framebuffer(0);
}

const uint16_t *LvglPort::shownFramebuffer() {
  return s_mode == LVGL_RENDER_DOUBLE ? s_shown : Display::framebuffer();
}

LvglPort::FrameStats LvglPort::takeFrameStats() {
  FrameStats s = s_stats;
  s_stats = {};
  return s;
}

void LvglPort::setFrameListener(FrameListener cb) { s_listener = cb; }

uint32_t LvglPort::takeTouchLatencyUs() {
  const uint32_t v = s_latencyUs;
  s_latencyUs = 0;
  return v;
}
