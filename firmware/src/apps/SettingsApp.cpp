// =============================================================================
//  Settings app: a menu on the left, one page per topic on the right, in the
//  order people know from a phone:
//    Wi-Fi         on/off, status, scan, connect, forget
//    Bluetooth     on/off, visibility
//    Hotspot       NAV-1's own Wi-Fi for a direct phone link: on/off, name, password
//    Display       brightness, dim when idle
//    Date & Time   time zone, current local time
//    About         name, firmware, storage
//  Everything goes through the services (WifiService, BleService, TimeService,
//  Backlight). The last page shown is remembered while the device runs.
// =============================================================================
#include "../Display.h"
#include "App.h"

#include <Arduino.h>
#include "../services/WifiService.h"
#include "../services/BleService.h"
#include "../services/Settings.h"
#include "../services/Storage.h"
#include "../services/TimeService.h"
#include "../services/Backlight.h"
#include "../../config.h"
#include "../../version.h"

namespace {

enum Page { P_WIFI, P_BLE, P_HOTSPOT, P_DISPLAY, P_TIME, P_ABOUT, P_COUNT };
const char *const PAGE_NAME[P_COUNT] = { LV_SYMBOL_WIFI "  Wi-Fi", LV_SYMBOL_BLUETOOTH "  Bluetooth",
                                         LV_SYMBOL_SHUFFLE "  Hotspot", LV_SYMBOL_EYE_OPEN "  Display",
                                         LV_SYMBOL_BELL "  Date & Time", LV_SYMBOL_SETTINGS "  About" };
using Apps::PAGE_W;

struct Ui {
  lv_obj_t *menu[P_COUNT] = {}, *page[P_COUNT] = {};
  lv_obj_t *wifiSw = nullptr, *wifiStatus = nullptr, *scanBtn = nullptr, *scanLbl = nullptr, *forgetBtn = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *bleSw = nullptr, *bleStatus = nullptr, *apSw = nullptr, *apStatus = nullptr;
  lv_obj_t *brightVal = nullptr, *timeNow = nullptr, *about = nullptr;
  lv_obj_t *overlay = nullptr, *passTa = nullptr;
  uint32_t listSerial = UINT32_MAX;
};
Ui s_ui;
int s_page = P_WIFI;
char s_selSsid[33];
const char *signalWord(int rssi);        // "strong" .. "weak" (users need a word, not dBm)

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, const char *text, lv_color_t color = lv_color_white()) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, lv_event_cb_t cb, lv_obj_t **lbl = nullptr) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, 44);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  if (lbl) *lbl = l;
  return b;
}

// Page title with an optional switch on the right.
lv_obj_t *pageTitle(lv_obj_t *page, const char *text, lv_obj_t **sw = nullptr, lv_event_cb_t cb = nullptr) {
  lv_obj_t *t = Apps::pageTitle(page, text);
  if (sw) {
    *sw = lv_switch_create(page);
    lv_obj_align(*sw, LV_ALIGN_TOP_RIGHT, -4, 6);
    lv_obj_add_event_cb(*sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  }
  return t;
}

// ---- password dialog -------------------------------------------------------------------
void closeOverlay() {
  if (s_ui.overlay) { lv_obj_delete_async(s_ui.overlay); s_ui.overlay = nullptr; s_ui.passTa = nullptr; }
}

void onKeyboard(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) WifiService::connect(s_selSsid, lv_textarea_get_text(s_ui.passTa));
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) closeOverlay();
}

void openPasswordDialog(lv_obj_t *anyChild) {
  lv_obj_t *scr = lv_obj_get_screen(anyChild);
  lv_obj_t *ov = lv_obj_create(scr);
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, Display::UI_W, Display::UI_H);
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x10141C), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);           // opaque: cheap to draw
  lv_obj_set_clickable(ov, true);              // swallows taps
  lv_obj_t *title = label(ov, &lv_font_montserrat_20, "");
  lv_label_set_text_fmt(title, "Password for '%s'   (" LV_SYMBOL_OK " connect, " LV_SYMBOL_KEYBOARD " cancel)", s_selSsid);
  lv_obj_set_pos(title, 24, 48);
  lv_obj_t *ta = lv_textarea_create(ov);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, 63);
  lv_obj_set_size(ta, 752, 56);
  lv_obj_set_pos(ta, 24, 84);
  lv_obj_t *kb = lv_keyboard_create(ov);
  lv_obj_set_size(kb, Display::UI_W, 320);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_add_event_cb(kb, onKeyboard, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, onKeyboard, LV_EVENT_CANCEL, nullptr);
  s_ui.overlay = ov;
  s_ui.passTa = ta;
}

