#include "Shell.h"

#include <esp_heap_caps.h>
#include "../../config.h"
#include "../LvglPort.h"
#include "../Display.h"
#include "../services/MapRender.h"
#include "../services/MapTiles.h"
#include "../SdLog.h"
#include "../GpsCsv.h"
#include "../ui/Transitions.h"
#include "../RgbPanel.h"
#include "../TouchPort.h"
#include "../services/Settings.h"
#include "../services/WifiService.h"
#include "../services/BleService.h"
#include "../diag/Diag.h"
#include "../diag/Health.h"
#include "../diag/FieldRecorder.h"
#include "../diag/GpsReplay.h"
#include "../services/TripRecorder.h"
#include "../services/WebService.h"
#include "../services/Location.h"
#include "../services/GpsConfig.h"
#include "../services/Storage.h"
#include "../services/Assets.h"
#include "../services/Ota.h"
#include "../services/Backlight.h"
#include "../services/TimeService.h"
#include "../apps/App.h"

namespace {

// ---- services --------------------------------------------------------------------
GpsLink *s_link = nullptr;
GpsParser *s_parser = nullptr;
SdLog s_sd;
uint32_t s_lastLogMs = 0;

// ---- UI objects -------------------------------------------------------------------
lv_obj_t *s_home = nullptr;
lv_obj_t *s_dots[2] = {};
lv_obj_t *s_sbTime = nullptr, *s_sbGps = nullptr, *s_sbIcons = nullptr;

const Apps::Entry &app(int i) { return Apps::entry(i); }   // launcher registry (apps/Apps.cpp)
constexpr int STATUS_H = 36;
constexpr int TILE_W = 210, TILE_H = 210;       // portrait home: 2 columns x 3 rows

// ---- measurement ------------------------------------------------------------------
struct PhaseStats {
  uint32_t animFrames = 0;
  uint64_t animIntervalUsSum = 0;     // between consecutive animation frames
  uint32_t animIntervalUsMax = 0;
  uint64_t animRenderUsSum = 0;
  uint32_t animRenderUsMax = 0;
  uint32_t loopUsMax = 0;
};
PhaseStats s_phase;
uint32_t s_prevAnimFrameEnd = 0;

// ---- flicker probe (console "flash") -----------------------------------------------
bool s_probe = false;
uint32_t s_probeT0 = 0, s_flashBackAt = 0;
int s_probeFrame = 0;
uint32_t s_drawBottom = 0, s_drawHome = 0, s_drawApp = 0, s_drawTiles = 0;

void countDraw(lv_event_t *e) { (*(uint32_t *)lv_event_get_user_data(e))++; }

// % of pure-white pixels on a 40 x 24 grid of the live framebuffer (CPU view).
float whitePct() {
  const uint16_t *fb = LvglPort::shownFramebuffer();
  int white = 0, n = 0;
  for (int y = 10; y < Display::HEIGHT; y += 20)
    for (int x = 10; x < Display::WIDTH; x += 20, n++)
      if (fb[y * Display::WIDTH + x] == 0xFFFF) white++;
  return 100.0f * white / n;
}

void probeFrame() {
  Serial.printf("[FL] +%4ums frame %2d stale=%u white=%5.1f%% draws: bottom=%u home=%u app=%u tiles=%u anims=%u\n",
                (unsigned)(millis() - s_probeT0), ++s_probeFrame, (unsigned)LvglPort::lastStalePixels(), whitePct(),
                (unsigned)s_drawBottom, (unsigned)s_drawHome, (unsigned)s_drawApp, (unsigned)s_drawTiles,
                (unsigned)lv_anim_count_running());
  s_drawBottom = s_drawHome = s_drawApp = s_drawTiles = 0;
  if (millis() - s_probeT0 > 1800) { s_probe = false; LvglPort::setStaleCheck(false); Serial.println("[FL] probe end"); }
}

void onInvalidate(lv_event_t *e) {
  if (!s_probe) return;
  const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
  if (a) Serial.printf("[FL]   invalidate x%d..%d y%d..%d\n", (int)a->x1, (int)a->x2, (int)a->y1, (int)a->y2);
}

// Navigation timing (validate.ps1 / stress.ps1 parse "[UI] visible <name> <ms> ms"): from the
// request (tap, console) to the end of the first frame rendered with the new screen active.
uint32_t s_navT0 = 0;
const char *s_navTarget = nullptr;
lv_obj_t *s_navScreen = nullptr;

void onFrame(uint32_t startUs, uint32_t endUs) {
  if (s_navTarget && s_navScreen && lv_screen_active() == s_navScreen) {
    Serial.printf("[UI] visible %s %u ms\n", s_navTarget, (unsigned)(millis() - s_navT0));
    s_navTarget = nullptr;
  }
  if (s_probe) probeFrame();
  if (lv_anim_count_running() == 0) { s_prevAnimFrameEnd = 0; return; }
  s_phase.animFrames++;
  const uint32_t render = endUs - startUs;
  s_phase.animRenderUsSum += render;
  if (render > s_phase.animRenderUsMax) s_phase.animRenderUsMax = render;
  if (s_prevAnimFrameEnd) {
    const uint32_t iv = endUs - s_prevAnimFrameEnd;
    s_phase.animIntervalUsSum += iv;
    if (iv > s_phase.animIntervalUsMax) s_phase.animIntervalUsMax = iv;
  }
  s_prevAnimFrameEnd = endUs;
}

void printMem(const char *tag) {
  Serial.printf("[SHELL] %s mem: internal free=%u min=%u largest=%u | psram free=%u min=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

void printPhase(const char *name) {
  const PhaseStats &p = s_phase;
  const uint32_t intervals = p.animFrames > 1 ? p.animFrames - 1 : 0;
  const float fpsAvg = (intervals && p.animIntervalUsSum) ? intervals * 1e6f / (float)p.animIntervalUsSum : 0;
  const float fpsWorst = p.animIntervalUsMax ? 1e6f / p.animIntervalUsMax : 0;
  Serial.printf("[SHELL] RESULT %-10s mode=%s wifi=%s | anim frames=%u fps avg=%.1f worst=%.1f | "
                "render avg=%.1fms max=%.1fms | loop max=%.1fms | gps overflow=%u bad_crc=%u\n",
                name, LvglPort::modeName(), WifiService::stateName(WifiService::state()),
                (unsigned)p.animFrames, fpsAvg, fpsWorst,
                p.animFrames ? p.animRenderUsSum / 1000.0f / p.animFrames : 0.0f,
                p.animRenderUsMax / 1000.0f, p.loopUsMax / 1000.0f,
                (unsigned)s_link->stats().overflows, (unsigned)s_link->stats().badChecksum);
  const LvglPort::FrameStats f = LvglPort::takeFrameStats();
  Serial.printf("[SHELL]   all frames=%u: render avg=%.1fms, of which flush avg=%.1fms (rotate %.1fms, %u flush calls)\n",
                (unsigned)f.frames, f.frames ? f.renderUsSum / 1000.0f / f.frames : 0.0f,
                f.frames ? f.flushUsSum / 1000.0f / f.frames : 0.0f,
                f.frames ? f.rotateUsSum / 1000.0f / f.frames : 0.0f, (unsigned)f.flushCalls);
  printMem(name);
  s_phase = PhaseStats();
}

// ---- UI: home (transition model) ----------------------------------------------------------
//  - swipe/flick left/right anywhere on Home -> instant page switch (never counts as a tap)
//  - tap a tile -> tile zoom (Transitions::zoomOpen), then the app screen appears
//  - Back -> instant Home + pulse on the tile the app came from
constexpr int PAGES = 2;
lv_obj_t *s_pages[PAGES] = {};
lv_obj_t *s_tileBtn[Apps::COUNT] = {}, *s_tileIcon[Apps::COUNT] = {};
int s_page = 0;
int s_openedApp = -1;
bool s_pulseOn = true;                 // console "pulse 0/1": A/B the Back highlight

void setPageDots(int page) {
  for (int i = 0; i < PAGES; i++) lv_obj_set_style_bg_opa(s_dots[i], i == page ? LV_OPA_COVER : LV_OPA_40, 0);
}

void showPage(int page) {
  page = LV_CLAMP(0, page, PAGES - 1);
  if (page == s_page) return;
  for (int p = 0; p < PAGES; p++) lv_obj_set_hidden(s_pages[p], p != page);
  s_page = page;
  setPageDots(page);
}

void onHomeGesture(lv_event_t *) {
  if (Transitions::zoomRunning()) return;
  lv_indev_t *indev = lv_indev_active();
  const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT) showPage(s_page + 1);
  else if (dir == LV_DIR_RIGHT) showPage(s_page - 1);
  else return;
  lv_indev_wait_release(indev);        // LVGL would otherwise send CLICKED to the tile on release
}

void openApp(int index);

void onAppClicked(lv_event_t *e) {
  if (Transitions::zoomRunning()) return;
  openApp((int)(intptr_t)lv_event_get_user_data(e));
}

void addAppTile(lv_obj_t *page, int index, int col, int row) {
  const Apps::Entry &a = app(index);
  lv_obj_t *btn = lv_button_create(page);
  s_tileBtn[index] = btn;
  lv_obj_set_size(btn, TILE_W, TILE_H);
  lv_obj_set_pos(btn, 15 + col * 240, 18 + row * 234);
  lv_obj_set_style_radius(btn, HOME_TILE_STYLE >= 1 ? 28 : 0, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(a.color), 0);
  if (HOME_TILE_STYLE >= 2) {
    lv_obj_set_style_bg_grad_color(btn, lv_color_darken(lv_color_hex(a.color), LV_OPA_40), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
  }
  lv_obj_add_event_cb(btn, onAppClicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

  lv_obj_t *icon = lv_label_create(btn);
  s_tileIcon[index] = icon;
  lv_label_set_text(icon, a.icon);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -18);

  lv_obj_t *name = lv_label_create(btn);
  lv_label_set_text(name, a.name);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
  lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void buildHome() {
  s_home = lv_obj_create(nullptr);
  lv_obj_set_scrollable(s_home, false);            // no scroll engine: gestures only
  lv_obj_set_style_bg_color(s_home, lv_color_hex(0x1A2340), 0);
  if (HOME_BG_GRADIENT) {
    lv_obj_set_style_bg_grad_color(s_home, lv_color_hex(0x05070D), 0);
    lv_obj_set_style_bg_grad_dir(s_home, LV_GRAD_DIR_VER, 0);
  }
  lv_obj_add_event_cb(s_home, onHomeGesture, LV_EVENT_GESTURE, nullptr);

  for (int page = 0; page < PAGES; page++) {
    lv_obj_t *pg = lv_obj_create(s_home);
    lv_obj_remove_style_all(pg);
    lv_obj_set_size(pg, Display::UI_W, Display::UI_H - STATUS_H);
    lv_obj_set_pos(pg, 0, STATUS_H);
    lv_obj_set_scrollable(pg, false);
    lv_obj_set_hidden(pg, page != 0);
    s_pages[page] = pg;
    for (int i = 0; i < 6; i++) addAppTile(pg, page * 6 + i, i % 2, i / 2);
  }

  for (int i = 0; i < PAGES; i++) {
    s_dots[i] = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_dots[i]);
    lv_obj_set_size(s_dots[i], 12, 12);
    lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dots[i], lv_color_white(), 0);
    lv_obj_set_pos(s_dots[i], Display::UI_W / 2 - 16 + i * 22, Display::UI_H - 26);
  }
  setPageDots(0);
}

// Optional SD images (Assets): a tile icon replaces the built-in symbol, a wallpaper is drawn
// over the background colour. Without files nothing changes.
void applyAssets() {
  const char *names[Apps::COUNT];
  for (int i = 0; i < Apps::COUNT; i++) names[i] = app(i).name;
  Assets::begin(s_sd, names, Apps::COUNT);
  for (int i = 0; i < Apps::COUNT; i++) {
    const lv_image_dsc_t *img = Assets::icon(i);
    if (!img || !s_tileIcon[i]) continue;
    lv_obj_set_hidden(s_tileIcon[i], true);
    lv_obj_t *im = lv_image_create(s_tileBtn[i]);
    lv_image_set_src(im, img);
    lv_obj_align(im, LV_ALIGN_CENTER, 0, -18);
  }
  if (const lv_image_dsc_t *w = Assets::wallpaper()) lv_obj_set_style_bg_image_src(s_home, w, 0);
}

// ---- UI: status bar (system layer, stays above every screen) -----------------------------
void buildStatusBar() {
  lv_obj_t *bar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, Display::UI_W, STATUS_H);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);

