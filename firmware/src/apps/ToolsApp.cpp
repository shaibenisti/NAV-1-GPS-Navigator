// =============================================================================
//  Tools app: development diagnostics only (the same data as the serial console).
//  Product information lives in the normal apps (GPS, Settings). Menu on the left,
//  one page per tool (same layout as Settings, Apps::buildMenu):
//    Health     - one plain status line per part of the device (Diag::healthRows), every 2 s;
//                 the full technical report stays on the console ("diag")
//    Touch test - draw test: finger position, taps, press-to-frame latency
//    Update     - allow a firmware upload over Wi-Fi for 10 min (Ota, tools/scripts/ota.ps1)
// =============================================================================
#include "App.h"

#include <Arduino.h>
#include "../diag/Diag.h"
#include "../services/Location.h"
#include "../services/Ota.h"
#include "../services/WebService.h"
#include "../LvglPort.h"
#include "../../version.h"

namespace {

class StringPrint : public Print {
public:
  explicit StringPrint(String &s) : _s(s) {}
  size_t write(uint8_t c) override { _s += (char)c; return 1; }
  size_t write(const uint8_t *b, size_t n) override { _s.concat((const char *)b, n); return n; }
private:
  String &_s;
};

enum Page { P_HEALTH, P_TOUCH, P_UPDATE, P_COUNT };
const char *const PAGE_NAME[P_COUNT] = { LV_SYMBOL_LIST "  Health",
                                         LV_SYMBOL_EDIT "  Touch test", LV_SYMBOL_UPLOAD "  Update" };
int s_page = P_HEALTH;
constexpr int HEALTH_ROWS = 12;          // Diag::healthRows() gives 9 today

struct Ui {
  lv_obj_t *menu[P_COUNT] = {}, *page[P_COUNT] = {};
  lv_obj_t *hRow[12] = {}, *hDot[12] = {}, *hName[12] = {}, *hText[12] = {};
  lv_obj_t *touchArea = nullptr, *dot = nullptr, *touchInfo = nullptr;
  lv_obj_t *otaBtn = nullptr, *otaBtnLbl = nullptr, *otaInfo = nullptr;
  uint32_t taps = 0;
};
Ui s_ui;

lv_obj_t *textLabel(lv_obj_t *parent, const lv_font_t *font) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, "");
  return l;
}

void updateOtaTab() {
  const Ota::State st = Ota::state();
  Apps::setText(s_ui.otaBtnLbl, st == Ota::State::Armed ? LV_SYMBOL_CLOSE "  Stop allowing" : LV_SYMBOL_UPLOAD "  Allow upload (10 min)");
  const String url = WebService::url();
  char buf[400];
  snprintf(buf, sizeof(buf),
           "Firmware %s   (slot %s%s)\nUpdate: %s%s%s\n\n%s",
           FW_GIT_DESCRIBE, Ota::runningSlot().c_str(), Ota::pendingVerify() ? ", being verified" : "", Ota::stateName(st),
           Ota::lastError().length() ? " - " : "", Ota::lastError().c_str(),
           st == Ota::State::Armed ? (url.length() ? ("Send from the PC now:\n  .\\tools\\scripts\\ota.ps1 -NoArm -Ip " + WebService::url().substring(7, url.length() - 1)).c_str()
                                                   : "NAV-1 is not on Wi-Fi.")
                                   : "Installs a new firmware over Wi-Fi. A firmware that does not run properly for 20 s is replaced by the previous one automatically.");
  Apps::setText(s_ui.otaInfo, buf);
}

void onOtaButton(lv_event_t *) {
  if (Ota::state() == Ota::State::Armed) Ota::disarm();
  else Ota::arm();
  updateOtaTab();
}

void onTouchArea(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) s_ui.taps++;
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
    lv_obj_set_hidden(s_ui.dot, false);
    lv_area_t a;
    lv_obj_get_coords(s_ui.touchArea, &a);
    lv_obj_set_pos(s_ui.dot, p.x - a.x1 - 14, p.y - a.y1 - 14);
    char buf[64];
    snprintf(buf, sizeof(buf), "x %d  y %d   taps %u", (int)p.x, (int)p.y, (unsigned)s_ui.taps);
    Apps::setText(s_ui.touchInfo, buf);
  }
}

void showPage(int p) {
  s_page = constrain(p, 0, P_COUNT - 1);
  Apps::showMenuPage(s_ui.menu, s_ui.page, P_COUNT, s_page);
}
void onMenu(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }

void create(lv_obj_t *content) {
  s_ui = Ui();
  Apps::buildMenu(content, PAGE_NAME, P_COUNT, s_ui.menu, s_ui.page, onMenu);

  lv_obj_t *h = s_ui.page[P_HEALTH];
  Apps::pageTitle(h, LV_SYMBOL_LIST "  Health");
  lv_obj_t *rows = lv_obj_create(h);                          // one row per part of the device
  lv_obj_remove_style_all(rows);
  lv_obj_set_pos(rows, 0, 50);
  lv_obj_set_size(rows, Apps::PAGE_INNER_W, Apps::PAGE_H - 20 - 50);
  lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rows, 4, 0);
  lv_obj_set_scroll_dir(rows, LV_DIR_VER);
  for (int i = 0; i < HEALTH_ROWS; i++) {
    lv_obj_t *r = lv_obj_create(rows);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 28);
    lv_obj_t *dot = lv_obj_create(r);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *name = textLabel(r, &lv_font_montserrat_20);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *text = textLabel(r, &lv_font_montserrat_14);
    lv_obj_set_style_text_color(text, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_width(text, Apps::PAGE_INNER_W - 148);
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, 146, 1);
    s_ui.hRow[i] = r; s_ui.hDot[i] = dot; s_ui.hName[i] = name; s_ui.hText[i] = text;
  }

  lv_obj_t *tt = s_ui.page[P_TOUCH];
  Apps::pageTitle(tt, LV_SYMBOL_EDIT "  Touch test");
  s_ui.touchArea = lv_obj_create(tt);
  lv_obj_remove_style_all(s_ui.touchArea);
  lv_obj_set_pos(s_ui.touchArea, 0, 48);
  lv_obj_set_size(s_ui.touchArea, Apps::PAGE_INNER_W, Apps::PAGE_H - 20 - 48);
  lv_obj_set_style_border_color(s_ui.touchArea, lv_color_white(), 0);
  lv_obj_set_style_border_opa(s_ui.touchArea, LV_OPA_40, 0);
  lv_obj_set_style_border_width(s_ui.touchArea, 1, 0);
  lv_obj_set_clickable(s_ui.touchArea, true);
  lv_obj_set_scrollable(s_ui.touchArea, false);
  lv_obj_add_event_cb(s_ui.touchArea, onTouchArea, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_ui.touchArea, onTouchArea, LV_EVENT_PRESSING, nullptr);
  s_ui.touchInfo = textLabel(s_ui.touchArea, &lv_font_montserrat_20);
  lv_obj_align(s_ui.touchInfo, LV_ALIGN_TOP_LEFT, 8, 6);
  lv_label_set_text(s_ui.touchInfo, "Touch and drag anywhere in this box");
  s_ui.dot = lv_obj_create(s_ui.touchArea);
  lv_obj_remove_style_all(s_ui.dot);
  lv_obj_set_size(s_ui.dot, 28, 28);
  lv_obj_set_style_radius(s_ui.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_ui.dot, lv_color_hex(0xFFB300), 0);
  lv_obj_set_style_bg_opa(s_ui.dot, LV_OPA_COVER, 0);
  lv_obj_set_hidden(s_ui.dot, true);
  lv_obj_set_clickable(s_ui.dot, false);

  lv_obj_t *up = s_ui.page[P_UPDATE];
  Apps::pageTitle(up, LV_SYMBOL_UPLOAD "  Update");
  s_ui.otaBtn = lv_button_create(up);
  lv_obj_set_size(s_ui.otaBtn, 300, 56);
  lv_obj_set_pos(s_ui.otaBtn, 0, 50);
  s_ui.otaBtnLbl = lv_label_create(s_ui.otaBtn);
  lv_obj_center(s_ui.otaBtnLbl);
  lv_obj_add_event_cb(s_ui.otaBtn, onOtaButton, LV_EVENT_CLICKED, nullptr);
  s_ui.otaInfo = textLabel(up, &lv_font_montserrat_20);
  lv_obj_set_pos(s_ui.otaInfo, 0, 124);
  lv_obj_set_width(s_ui.otaInfo, Apps::PAGE_INNER_W);
  updateOtaTab();
  showPage(s_page);
}

void update() {
  static uint32_t lastField = 0;
  if (s_page == P_UPDATE && millis() - lastField >= 1000) {
    lastField = millis();
    updateOtaTab();
  }
  static uint32_t last = 0;
  const bool healthEmpty = s_page == P_HEALTH && !lv_label_get_text(s_ui.hName[0])[0];   // just opened: fill now
  if (millis() - last < 2000 && !healthEmpty) return;
  last = millis();
  if (s_page == P_HEALTH) {
    static Diag::HealthRow rows[HEALTH_ROWS];
    static const uint32_t COLOR[] = { 0x66BB6A, 0xFFB300, 0xFF5252, 0x8A96A6 };   // OK, attention, problem, off
    const int n = Diag::healthRows(rows, HEALTH_ROWS);
    for (int i = 0; i < HEALTH_ROWS; i++) {
      lv_obj_set_hidden(s_ui.hRow[i], i >= n);
      if (i >= n) continue;
      Apps::setText(s_ui.hName[i], rows[i].name);
      Apps::setText(s_ui.hText[i], rows[i].text);
      lv_obj_set_style_bg_color(s_ui.hDot[i], lv_color_hex(COLOR[rows[i].level]), 0);
    }
  }
  if (s_page == P_TOUCH) {
    const uint32_t lat = LvglPort::takeTouchLatencyUs();
    if (lat) {
      char buf[80];
      snprintf(buf, sizeof(buf), "taps %u   last press -> screen %.0f ms", (unsigned)s_ui.taps, lat / 1000.0f);
      Apps::setText(s_ui.touchInfo, buf);
    }
  }
}

void destroy() { s_ui = Ui(); }

}  // namespace

extern const AppImpl TOOLS_APP = { "Tools", create, update, destroy };

// Console / screenshots: the page the app opens on next ("tools <0..4>").
void toolsAppSetPage(int page) { s_page = constrain(page, 0, P_COUNT - 1); }