// ---- events -----------------------------------------------------------------------------
void showPage(int p) {
  s_page = constrain(p, 0, P_COUNT - 1);
  Apps::showMenuPage(s_ui.menu, s_ui.page, P_COUNT, s_page);
}
void onMenu(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }

void onWifiSwitch(lv_event_t *e) {
  WifiService::setEnabled(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
}
void onBleSwitch(lv_event_t *e) {
  BleService::setEnabled(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
}
void onHotspotSwitch(lv_event_t *e) {
  WifiService::setHotspot(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
}
void onScan(lv_event_t *) { WifiService::startScan(); }
// Forget asks first: a single tap on the big button used to drop the saved network (2026-09-29).
void onForgetCancel(lv_event_t *) { closeOverlay(); }
void onForgetConfirmed(lv_event_t *) {
  closeOverlay();
  WifiService::forget();
}
void onForget(lv_event_t *e) {
  lv_obj_t *ov = lv_obj_create(lv_obj_get_screen((lv_obj_t *)lv_event_get_target(e)));
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, Display::UI_W, Display::UI_H);
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x10141C), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
  lv_obj_set_clickable(ov, true);
  lv_obj_t *t = label(ov, &lv_font_montserrat_28, "");
  lv_label_set_text_fmt(t, "Forget '%s'?", WifiService::ssid().c_str());
  lv_obj_set_pos(t, 24, 52);
  lv_obj_t *d = label(ov, &lv_font_montserrat_20, "NAV-1 disconnects and will not reconnect by itself.\n"
                                                   "To use this network again, scan and enter its password.",
                      lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(d, 24, 104);
  button(ov, "Cancel", 24, 220, 240, onForgetCancel);
  lv_obj_t *ok = button(ov, LV_SYMBOL_TRASH "  Forget", 536, 220, 240, onForgetConfirmed);
  lv_obj_set_style_bg_color(ok, lv_color_hex(0xC62828), 0);
  s_ui.overlay = ov;
}
void onZone(lv_event_t *e) { TimeService::setZone(lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e))); }

const uint32_t DIM_S[] = { 0, 30, 60, 120, 300 };
const char *DIM_OPTIONS = "Never\nAfter 30 seconds\nAfter 1 minute\nAfter 2 minutes\nAfter 5 minutes";

void onBrightness(lv_event_t *e) {       // live while dragging, saved on release
  lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
  Backlight::setBrightness(lv_slider_get_value(s), lv_event_get_code(e) == LV_EVENT_RELEASED);
  lv_label_set_text_fmt(s_ui.brightVal, "%d %%", Backlight::brightness());
}
void onDim(lv_event_t *e) { Backlight::setDimAfter(DIM_S[lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e))]); }

void onNetwork(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i >= WifiService::networkCount()) return;
  const WifiService::Network &n = WifiService::network(i);
  strlcpy(s_selSsid, n.ssid, sizeof(s_selSsid));
  if (n.open) WifiService::connect(s_selSsid, "");
  else openPasswordDialog((lv_obj_t *)lv_event_get_target(e));
}