  s_sbTime = lv_label_create(bar);
  lv_obj_set_style_text_color(s_sbTime, lv_color_white(), 0);
  lv_obj_align(s_sbTime, LV_ALIGN_LEFT_MID, 14, 0);
  lv_label_set_text(s_sbTime, "--:--");

  s_sbGps = lv_label_create(bar);
  lv_obj_set_style_text_color(s_sbGps, lv_color_white(), 0);
  lv_obj_align(s_sbGps, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(s_sbGps, "");

  s_sbIcons = lv_label_create(bar);
  lv_obj_set_style_text_color(s_sbIcons, lv_color_white(), 0);
  lv_obj_align(s_sbIcons, LV_ALIGN_RIGHT_MID, -14, 0);
  lv_label_set_text(s_sbIcons, "");
}

// Firmware update in progress (Ota): full-screen system overlay above every screen.
lv_obj_t *s_otaOverlay = nullptr, *s_otaBar = nullptr, *s_otaLabel = nullptr;

void updateOtaOverlay() {
  const Ota::State st = Ota::state();
  const bool show = st == Ota::State::Receiving || st == Ota::State::Done;
  if (!show) {
    if (s_otaOverlay) {                                      // update failed / aborted: normal brightness again
      lv_obj_delete(s_otaOverlay);
      s_otaOverlay = s_otaBar = s_otaLabel = nullptr;
      Backlight::forceOff(false);
    }
    return;
  }
  if (!s_otaOverlay) {
    // While flash is written the CPU cache is blocked and the display's refill interrupt with it: the
    // panel shows garbage. Ota shows this message for 1.5 s, then switches
    // the backlight off until the writes are over (Backlight::forceOff).
    s_otaOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_otaOverlay);
    lv_obj_set_size(s_otaOverlay, Display::UI_W, Display::UI_H);
    lv_obj_set_style_bg_color(s_otaOverlay, lv_color_hex(0x10141C), 0);
    lv_obj_set_style_bg_opa(s_otaOverlay, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_otaOverlay, true);               // no touch input while installing
    s_otaLabel = lv_label_create(s_otaOverlay);
    lv_obj_set_style_text_font(s_otaLabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_otaLabel, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_otaLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_otaLabel, LV_ALIGN_CENTER, 0, -50);
    s_otaBar = lv_bar_create(s_otaOverlay);
    lv_obj_set_size(s_otaBar, 600, 24);
    lv_obj_align(s_otaBar, LV_ALIGN_CENTER, 0, 20);
  }
  char t[96];
  if (st == Ota::State::Done) snprintf(t, sizeof(t), LV_SYMBOL_OK "  Update installed - restarting");
  else snprintf(t, sizeof(t), LV_SYMBOL_DOWNLOAD "  Installing update\nThe screen goes dark until NAV-1 restarts");
  Apps::setText(s_otaLabel, t);
  lv_bar_set_value(s_otaBar, Ota::progress(), LV_ANIM_OFF);
}

