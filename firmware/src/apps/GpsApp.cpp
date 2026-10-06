// =============================================================================
//  GPS app: live GPS, honest about fix quality. Menu on
//  the left, one page per topic (same layout as Settings / Tools):
//    Position    state, coordinates, accuracy, altitude, speed, heading
//    Satellites  one signal bar per satellite: used in the fix / received / not received
//    Details     UTC time, time to first fix, fix type, position age, GPS module
//  States: NO GPS SIGNAL (no data) / ACQUIRING / FIX (RMC 'A' + position < 2 s, D8) /
//  FIX LOST (last values dimmed). User information only; link diagnostics are in Tools.
// =============================================================================
#include "App.h"

#include <Arduino.h>
#include <algorithm>
#include "../services/Location.h"

namespace {

enum Page { P_POSITION, P_SATS, P_DETAILS, P_COUNT };
const char *const PAGE_NAME[P_COUNT] = { LV_SYMBOL_GPS "  Position", LV_SYMBOL_WIFI "  Satellites", LV_SYMBOL_LIST "  Details" };
int s_page = P_POSITION;

constexpr int BARS = 20;                 // satellite bars created once
constexpr int CHART_H = 190, CHART_Y = 60;
constexpr uint32_t C_FIX = 0x43A047, C_WAIT = 0xFFB300, C_BAD = 0xE53935, C_OFF = 0x8A96A6, C_RX = 0x1E88E5;

struct Ui {
  lv_obj_t *menu[P_COUNT] = {}, *page[P_COUNT] = {};
  lv_obj_t *state = nullptr, *lat = nullptr, *lon = nullptr, *val[4] = {};
  lv_obj_t *satSummary = nullptr, *chart = nullptr, *bar[BARS] = {}, *barLbl[BARS] = {}, *barVal[BARS] = {}, *noSats = nullptr;
  lv_obj_t *detail[7] = {};
};
Ui s_ui;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

void showPage(int p) {
  s_page = constrain(p, 0, P_COUNT - 1);
  Apps::showMenuPage(s_ui.menu, s_ui.page, P_COUNT, s_page);
}
void onMenu(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }

const char *cardinal(double deg) {
  static const char *const N[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  return N[(int)((deg + 22.5) / 45.0) & 7];
}

// ---- pages ------------------------------------------------------------------------------
const char *const CAPTION[4] = { "Accuracy", "Altitude", "Speed", "Heading" };
const char *const DETAIL[7] = { "Time (UTC)", "Date", "First fix", "Fix type", "Satellites", "Position age", "GPS module" };

void buildPosition(lv_obj_t *p) {
  Apps::pageTitle(p, LV_SYMBOL_GPS "  Position");
  s_ui.state = label(p, &lv_font_montserrat_20, lv_color_white());
  lv_obj_align(s_ui.state, LV_ALIGN_TOP_RIGHT, -4, 10);
  s_ui.lat = label(p, &lv_font_montserrat_28, lv_color_white());
  lv_obj_set_pos(s_ui.lat, 4, 56);
  s_ui.lon = label(p, &lv_font_montserrat_28, lv_color_white());
  lv_obj_set_pos(s_ui.lon, 4, 92);
  for (int i = 0; i < 4; i++) {
    const int x = 4 + (i % 2) * 262, y = 150 + (i / 2) * 92;
    lv_obj_set_pos(label(p, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB), CAPTION[i]), x, y);
    s_ui.val[i] = label(p, &lv_font_montserrat_28, lv_color_white());
    lv_obj_set_pos(s_ui.val[i], x, y + 22);
  }
}

void buildSats(lv_obj_t *p) {
  Apps::pageTitle(p, LV_SYMBOL_WIFI "  Satellites");
  s_ui.satSummary = label(p, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD));
  lv_obj_align(s_ui.satSummary, LV_ALIGN_TOP_RIGHT, -4, 10);
  s_ui.chart = lv_obj_create(p);
  lv_obj_remove_style_all(s_ui.chart);
  lv_obj_set_pos(s_ui.chart, 0, CHART_Y);
  lv_obj_set_size(s_ui.chart, Apps::PAGE_INNER_W, CHART_H + 44);
  for (int i = 0; i < BARS; i++) {
    s_ui.bar[i] = lv_obj_create(s_ui.chart);
    lv_obj_remove_style_all(s_ui.bar[i]);
    lv_obj_set_style_bg_opa(s_ui.bar[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ui.bar[i], 4, 0);
    s_ui.barVal[i] = label(s_ui.chart, &lv_font_montserrat_14, lv_color_hex(0xDDDDDD));
    s_ui.barLbl[i] = label(s_ui.chart, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB));
  }
  s_ui.noSats = label(s_ui.chart, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD), "");
  lv_obj_set_pos(s_ui.noSats, 4, 60);
  lv_obj_set_width(s_ui.noSats, Apps::PAGE_INNER_W - 8);
  // legend
  const uint32_t lc[3] = { C_FIX, C_RX, C_OFF };
  const char *const lt[3] = { "used in the fix", "received", "not received" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *sw = lv_obj_create(p);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 12, 12);
    lv_obj_set_style_radius(sw, 3, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(lc[i]), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_pos(sw, 4 + i * 150, 322);
    lv_obj_set_pos(label(p, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB), lt[i]), 22 + i * 150, 318);
  }
}

