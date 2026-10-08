// =============================================================================
//  Navigate app: the guidance screen (services/Navigator).
//    Navigating   destination name, a big arrow (towards the destination, or along the route being
//                 followed), the distance, ETA / speed / bearing, off-route and arrival states;
//                 Map, Reverse (routes) and Stop.
//    Not          Go to a saved place (opens Places) or follow a recorded trip - the trips are listed
//                 here; the direction is picked from where you stand (at the trip's end = back to its
//                 start).
//  While moving the arrow is relative to the direction of travel (straight up = straight on); standing
//  still there is no heading (no magnetic sensor), so the dial turns north up and says so.
//  The screen does not dim while this app is open. Product UI only.
// =============================================================================
#include "App.h"
#include "../shell/Shell.h"

#include <Arduino.h>
#include <math.h>
#include <new>
#include <esp_heap_caps.h>
#include "../services/Backlight.h"
#include "../services/Geo.h"
#include "../services/Location.h"
#include "../services/Navigator.h"
#include "../services/TripRecorder.h"
#include "../ui/NavUi.h"
#include "../ui/UiText.h"

namespace {

constexpr int DIAL = 330;
constexpr uint32_t C_ARROW = 0x4FC3F7, C_ARRIVED = 0x66BB6A, C_WARN = 0xFFB300, C_OFF = 0xFF5252, C_STOP = 0xC62828;
constexpr int MAX_TRIPS = 30;

struct Ui {
  lv_obj_t *nav = nullptr, *idle = nullptr, *mapBtn = nullptr, *stopBtn = nullptr;
  lv_obj_t *name = nullptr, *sub = nullptr, *dial = nullptr, *arrow = nullptr, *dist = nullptr, *state = nullptr, *bar = nullptr;
  lv_obj_t *eta = nullptr, *speed = nullptr, *brg = nullptr, *rev = nullptr;
  lv_obj_t *tripList = nullptr;
};
Ui s_ui;
uint32_t s_rev = 0;
float s_northDeg = 0;                    // where north is on the dial (0 = up)
bool s_relative = false;
TripRecorder::Summary *s_trips = nullptr;   // PSRAM while the trip list is shown
int s_tripCount = 0;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color, int w, int h, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_center(label(b, &lv_font_montserrat_20, 0xFFFFFF, text));
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

lv_obj_t *stat(lv_obj_t *parent, int x, int y, const char *caption) {
  lv_obj_set_pos(label(parent, &lv_font_montserrat_14, 0xBBBBBB, caption), x, y);
  lv_obj_t *v = label(parent, &lv_font_montserrat_28, 0xFFFFFF, "--");
  lv_obj_set_pos(v, x, y + 18);
  return v;
}

// Dial: ring, north mark (red), ticks; the arrow object sits on top of it
void onDrawDial(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, R = DIAL / 2 - 4;
  lv_draw_rect_dsc_t ring;
  lv_draw_rect_dsc_init(&ring);
  ring.radius = LV_RADIUS_CIRCLE;
  ring.bg_color = lv_color_black();
  ring.bg_opa = LV_OPA_30;
  ring.border_color = lv_color_hex(0x9FA8DA);
  ring.border_width = 3;
  lv_area_t ra = { cx - R, cy - R, cx + R, cy + R };
  lv_draw_rect(layer, &ring, &ra);
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  for (int b = 0; b < 360; b += 30) {
    const float th = (b + s_northDeg) * (float)M_PI / 180.0f;
    const float r1 = R - 4, r2 = R - (b == 0 ? 30 : 16);
    ln.color = b == 0 ? lv_color_hex(0xE53935) : lv_color_white();
    ln.width = b == 0 ? 6 : 3;
    ln.p1.x = cx + r1 * sinf(th); ln.p1.y = cy - r1 * cosf(th);
    ln.p2.x = cx + r2 * sinf(th); ln.p2.y = cy - r2 * cosf(th);
    lv_draw_line(layer, &ln);
  }
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.font = &lv_font_montserrat_20;
  ld.color = lv_color_hex(0xE53935);
  ld.align = LV_TEXT_ALIGN_CENTER;
  ld.text = "N";
  ld.text_static = 1;
  const float th = s_northDeg * (float)M_PI / 180.0f;
  const int nx = cx + (int)((R - 48) * sinf(th)), ny = cy - (int)((R - 48) * cosf(th));
  lv_area_t la = { nx - 14, ny - 12, nx + 14, ny + 12 };
  lv_draw_label(layer, &ld, &la);
}

// ---- not navigating: start one ---------------------------------------------------------------
void onGoPlace(lv_event_t *) { Shell::switchApp("Places"); }

void onTrip(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (!s_trips || i < 0 || i >= s_tripCount) return;
  Navigator::follow(s_trips[i].base, s_trips[i].title.c_str());
}

void freeTrips() {
  if (!s_trips) return;
  for (int i = 0; i < MAX_TRIPS; i++) s_trips[i].~Summary();
  heap_caps_free(s_trips);
  s_trips = nullptr;
  s_tripCount = 0;
}

void buildTripList() {
  lv_obj_clean(s_ui.tripList);
  freeTrips();
  s_trips = (TripRecorder::Summary *)heap_caps_malloc(sizeof(TripRecorder::Summary) * MAX_TRIPS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_trips) return;
  for (int i = 0; i < MAX_TRIPS; i++) new (&s_trips[i]) TripRecorder::Summary();
  s_tripCount = TripRecorder::list(s_trips, MAX_TRIPS);
  if (!s_tripCount) label(s_ui.tripList, &lv_font_montserrat_16, 0xBBBBBB, "No recorded trips yet (Trips > Record).");
  for (int i = 0; i < s_tripCount; i++) {
    const TripRecorder::Summary &t = s_trips[i];
    lv_obj_t *b = lv_button_create(s_ui.tripList);
    lv_obj_set_size(b, lv_pct(100), 56);
    lv_obj_add_event_cb(b, onTrip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_align(label(b, &lv_font_montserrat_20, 0xFFFFFF, t.title.c_str()), LV_ALIGN_TOP_LEFT, 0, -6);
    char s[48];
    snprintf(s, sizeof(s), "%.2f km", t.distanceKm);
    lv_obj_align(label(b, &lv_font_montserrat_14, 0xE0E0E0, s), LV_ALIGN_BOTTOM_LEFT, 0, 6);
    lv_obj_align(label(b, &lv_font_montserrat_20, 0xFFFFFF, LV_SYMBOL_LOOP), LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

void buildIdle(lv_obj_t *content) {
  lv_obj_t *p = Apps::card(content, 12, 0, Apps::PAGE_W, Apps::CONTENT_H - 12);
  s_ui.idle = p;
  Apps::pageTitle(p, "Where to?");
  lv_obj_set_pos(button(p, LV_SYMBOL_HOME "  Go to a saved place", 0x1E88E5, Apps::PAGE_INNER_W, 64, onGoPlace), 0, 52);
  lv_obj_t *t = label(p, &lv_font_montserrat_20, 0xFFFFFF, LV_SYMBOL_LOOP "  Follow a recorded trip");
  lv_obj_set_pos(t, 4, 134);
  lv_obj_t *h = label(p, &lv_font_montserrat_14, 0xBBBBBB,
                      "Standing at a trip's end, NAV-1 leads you back to its start (back-track); "
                      "Reverse on the next screen turns it around.");
  lv_obj_set_width(h, Apps::PAGE_INNER_W);
  lv_obj_set_pos(h, 4, 164);
  s_ui.tripList = lv_obj_create(p);
  lv_obj_remove_style_all(s_ui.tripList);
  lv_obj_set_pos(s_ui.tripList, 0, 214);
  lv_obj_set_size(s_ui.tripList, Apps::PAGE_INNER_W, Apps::CONTENT_H - 12 - 214 - 20);
  lv_obj_set_flex_flow(s_ui.tripList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ui.tripList, 8, 0);
  lv_obj_set_scroll_dir(s_ui.tripList, LV_DIR_VER);
}

// ---- navigating --------------------------------------------------------------------------------
void onMap(lv_event_t *) { Shell::switchApp("Map"); }
void onReverse(lv_event_t *) { Navigator::reverse(); }
void onStop(lv_event_t *) { Navigator::stop(); }

void buildNav(lv_obj_t *content) {
  lv_obj_t *n = lv_obj_create(content);
  lv_obj_remove_style_all(n);
  lv_obj_set_size(n, Apps::CONTENT_W, Apps::CONTENT_H);
  s_ui.nav = n;
  s_ui.name = label(n, &nav_he_28, 0xFFFFFF);
  lv_obj_set_width(s_ui.name, Apps::CONTENT_W - 24);
  lv_obj_set_style_text_align(s_ui.name, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_ui.name, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_ui.name, 12, 0);
  s_ui.sub = label(n, &lv_font_montserrat_16, 0xBBBBBB);
  lv_obj_set_width(s_ui.sub, Apps::CONTENT_W - 24);
  lv_obj_set_style_text_align(s_ui.sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_ui.sub, 12, 36);
  s_ui.dial = lv_obj_create(n);
  lv_obj_remove_style_all(s_ui.dial);
  lv_obj_set_size(s_ui.dial, DIAL, DIAL);
  lv_obj_set_pos(s_ui.dial, (Apps::CONTENT_W - DIAL) / 2, 62);
  lv_obj_add_event_cb(s_ui.dial, onDrawDial, LV_EVENT_DRAW_MAIN, nullptr);
  s_ui.arrow = NavUi::arrow(s_ui.dial, 190, C_ARROW);
  lv_obj_center(s_ui.arrow);
  s_ui.dist = label(n, &lv_font_montserrat_48, 0xFFFFFF, "--");
  lv_obj_align(s_ui.dist, LV_ALIGN_TOP_MID, 0, 398);
  s_ui.state = label(n, &lv_font_montserrat_20, C_WARN);
  lv_obj_set_width(s_ui.state, Apps::CONTENT_W - 24);
  lv_obj_set_style_text_align(s_ui.state, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_ui.state, 12, 456);
  s_ui.bar = lv_bar_create(n);
  lv_obj_set_size(s_ui.bar, Apps::CONTENT_W - 48, 8);
  lv_obj_set_pos(s_ui.bar, 24, 486);
  lv_bar_set_range(s_ui.bar, 0, 1000);
  s_ui.eta = stat(n, 24, 504, "Arrival in");
  s_ui.speed = stat(n, 184, 504, "Speed");
  s_ui.brg = stat(n, 330, 504, "Bearing");
  const int y = Apps::CONTENT_H - 72, w = (Apps::CONTENT_W - 24 - 2 * 10) / 3;
  s_ui.mapBtn = button(n, LV_SYMBOL_IMAGE "  Map", 0x00897B, w, 64, onMap);
  lv_obj_set_pos(s_ui.mapBtn, 12, y);
  s_ui.rev = button(n, LV_SYMBOL_LOOP "  Reverse", 0x37474F, w, 64, onReverse);
  lv_obj_set_pos(s_ui.rev, 12 + w + 10, y);
  s_ui.stopBtn = button(n, LV_SYMBOL_STOP "  Stop", C_STOP, w, 64, onStop);
  lv_obj_set_pos(s_ui.stopBtn, 12 + 2 * (w + 10), y);
}

void refreshMode() {
  s_rev = Navigator::revision();
  const bool on = Navigator::active();
  lv_obj_set_hidden(s_ui.nav, !on);
  lv_obj_set_hidden(s_ui.idle, on);
  if (!on) { buildTripList(); return; }
  freeTrips();
  const Navigator::Status st = Navigator::status();
  UiText::setName(s_ui.name, st.name);
  const bool route = st.mode == Navigator::Mode::Route;
  lv_obj_set_hidden(s_ui.rev, !route);
  const int w3 = (Apps::CONTENT_W - 24 - 2 * 10) / 3, w2 = (Apps::CONTENT_W - 24 - 10) / 2;   // 3 buttons, or Map + Stop
  lv_obj_set_width(s_ui.mapBtn, route ? w3 : w2);
  lv_obj_set_width(s_ui.stopBtn, route ? w3 : w2);
  lv_obj_set_x(s_ui.stopBtn, route ? 12 + 2 * (w3 + 10) : 12 + w2 + 10);
  lv_obj_set_hidden(s_ui.bar, !route);
  Apps::setText(s_ui.sub, route ? (st.reverse ? "Following the trip back to its start" : "Following the trip") : "Straight line to the place");
}

void updateNav() {
  const Navigator::Status st = Navigator::status();
  char b[48];
  Geo::formatDistance(st.distM, b, sizeof(b));
  Apps::setText(s_ui.dist, st.arrived ? "Arrived" : b);
  lv_obj_set_style_text_color(s_ui.dist, lv_color_hex(st.arrived ? C_ARRIVED : 0xFFFFFF), 0);

  float angle = st.bearingDeg;
  const bool moving = NavUi::relative(st.bearingDeg, angle);
  float h;
  NavUi::heading(h);
  const float north = moving ? -h : 0.0f;
  if (fabsf(north - s_northDeg) >= 1.0f || moving != s_relative) { s_northDeg = north; s_relative = moving; lv_obj_invalidate(s_ui.dial); }
  NavUi::setArrow(s_ui.arrow, angle, st.fix && st.distM >= 0 && !st.arrived);

  const char *state = "";
  uint32_t color = C_WARN;
  if (!st.fix) state = st.distM < 0 ? "Waiting for a GPS fix" : "GPS fix lost - last known distance";
  else if (st.arrived) { state = "Stop ends the guidance"; color = C_ARRIVED; }
  else if (st.offRoute) { snprintf(b, sizeof(b), "Off the route by %.0f m - the arrow leads back", st.offM); state = b; color = C_OFF; }
  else if (!moving) { state = "Standing still: dial north up"; color = 0xBBBBBB; }
  Apps::setText(s_ui.state, state);
  lv_obj_set_style_text_color(s_ui.state, lv_color_hex(color), 0);
  if (st.mode == Navigator::Mode::Route) lv_bar_set_value(s_ui.bar, (int32_t)(st.progress * 1000), LV_ANIM_OFF);

  char e[24];
  NavUi::formatEta(st.etaS, e, sizeof(e));
  Apps::setText(s_ui.eta, e);
  const GpsData d = Location::snapshot();
  if (st.fix && d.speedValid) snprintf(e, sizeof(e), "%.0f km/h", d.speedKmh);
  else snprintf(e, sizeof(e), "--");
  Apps::setText(s_ui.speed, e);
  if (st.distM >= 0) snprintf(e, sizeof(e), "%s %03.0f\xC2\xB0", Geo::cardinal(st.bearingDeg), st.bearingDeg);
  else snprintf(e, sizeof(e), "--");
  Apps::setText(s_ui.brg, e);
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  s_northDeg = 0;
  s_relative = false;
  buildIdle(content);
  buildNav(content);
  refreshMode();
  if (Navigator::active()) updateNav();
  Backlight::keepAwake(true);
}

void update() {
  if (Navigator::revision() != s_rev) refreshMode();
  if (Navigator::active()) updateNav();
}

void destroy() {
  Backlight::keepAwake(false);
  freeTrips();
  s_ui = Ui();
}

}  // namespace

extern const AppImpl NAVIGATE_APP = { "Navigate", create, update, destroy };