void updateStatusBar() {
  updateOtaOverlay();
  static char lastTime[16], lastGps[40], lastIcons[40];
  char buf[40];
  const GpsData d = s_parser->snapshot(s_link->linkUp(GPS_LINK_TIMEOUT_MS));

  struct tm lt;
  if (TimeService::local(lt)) snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
  else snprintf(buf, sizeof(buf), "--:--");
  if (strcmp(buf, lastTime)) { strcpy(lastTime, buf); lv_label_set_text(s_sbTime, buf); }

  if (!d.linkUp) snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " no GPS");
  else snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %s  %u/%u sat", d.fix ? "FIX" : "no fix",
                (unsigned)d.satsUsed, (unsigned)d.satsInView());
  if (strcmp(buf, lastGps)) { strcpy(lastGps, buf); lv_label_set_text(s_sbGps, buf); }

  snprintf(buf, sizeof(buf), "%s%s  %s%s  %s", TripRecorder::recording() ? "REC  " : "", BleService::advertising() || BleService::connectedCount() ? LV_SYMBOL_BLUETOOTH : "",
           WifiService::hotspotOn() ? "AP " : "", WifiService::state() == WifiService::State::Connected ? LV_SYMBOL_WIFI : "",
           s_sd.logging() ? LV_SYMBOL_SD_CARD : "");
  if (strcmp(buf, lastIcons)) { strcpy(lastIcons, buf); lv_label_set_text(s_sbIcons, buf); }
}