void buildDetails(lv_obj_t *p) {
  Apps::pageTitle(p, LV_SYMBOL_LIST "  Details");
  for (int i = 0; i < 7; i++) {
    const int y = 54 + i * 40;
    lv_obj_set_pos(label(p, &lv_font_montserrat_20, lv_color_white(), DETAIL[i]), 4, y);
    s_ui.detail[i] = label(p, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD));
    lv_obj_set_pos(s_ui.detail[i], 190, y);
    lv_obj_set_width(s_ui.detail[i], Apps::PAGE_INNER_W - 194);
    lv_label_set_long_mode(s_ui.detail[i], LV_LABEL_LONG_DOT);
  }
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  Apps::buildMenu(content, PAGE_NAME, P_COUNT, s_ui.menu, s_ui.page, onMenu);
  buildPosition(s_ui.page[P_POSITION]);
  buildSats(s_ui.page[P_SATS]);
  buildDetails(s_ui.page[P_DETAILS]);
  showPage(s_page);
}

// ---- update -----------------------------------------------------------------------------
void updatePosition(const GpsData &d, Location::Quality q) {
  static const char *const STATE[] = { "NO GPS SIGNAL", "ACQUIRING", "FIX", "FIX LOST" };
  static const uint32_t COLOR[] = { C_BAD, C_WAIT, C_FIX, C_WAIT };
  Apps::setText(s_ui.state, STATE[(int)q]);
  lv_obj_set_style_text_color(s_ui.state, lv_color_hex(COLOR[(int)q]), 0);
  char buf[64];
  const bool fix = q == Location::Quality::Fix;
  if (d.locValid) {
    snprintf(buf, sizeof(buf), "%.6f\xC2\xB0 %c", fabs(d.lat), d.lat < 0 ? 'S' : 'N');
    Apps::setText(s_ui.lat, buf);
    snprintf(buf, sizeof(buf), "%.6f\xC2\xB0 %c", fabs(d.lng), d.lng < 0 ? 'W' : 'E');
    Apps::setText(s_ui.lon, buf);
  } else {
    Apps::setText(s_ui.lat, q == Location::Quality::NoSignal ? "No data from the GPS module" : "Waiting for a position...");
    Apps::setText(s_ui.lon, "");
  }
  const lv_opa_t opa = fix ? LV_OPA_COVER : LV_OPA_50;          // last known values dimmed
  lv_obj_set_style_text_opa(s_ui.lat, opa, 0);
  lv_obj_set_style_text_opa(s_ui.lon, opa, 0);

  if (d.hdopValid && d.hdop < 50 && fix) snprintf(buf, sizeof(buf), "%s", Location::hdopWord(d.hdop));
  else snprintf(buf, sizeof(buf), "--");
  Apps::setText(s_ui.val[0], buf);
  if (d.altValid && d.locValid) snprintf(buf, sizeof(buf), "%.0f m", d.altM);
  else snprintf(buf, sizeof(buf), "--");
  Apps::setText(s_ui.val[1], buf);
  if (d.speedValid && fix) snprintf(buf, sizeof(buf), "%.1f km/h", d.speedKmh);
  else snprintf(buf, sizeof(buf), "--");
  Apps::setText(s_ui.val[2], buf);
  if (d.courseValid && fix && d.speedValid && d.speedKmh >= 2) snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 %s", d.courseDeg, cardinal(d.courseDeg));
  else snprintf(buf, sizeof(buf), "--");                          // heading is noise when standing
  Apps::setText(s_ui.val[3], buf);
  lv_obj_set_style_text_opa(s_ui.val[1], opa, 0);
}