void rebuildList() {
  lv_obj_clean(s_ui.list);
  const int n = WifiService::networkCount();
  if (!n) {
    label(s_ui.list, &lv_font_montserrat_20, WifiService::scanSerial() ? "No networks found" : "Tap Scan to search");
    return;
  }
  for (int i = 0; i < n; i++) {
    const WifiService::Network &net = WifiService::network(i);
    char text[80];
    snprintf(text, sizeof(text), LV_SYMBOL_WIFI "  %s   %s%s", net.ssid, signalWord(net.rssi), net.open ? "   (open)" : "");
    lv_obj_t *b = lv_button_create(s_ui.list);
    lv_obj_set_size(b, lv_pct(100), 48);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(b, onNetwork, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}

// ---- pages ------------------------------------------------------------------------------
void buildWifi(lv_obj_t *p) {
  pageTitle(p, LV_SYMBOL_WIFI "  Wi-Fi", &s_ui.wifiSw, onWifiSwitch);
  s_ui.wifiStatus = label(p, &lv_font_montserrat_20, "", lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.wifiStatus, 4, 52);
  lv_obj_set_width(s_ui.wifiStatus, PAGE_W - 30);
  s_ui.scanBtn = button(p, "Scan", 4, 110, 150, onScan, &s_ui.scanLbl);
  s_ui.forgetBtn = button(p, "Forget network", 170, 110, 210, onForget);
  s_ui.list = lv_obj_create(p);                            // scrollable flex column of network buttons
  lv_obj_remove_style_all(s_ui.list);
  lv_obj_set_pos(s_ui.list, 0, 166);
  lv_obj_set_size(s_ui.list, PAGE_W - 20, 178);
  lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ui.list, 6, 0);
  lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
}

void buildBle(lv_obj_t *p) {
  pageTitle(p, LV_SYMBOL_BLUETOOTH "  Bluetooth", &s_ui.bleSw, onBleSwitch);
  s_ui.bleStatus = label(p, &lv_font_montserrat_20, "", lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.bleStatus, 4, 52);
  lv_obj_set_width(s_ui.bleStatus, PAGE_W - 30);
}

void buildHotspot(lv_obj_t *p) {
  pageTitle(p, LV_SYMBOL_SHUFFLE "  Hotspot", &s_ui.apSw, onHotspotSwitch);
  s_ui.apStatus = label(p, &lv_font_montserrat_20, "", lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.apStatus, 4, 52);
  lv_obj_set_width(s_ui.apStatus, PAGE_W - 30);
}

void buildDisplay(lv_obj_t *p) {
  pageTitle(p, LV_SYMBOL_EYE_OPEN "  Display");
  lv_obj_t *bl = label(p, &lv_font_montserrat_20, "Brightness");
  lv_obj_set_pos(bl, 4, 60);
  s_ui.brightVal = label(p, &lv_font_montserrat_20, "", lv_color_hex(0xDDDDDD));
  lv_obj_align(s_ui.brightVal, LV_ALIGN_TOP_RIGHT, -8, 60);
  lv_label_set_text_fmt(s_ui.brightVal, "%d %%", Backlight::brightness());
  lv_obj_t *sl = lv_slider_create(p);
  lv_slider_set_range(sl, 10, 100);
  lv_slider_set_value(sl, Backlight::brightness(), LV_ANIM_OFF);
  lv_obj_set_size(sl, PAGE_W - 60, 18);
  lv_obj_set_pos(sl, 14, 104);
  lv_obj_add_event_cb(sl, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sl, onBrightness, LV_EVENT_RELEASED, nullptr);

  lv_obj_t *dl = label(p, &lv_font_montserrat_20, "Dim the screen when not touched");
  lv_obj_set_pos(dl, 4, 160);
  lv_obj_t *dim = lv_dropdown_create(p);
  lv_dropdown_set_options(dim, DIM_OPTIONS);
  int sel = 0;
  for (int i = 0; i < 5; i++) if (DIM_S[i] == Backlight::dimAfter()) sel = i;
  lv_dropdown_set_selected(dim, sel);
  lv_obj_set_size(dim, 280, 44);
  lv_obj_set_pos(dim, 4, 196);
  lv_obj_add_event_cb(dim, onDim, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *hint = label(p, &lv_font_montserrat_14, "A tap wakes the screen without pressing anything.", lv_color_hex(0xBBBBBB));
  lv_obj_set_pos(hint, 4, 252);
}

void buildTime(lv_obj_t *p) {
  pageTitle(p, LV_SYMBOL_BELL "  Date & Time");
  lv_obj_t *zl = label(p, &lv_font_montserrat_20, "Time zone");
  lv_obj_set_pos(zl, 4, 60);
  String opts;
  for (int i = 0; i < TimeService::zoneCount(); i++) { if (i) opts += '\n'; opts += TimeService::zoneName(i); }
  lv_obj_t *tz = lv_dropdown_create(p);
  lv_dropdown_set_options(tz, opts.c_str());
  lv_dropdown_set_selected(tz, TimeService::zone());
  lv_obj_set_size(tz, 280, 44);
  lv_obj_set_pos(tz, 4, 96);
  lv_obj_add_event_cb(tz, onZone, LV_EVENT_VALUE_CHANGED, nullptr);
  s_ui.timeNow = label(p, &lv_font_montserrat_20, "", lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.timeNow, 4, 164);
  lv_obj_set_width(s_ui.timeNow, PAGE_W - 30);
}

void buildAbout(lv_obj_t *p) {
  pageTitle(p, LV_SYMBOL_SETTINGS "  About");
  s_ui.about = label(p, &lv_font_montserrat_20, "", lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.about, 4, 60);
  lv_obj_set_width(s_ui.about, PAGE_W - 30);
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  Apps::buildMenu(content, PAGE_NAME, P_COUNT, s_ui.menu, s_ui.page, onMenu);
  void (*const build[P_COUNT])(lv_obj_t *) = { buildWifi, buildBle, buildHotspot, buildDisplay, buildTime, buildAbout };
  for (int i = 0; i < P_COUNT; i++) build[i](s_ui.page[i]);
  showPage(s_page);
}

const char *signalWord(int rssi) {
  if (rssi >= -60) return "strong";
  if (rssi >= -70) return "good";
  if (rssi >= -80) return "fair";
  return "weak";
}

void update() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  if (s_ui.overlay) return;                      // keyboard open: nothing behind it needs updating

  const bool on = WifiService::enabled();
  if (lv_obj_has_state(s_ui.wifiSw, LV_STATE_CHECKED) != on) lv_obj_set_state(s_ui.wifiSw, LV_STATE_CHECKED, on);
  char buf[200];
  const WifiService::State st = WifiService::state();
  if (st == WifiService::State::Connected)
    snprintf(buf, sizeof(buf), "Connected to '%s'\nSignal %s   IP %s", WifiService::ssid().c_str(),
             signalWord(WifiService::rssi()), WifiService::ip().c_str());
  else if (st == WifiService::State::Connecting)
    snprintf(buf, sizeof(buf), "Connecting to '%s'...", WifiService::ssid().c_str());
  else if (st == WifiService::State::Failed && WifiService::ssid().length())
    snprintf(buf, sizeof(buf), "Not connected to '%s'\n%s - retrying", WifiService::ssid().c_str(),
             WifiService::lastError().c_str());
  else if (WifiService::lastError().length())            // a new network failed, nothing saved
    snprintf(buf, sizeof(buf), "Could not connect: %s\nScan and tap the network to try again", WifiService::lastError().c_str());
  else snprintf(buf, sizeof(buf), "%s", on ? "No network saved - scan and tap a network" : "Wi-Fi is off");
  Apps::setText(s_ui.wifiStatus, buf);
  Apps::setText(s_ui.scanLbl, WifiService::scanning() ? "Scanning..." : "Scan");
  lv_obj_set_state(s_ui.scanBtn, LV_STATE_DISABLED, !on);
  lv_obj_set_hidden(s_ui.forgetBtn, !WifiService::hasSavedNetwork());
  if (WifiService::scanSerial() != s_ui.listSerial) {
    s_ui.listSerial = WifiService::scanSerial();
    rebuildList();
  }

  const bool ble = BleService::enabled();
  if (lv_obj_has_state(s_ui.bleSw, LV_STATE_CHECKED) != ble) lv_obj_set_state(s_ui.bleSw, LV_STATE_CHECKED, ble);
  if (!ble && BleService::refusal().length()) snprintf(buf, sizeof(buf), "%s", BleService::refusal().c_str());
  else if (!ble) snprintf(buf, sizeof(buf), "Bluetooth is off");
  else if (BleService::connectedCount()) snprintf(buf, sizeof(buf), "A phone is connected.\nVisible as %s", BleService::name().c_str());
  else snprintf(buf, sizeof(buf), "Visible to phones as\n%s", BleService::name().c_str());
  Apps::setText(s_ui.bleStatus, buf);

  const bool ap = Settings::hotspotEnabled();
  if (lv_obj_has_state(s_ui.apSw, LV_STATE_CHECKED) != ap) lv_obj_set_state(s_ui.apSw, LV_STATE_CHECKED, ap);
  if (WifiService::hotspotOn()) {
    const int n = WifiService::hotspotClients();
    snprintf(buf, sizeof(buf), "Network  %s\nPassword  %s\n%s\n\nThe Phone app shows a QR code to connect.",
             WifiService::hotspotSsid().c_str(), WifiService::hotspotPass().c_str(),
             n ? (n == 1 ? "1 phone connected" : "Phones connected") : "No phone connected");
  } else snprintf(buf, sizeof(buf), "%s", "Lets a phone connect straight to NAV-1 where there is no shared Wi-Fi, "
                                          "for example outdoors. Uses a little more power while on.");
  Apps::setText(s_ui.apStatus, buf);

  struct tm lt;
  if (TimeService::local(lt)) snprintf(buf, sizeof(buf), "Now: %02d:%02d, %02d.%02d.%04d\nSet from %s", lt.tm_hour, lt.tm_min,
                                       lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900,
                                       strcmp(TimeService::source(), "NTP") ? TimeService::source() : "the internet (Wi-Fi)");
  else snprintf(buf, sizeof(buf), "The clock is not set yet.\nIt sets itself from GPS or from the internet (Wi-Fi).");
  Apps::setText(s_ui.timeNow, buf);

  snprintf(buf, sizeof(buf), "Name\n    %s\nFirmware\n    %s %s\nStorage\n    %s", APP_NAME, APP_NAME,
           FW_VERSION, Storage::summary().c_str());
  Apps::setText(s_ui.about, buf);
}

void destroy() { s_ui = Ui(); }

}  // namespace

extern const AppImpl SETTINGS_APP = { "Settings", create, update, destroy };

// Console / screenshots: the page the app opens on next ("settings <0..4>").
void settingsAppSetPage(int page) { s_page = constrain(page, 0, P_COUNT - 1); }