// ---- UI: placeholder app ------------------------------------------------------------------
lv_color_t appBg(int index) { return lv_color_darken(lv_color_hex(app(index).color), LV_OPA_70); }

const AppImpl *s_activeApp = nullptr;      // app with a real implementation that is open

void goHome() {
  lv_obj_t *app = lv_screen_active();
  if (app == s_home) return;
  if (Diag::busy()) return;                 // a self-test is using the app screen
  if (s_activeApp) { s_activeApp->destroy(); s_activeApp = nullptr; }
  // Instant. The app screen is deleted after this event has finished: goHome() runs inside
  // the click handler of that screen's own Back button.
  s_navT0 = millis();
  s_navTarget = "Home";
  s_navScreen = s_home;
  lv_screen_load(s_home);
  lv_obj_delete_async(app);
  if (s_openedApp >= 0 && s_pulseOn) Transitions::pulse(s_tileBtn[s_openedApp], lv_color_white(), 350);
}

void onBack(lv_event_t *) { goHome(); }

void showAppScreen(void *user) {
  const int index = (int)(intptr_t)user;
  const Apps::Entry &a = app(index);
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, appBg(index), 0);          // same colour as the zoom card

  lv_obj_t *back = lv_button_create(scr);
  lv_obj_set_pos(back, 16, STATUS_H + 12);
  lv_obj_set_size(back, 150, 52);
  lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT " Home");
  lv_obj_center(bl);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text_fmt(title, "%s  %s", a.icon, a.name);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 186, STATUS_H + 22);

  s_activeApp = a.impl;
  if (s_activeApp) {
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, Display::UI_H - Apps::CONTENT_H);
    lv_obj_set_size(content, Apps::CONTENT_W, Apps::CONTENT_H);
    lv_obj_set_scrollable(content, false);
    s_activeApp->create(content);
    s_activeApp->update();
  } else {
    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_text(body, "Coming soon");
    lv_obj_set_style_text_color(body, lv_color_hex(0xDDDDDD), 0);
    lv_obj_center(body);
  }

  lv_obj_add_event_cb(scr, countDraw, LV_EVENT_DRAW_MAIN_BEGIN, &s_drawApp);
  s_openedApp = index;
  s_navScreen = scr;
  lv_screen_load(scr);                                        // instant: the card already fills the screen
}