void updateSats(const GpsData &d) {
  static GpsSat sats[GpsParser::MAX_SATS];
  int n = Location::satellites(sats, GpsParser::MAX_SATS);
  // strongest first: the bars read like a ranking
  std::sort(sats, sats + n, [](const GpsSat &a, const GpsSat &b) { return a.snr > b.snr; });
  n = min(n, BARS);
  int used = 0;
  for (int i = 0; i < n; i++) used += sats[i].used;
  char buf[64];
  snprintf(buf, sizeof(buf), "%d used  -  %u in view", used, (unsigned)d.satsInView());
  Apps::setText(s_ui.satSummary, buf);
  Apps::setText(s_ui.noSats, n ? "" : d.linkUp ? "No satellites received yet. Outdoors with a clear view of the sky this takes 1-4 minutes."
                                                : "No data from the GPS module.");
  const int slot = n ? min(48, Apps::PAGE_INNER_W / n) : 48, w = slot - 10;
  for (int i = 0; i < BARS; i++) {
    const bool on = i < n;
    lv_obj_set_hidden(s_ui.bar[i], !on);
    lv_obj_set_hidden(s_ui.barLbl[i], !on);
    lv_obj_set_hidden(s_ui.barVal[i], !on);
    if (!on) continue;
    const GpsSat &s = sats[i];
    const int h = s.snr > 0 ? max(6, min(CHART_H, s.snr * CHART_H / 50)) : 4;
    const int x = i * slot + 4;
    lv_obj_set_pos(s_ui.bar[i], x, CHART_H - h);
    lv_obj_set_size(s_ui.bar[i], w, h);
    lv_obj_set_style_bg_color(s_ui.bar[i], lv_color_hex(s.used ? C_FIX : s.snr > 0 ? C_RX : C_OFF), 0);
    snprintf(buf, sizeof(buf), "%s%u", s.gnss == GNSS_GLONASS ? "R" : s.gnss == GNSS_GALILEO ? "E" : "", (unsigned)s.prn);
    Apps::setText(s_ui.barLbl[i], buf);
    lv_obj_set_pos(s_ui.barLbl[i], x, CHART_H + 6);
    if (s.snr > 0) snprintf(buf, sizeof(buf), "%d", s.snr);
    else buf[0] = 0;
    Apps::setText(s_ui.barVal[i], buf);
    lv_obj_set_pos(s_ui.barVal[i], x, CHART_H - h - 20);
  }
}

void updateDetails(const GpsData &d, Location::Quality q) {
  char buf[64];
  if (d.timeValid) snprintf(buf, sizeof(buf), "%02u:%02u:%02u", d.hour, d.minute, d.second);
  else snprintf(buf, sizeof(buf), "--");
  Apps::setText(s_ui.detail[0], buf);
  if (d.dateValid) snprintf(buf, sizeof(buf), "%02u.%02u.%04u", d.day, d.month, d.year);
  else snprintf(buf, sizeof(buf), "--");
  Apps::setText(s_ui.detail[1], buf);
  const uint32_t ttff = Location::timeToFirstFixS();
  if (ttff) snprintf(buf, sizeof(buf), "%lu min %02lu s after start-up", (unsigned long)(ttff / 60), (unsigned long)(ttff % 60));
  else snprintf(buf, sizeof(buf), "not yet (running %lu min)", (unsigned long)(millis() / 60000));
  Apps::setText(s_ui.detail[2], buf);
  Apps::setText(s_ui.detail[3], q == Location::Quality::Fix ? (d.fixQuality == 2 ? "3D fix (DGPS)" : "3D fix")
                                : q == Location::Quality::NoSignal ? "no data" : "no fix");
  snprintf(buf, sizeof(buf), "%u used, %u in view", (unsigned)d.satsUsed, (unsigned)d.satsInView());
  Apps::setText(s_ui.detail[4], buf);
  if (d.locValid && d.locAgeMs < 100000) snprintf(buf, sizeof(buf), "%.1f s", d.locAgeMs / 1000.0);
  else snprintf(buf, sizeof(buf), "--");
  Apps::setText(s_ui.detail[5], buf);
  const GpsLink::Stats &s = Location::linkStats();
  if (!d.linkUp) snprintf(buf, sizeof(buf), "no data");
  else if (s.receiverRestarts) snprintf(buf, sizeof(buf), "OK, restarted %u times", (unsigned)s.receiverRestarts);
  else snprintf(buf, sizeof(buf), "OK");
  Apps::setText(s_ui.detail[6], buf);
}

void update() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  const GpsData d = Location::snapshot();
  const Location::Quality q = Location::quality(d);
  if (s_page == P_POSITION) updatePosition(d, q);
  else if (s_page == P_SATS) updateSats(d);
  else updateDetails(d, q);
}

void destroy() { s_ui = Ui(); }

}  // namespace

extern const AppImpl GPS_APP = { "GPS", create, update, destroy };

// Console / screenshots: the page the app opens on next ("gpsapp <0..2>").
void gpsAppSetPage(int page) { s_page = constrain(page, 0, P_COUNT - 1); }