void openApp(int index) {
  if (lv_screen_active() != s_home) return;
  s_navT0 = millis();
  s_navTarget = app(index).name;
  s_navScreen = nullptr;                                    // set when the app screen exists
  if (index / 6 != s_page) showPage(index / 6);             // benchmark/console may open off-page apps
  lv_area_t from;
  lv_obj_get_coords(s_tileBtn[index], &from);
  Transitions::zoomOpen(s_home, from, appBg(index), 150, showAppScreen, (void *)(intptr_t)index);
}
int s_switchTo = -1;

void doSwitch(void *) {
  const int index = s_switchTo;
  s_switchTo = -1;
  lv_obj_t *old = lv_screen_active();
  if (index < 0 || old == s_home) return;
  if (s_activeApp) { s_activeApp->destroy(); s_activeApp = nullptr; }
  showAppScreen((void *)(intptr_t)index);
  lv_obj_delete_async(old);
}

// ---- automatic benchmark (console "bench"; runs in the current Wi-Fi/BLE state) ---------------
enum Phase { SETTLE, SWIPE, APP_OPEN, MANUAL };
Phase s_phaseId = MANUAL;
uint32_t s_phaseT0 = 0, s_nextActionMs = 0;
int s_actions = 0;

void enterPhase(Phase p) {
  s_phaseId = p;
  s_phaseT0 = millis();
  s_nextActionMs = millis() + 500;
  s_actions = 0;
  s_phase = PhaseStats();
  LvglPort::takeFrameStats();
}

void runBenchmark() {
  const uint32_t now = millis();
  switch (s_phaseId) {
    case SETTLE:
      if (now - s_phaseT0 > 3000) { Serial.println("[SHELL] benchmark: swipe x10"); enterPhase(SWIPE); }
      break;
    case SWIPE:
      if (now >= s_nextActionMs) {
        if (s_actions == 10) {
          printPhase("swipe");
          Serial.println("[SHELL] benchmark: app open/close x3");
          enterPhase(APP_OPEN);
          break;
        }
        showPage((s_actions + 1) % 2);
        s_actions++;
        s_nextActionMs = now + 900;
      }
      break;
    case APP_OPEN:
      if (now >= s_nextActionMs) {
        if (s_actions == 6) {
          printPhase("app-anim");
          Serial.println("[SHELL] benchmark done");
          enterPhase(MANUAL);
          break;
        }
        if (s_actions % 2 == 0) openApp(0);
        else goHome();
        s_actions++;
        s_nextActionMs = now + 900;
      }
      break;
    case MANUAL:
      break;
  }
}

// ---- periodic report ("report on") + console ---------------------------------------------------
void reportPeriodic() {
  const uint32_t lat = LvglPort::takeTouchLatencyUs();
  if (!Diag::reportsOn()) return;
  if (lat) Serial.printf("[SHELL] touch: press -> first frame %.1f ms\n", lat / 1000.0f);
  static uint32_t last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  Diag::print(Serial, "display");
  Diag::print(Serial, "touch");
  Diag::print(Serial, "mem");
}

void handleLine(const char *cmd) {
  if (Diag::command(cmd, Serial)) return;
  if (strcmp(cmd, "flash") == 0) {
    Serial.printf("[FL] probe start, white now=%.1f%%: open app, Home after 800 ms\n", whitePct());
    s_probe = true; s_probeT0 = millis(); s_probeFrame = 0; LvglPort::setStaleCheck(true);
    openApp(0);
    s_flashBackAt = millis() + 800;
  } else if (strcmp(cmd, "swipe") == 0) {
    Serial.printf("[FL] probe start: page switch to %d\n", 1 - s_page);
    s_probe = true; s_probeT0 = millis(); s_probeFrame = 0; LvglPort::setStaleCheck(true);
    showPage(1 - s_page);
  } else if (strncmp(cmd, "map ", 4) == 0 && strncmp(cmd + 4, "trip ", 5) == 0) {       // map trip <base>: Trips > Show on map
    mapAppShowTrip(cmd + 9);
    if (lv_screen_active() == s_home) openApp(Apps::indexOf("Map")); else Shell::switchApp("Map");
  } else if (strncmp(cmd, "map ", 4) == 0) {
    mapAppCommand(cmd + 4);
  } else if (strcmp(cmd, "map") == 0) {
    const MapRender::Status m = MapRender::status();
    const MapTiles::Stats t = MapTiles::stats();
    Serial.printf("[MAP] %s, last render %u ms (%u tiles), frames %u, task stack unused %u B, error '%s'; tiles read %u (avg %u ms), dir reads %u, %u KB from the card\n",
                  m.mapOk ? (m.busy ? "rendering" : "idle") : "closed", (unsigned)m.lastMs, (unsigned)m.lastTiles, (unsigned)m.frames,
                  (unsigned)m.stackFree, m.error, (unsigned)t.tiles, t.tiles ? (unsigned)(t.tileMsSum / t.tiles) : 0, (unsigned)t.dirReads,
                  (unsigned)(t.bytesRead >> 10));
  } else if (strcmp(cmd, "races") == 0) {
    Serial.printf("[FL] vsync races since boot: %u (handled)\n", (unsigned)Health::totals().vsyncRaces);
  } else if (strncmp(cmd, "pulse ", 6) == 0) {
    s_pulseOn = cmd[6] == '1';
    Serial.printf("[FL] Back pulse %s\n", s_pulseOn ? "ON" : "OFF");
  } else if (strncmp(cmd, "open ", 5) == 0) {
    const int i = Apps::indexOf(cmd + 5);
    if (i < 0) { Serial.printf("[UI] no app '%s'\n", cmd + 5); return; }
    if (lv_screen_active() != s_home) goHome();
    openApp(i);
    Serial.printf("[UI] opened %s\n", app(i).name);
  } else if (strncmp(cmd, "page ", 5) == 0) {        // "page <App> <n>": open an app on a menu page
    char name[16] = "";
    int page = 0;
    sscanf(cmd + 5, "%15s %d", name, &page);
    void (*setPage)(int) = !strcasecmp(name, "Settings") ? settingsAppSetPage : !strcasecmp(name, "Tools") ? toolsAppSetPage
                         : !strcasecmp(name, "GPS") ? gpsAppSetPage : !strcasecmp(name, "Trips") ? tripsAppSetPage : nullptr;
    const int i = Apps::indexOf(name);
    if (!setPage || i < 0) { Serial.printf("[UI] no paged app '%s'\n", name); return; }
    setPage(page);
    if (lv_screen_active() != s_home) goHome();
    openApp(i);
    Serial.printf("[UI] opened %s page %d\n", app(i).name, page);
  } else if (strncmp(cmd, "files ", 6) == 0) {       // open the Files app in a folder
    filesAppSetFolder(cmd + 6);
    const int i = Apps::indexOf("Files");
    if (i < 0) return;
    if (lv_screen_active() != s_home) goHome();
    openApp(i);
    Serial.printf("[UI] opened Files at %s\n", cmd + 6);
  } else if (strcmp(cmd, "home") == 0) {
    goHome();
    Serial.println("[UI] home");
  } else if (strcmp(cmd, "bench") == 0) {
    lv_screen_load(s_home);
    Serial.println("[SHELL] benchmark restarting");
    enterPhase(SETTLE);
  } else if (cmd[0]) {
    Serial.printf("[CONSOLE] unknown command '%s' (try: help)\n", cmd);
  }
}

void handleConsole() {
  static char cmd[128];
  static size_t len = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (len < sizeof(cmd) - 1) cmd[len++] = c; continue; }
    cmd[len] = '\0';
    len = 0;
    handleLine(cmd);
  }
}

// Services that must keep running whenever the loop is busy (also used by Diag while a
// self-test waits).
// Times one part of the non-LVGL loop for Health (the slowest part names a loop stall).
#define TIMED(name, call) do { const uint32_t t_ = micros(); call; Health::notePart(name, micros() - t_); } while (0)

void updateServices() {
  if (millis() - s_lastLogMs >= FIELD_LOG_MS && s_sd.logging()) {
    s_lastLogMs = millis();
    char row[256];
    const GpsData d = s_parser->snapshot(s_link->linkUp(GPS_LINK_TIMEOUT_MS));
    s_sd.line(GpsCsv::row(row, sizeof(row), millis(), d, s_link->stats()));
  }
  TIMED("sd", s_sd.update(SD_FLUSH_MS));
  TIMED("settings", Settings::update());
  TIMED("gpscfg", GpsConfig::update());
  TIMED("wifi", WifiService::update());
  TIMED("time", TimeService::update());
  TIMED("fieldtest", FieldRecorder::update());
  TIMED("replay", GpsReplay::update());
  TIMED("trip", TripRecorder::update());
  TIMED("web", WebService::update());
  TIMED("ota", Ota::update());
  TIMED("ble", BleService::update());
  TIMED("backlight", Backlight::update());
}

void diagPump() {
  s_link->update();
  LvglPort::update();
  updateServices();
}

}  // namespace

// ---- public ---------------------------------------------------------------------------------------

static void heapCheck(const char *where) {
  const bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true);
  Serial.printf("[SHELL] heap integrity after %-12s: %s\n", where, ok ? "OK" : "CORRUPT");
}

void Shell::begin(GpsLink &link, GpsParser &parser, bool parserSelfTestOk) {
  s_link = &link;
  s_parser = &parser;
  uint32_t ph[7];                          // boot phase ends (ms), printed before the ready line
  ph[0] = millis();
  printMem("boot");
  heapCheck("boot");

  if (!LvglPort::begin(LVGL_RENDER_MODE)) {
    Serial.println("[SHELL] LvglPort::begin FAILED");
    return;
  }
  Serial.printf("[SHELL] LVGL %d.%d.%d, render %s, GT911 %s\n", lv_version_major(), lv_version_minor(),
                lv_version_patch(), LvglPort::modeName(),
                LvglPort::touchPresent() ? "found at 0x5D" : "NOT FOUND");
  LvglPort::setFrameListener(onFrame);
  heapCheck("lvglport");
  ph[1] = millis();

  lv_theme_default_init(LvglPort::display(), lv_color_hex(0x1E88E5), lv_color_hex(0xFB8C00), true,
                        &lv_font_montserrat_20);
  heapCheck("theme");
  buildHome();
  lv_obj_add_event_cb(s_home, countDraw, LV_EVENT_DRAW_MAIN_BEGIN, &s_drawHome);
  lv_obj_add_event_cb(s_pages[0], countDraw, LV_EVENT_DRAW_MAIN_BEGIN, &s_drawTiles);
  lv_obj_add_event_cb(lv_layer_bottom(), countDraw, LV_EVENT_DRAW_MAIN_BEGIN, &s_drawBottom);
  heapCheck("home");
  buildStatusBar();
  lv_screen_load(s_home);
  lv_display_add_event_cb(LvglPort::display(), onInvalidate, LV_EVENT_INVALIDATE_AREA, nullptr);
  heapCheck("statusbar");
  ph[2] = millis();

  const SdLog::Pins pins = { SD_CS_PIN, SD_MOSI_PIN, SD_SCK_PIN, SD_MISO_PIN };
  if (s_sd.begin(pins, SD_SPI_HZ) && s_sd.openSession(SD_LOG_DIR)) {
    link.setRawEcho(&s_sd.nmeaSink());
    s_sd.line("# NAV-1 " FW_VERSION " session (" FW_GIT_DESCRIBE ")");
    s_sd.line(GpsCsv::header());
    const bool writer = s_sd.startWriter(SD_FLUSH_MS, SD_WRITER_CORE, SD_WRITER_PRIO);   // card I/O off the UI loop
    Serial.printf("[SHELL] SD logging to %s/%s (mount attempts %d, wake-up %s, R1=0x%02X), writer task %s\n", SD_LOG_DIR,
                  s_sd.sessionName(), s_sd.mountAttempts(), s_sd.wakeUpUsed() ? "used" : "no", s_sd.wakeUpR1(),
                  writer ? "ON" : "FAILED (writes in loop)");
  } else {
    Serial.printf("[SHELL] SD NOT logging: mounted=%d attempts=%d wake_up=%s (last CMD0 R1=0x%02X) errors=%u\n", s_sd.mounted(),
                  s_sd.mountAttempts(), s_sd.wakeUpUsed() ? "used" : "no", s_sd.wakeUpR1(), (unsigned)s_sd.writeErrors());
  }
  applyAssets();
  heapCheck("sd");
  ph[3] = millis();

  Location::begin(link, parser);
  Storage::begin(s_sd);
  Settings::begin();
  Settings::attachSd(s_sd);               // /system/settings.json edited on a PC wins
  GpsConfig::begin(link);                 // GPS module profile (checked ~3 s after start, then after module reboots)
  Backlight::begin();                     // brightness + dim-when-idle from Settings
  LvglPort::setTouchGate(Backlight::touchGate);
  TimeService::begin();
  printMem("before-radio");
  WifiService::begin();
  printMem("after-wifi");
  ph[4] = millis();
  BleService::begin(link, parser);
  printMem("after-ble");
  heapCheck("radio");
  ph[5] = millis();
  Diag::begin(link, parser, s_sd, parserSelfTestOk, diagPump);
  Diag::setStatusIconsLabel(s_sbIcons);   // self-test: the Wi-Fi icon must be on screen when connected
  FieldRecorder::begin(s_sd);             // resumes a field test interrupted by a reset
  GpsReplay::begin(s_sd, link, parser);
  TripRecorder::begin(s_sd);              // resumes a trip interrupted by a reset
  WebService::begin(s_sd);                // starts serving once Wi-Fi is connected
  Ota::begin();                           // new firmware from an update: verify, else rollback
  ph[6] = millis();
  Serial.printf("[SHELL] boot phases (ms): before shell %u, LVGL+touch %u, UI %u, SD %u, Wi-Fi %u, BLE %u, recorders %u\n",
                (unsigned)ph[0], (unsigned)(ph[1] - ph[0]), (unsigned)(ph[2] - ph[1]), (unsigned)(ph[3] - ph[2]),
                (unsigned)(ph[4] - ph[3]), (unsigned)(ph[5] - ph[4]), (unsigned)(ph[6] - ph[5]));
  Diag::setBootMs(millis());
  Serial.printf("[SHELL] ready in %u ms: Wi-Fi %s%s, BLE %s '%s'. Serial: help\n", (unsigned)millis(),
                WifiService::stateName(WifiService::state()),
                WifiService::hasSavedNetwork() ? (" -> '" + WifiService::ssid() + "'").c_str() : "",
                BleService::enabled() ? "advertising as" : "off", BleService::name().c_str());
}

void Shell::switchApp(const char *name) {
  const int i = Apps::indexOf(name);
  if (i < 0) return;
  s_switchTo = i;
  lv_async_call(doSwitch, nullptr);
}

void Shell::update() {
  const uint32_t t0 = micros();
  LvglPort::update();
  const uint32_t tLvgl = micros();

  static uint32_t lastSb = 0, lastApp = 0;
  if (millis() - lastSb >= 500) { lastSb = millis(); TIMED("status bar", updateStatusBar()); }
  if (s_activeApp && s_activeApp->update && millis() - lastApp >= Apps::UPDATE_MS) {
    lastApp = millis();
    TIMED(s_activeApp->name, s_activeApp->update());
  }
  updateServices();
  runBenchmark();
  TIMED("report", reportPeriodic());
  TIMED("console", handleConsole());
  if (s_flashBackAt && millis() >= s_flashBackAt) {
    s_flashBackAt = 0;
    Serial.printf("[FL] +%ums -> Home\n", (unsigned)(millis() - s_probeT0));
    goHome();
  }

  const uint32_t now = micros();
  Health::noteLoop(tLvgl - t0, now - tLvgl);
  Health::tick();
  if (now - t0 > s_phase.loopUsMax) s_phase.loopUsMax = now - t0;
}
