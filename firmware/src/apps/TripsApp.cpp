// =============================================================================
//  Trips app. Tab strip on top, one page per topic (same layout
//  as Settings / Tools / GPS):
//    Record  Start / Stop, state, live values of the running trip
//    Trips   recorded trips, newest first; tap one for its numbers and route shape
//    Totals  all trips together: count, distance, time, longest, fastest
//  Recording itself is services/TripRecorder (keeps running when the app is closed).
// =============================================================================
#include "App.h"
#include "../shell/Shell.h"

#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <new>
#include "../services/TripRecorder.h"
#include "../services/Location.h"
#include "../services/Navigator.h"
#include "../services/Storage.h"

namespace {

enum Page { P_RECORD, P_TRIPS, P_TOTALS, P_COUNT };
const char *const PAGE_NAME[P_COUNT] = { LV_SYMBOL_PLAY "  Record", LV_SYMBOL_LIST "  Trips", LV_SYMBOL_LOOP "  Totals" };
int s_page = P_RECORD;
int s_openTrip = -1;                     // console: open this trip's detail on the next create()

constexpr int DEL_W = 84;                // small delete button at the end of a list row
constexpr int MAX_TRIPS = 40, ROUTE_PTS = 400, ROUTE_W = 436, ROUTE_H = 230;
constexpr uint32_t C_GO = 0x2E7D32, C_STOP = 0xC62828, C_OK = 0x66BB6A, C_WAIT = 0xFFB300, C_ROUTE = 0x4FC3F7;

struct Ui {
  lv_obj_t *menu[P_COUNT] = {}, *page[P_COUNT] = {};
  lv_obj_t *recState = nullptr, *btn = nullptr, *btnLbl = nullptr, *recInfo = nullptr, *value[6] = {};
  lv_obj_t *tripsTitle = nullptr, *tripsCount = nullptr, *list = nullptr, *detail = nullptr, *back = nullptr;
  lv_obj_t *delBtn = nullptr, *delLbl = nullptr;
  lv_obj_t *total[6] = {};
  bool wasRecording = false, listPending = false;
};
Ui s_ui;
TripRecorder::Summary *s_trips = nullptr;   // PSRAM while the app is open (MAX_TRIPS)
int s_tripCount = 0;
int s_delArmed = -1;                            // list index whose delete button was tapped once (-1 none)
bool s_delFailed = false, s_delWide = false;    // a file could not be deleted / the armed button is the wide one
bool s_delAll = false;                          // deleting the whole /data/trips folder
int s_delStep = -1;                             // -1 idle, 0..2 deleting .gpx / .csv / .json, 3 finished
String s_delBase;
lv_point_precise_t *s_route = nullptr;   // points of the drawn route (PSRAM, kept while shown)

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

String hms(uint32_t s) {
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  else snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return b;
}

// Caption (small, grey) + value (large) at x, y; returns the value label.
lv_obj_t *stat(lv_obj_t *parent, int x, int y, const char *caption, const lv_font_t *font = &lv_font_montserrat_28) {
  lv_obj_set_pos(label(parent, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB), caption), x, y);
  lv_obj_t *v = label(parent, font, lv_color_white(), "--");
  lv_obj_set_pos(v, x, y + 20);
  return v;
}

void showPage(int p) {
  s_page = constrain(p, 0, P_COUNT - 1);
  Apps::showMenuPage(s_ui.menu, s_ui.page, P_COUNT, s_page);
}
void onMenu(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }

// ---- Record ------------------------------------------------------------------------------
const char *const REC_CAPTION[6] = { "Duration", "Distance", "Speed", "Average (moving)", "Max speed", "Track points" };

void onButton(lv_event_t *) {
  if (TripRecorder::recording()) TripRecorder::stop();
  else {
    String err;
    if (!TripRecorder::start(&err)) { lv_label_set_text_fmt(s_ui.recInfo, "Cannot record: %s", err.c_str()); return; }
  }
}

void buildRecord(lv_obj_t *p) {
  Apps::pageTitle(p, LV_SYMBOL_PLAY "  Record");
  s_ui.recState = label(p, &lv_font_montserrat_20, lv_color_white());
  lv_obj_align(s_ui.recState, LV_ALIGN_TOP_RIGHT, -4, 10);
  s_ui.btn = lv_button_create(p);
  lv_obj_set_size(s_ui.btn, Apps::PAGE_INNER_W, 64);
  lv_obj_set_pos(s_ui.btn, 0, 50);
  lv_obj_add_event_cb(s_ui.btn, onButton, LV_EVENT_CLICKED, nullptr);
  s_ui.btnLbl = label(s_ui.btn, &lv_font_montserrat_28, lv_color_white());
  lv_obj_center(s_ui.btnLbl);
  s_ui.recInfo = label(p, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD));
  lv_obj_set_pos(s_ui.recInfo, 4, 126);
  lv_obj_set_width(s_ui.recInfo, Apps::PAGE_INNER_W - 8);
  lv_label_set_long_mode(s_ui.recInfo, LV_LABEL_LONG_DOT);
  for (int i = 0; i < 6; i++) s_ui.value[i] = stat(p, 4 + (i % 2) * 226, 176 + (i / 2) * 84, REC_CAPTION[i]);
}

void updateRecord() {
  const bool rec = TripRecorder::recording();
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix;
  Apps::setText(s_ui.btnLbl, rec ? LV_SYMBOL_STOP "  Stop trip" : LV_SYMBOL_PLAY "  Start trip");
  lv_obj_set_style_bg_color(s_ui.btn, lv_color_hex(rec ? C_STOP : C_GO), 0);
  const bool waiting = rec ? TripRecorder::waitingForFix() : !fix;
  Apps::setText(s_ui.recState, rec ? (waiting ? "WAITING FOR FIX" : "RECORDING") : (fix ? "READY" : "NO FIX"));
  lv_obj_set_style_text_color(s_ui.recState, lv_color_hex(rec ? (waiting ? C_WAIT : 0xFF5252) : (fix ? C_OK : C_WAIT)), 0);
  if (rec) Apps::setText(s_ui.recInfo, waiting ? "The route starts once the GPS has a fix." : "Keeps recording when you leave this app.");
  else Apps::setText(s_ui.recInfo, fix ? "GPS ready. The trip is saved when you press Stop."
                                       : "Start now: the route begins at the first GPS fix.");
  char b[32];
  Apps::setText(s_ui.value[0], rec ? hms(TripRecorder::durationS()).c_str() : "--");
  snprintf(b, sizeof(b), "%.2f km", TripRecorder::distanceKm());
  Apps::setText(s_ui.value[1], rec ? b : "--");
  snprintf(b, sizeof(b), "%.1f km/h", rec ? TripRecorder::currentKmh() : (fix && d.speedValid ? d.speedKmh : 0.0));
  Apps::setText(s_ui.value[2], b);
  const float avg = TripRecorder::movingS() >= 30 ? TripRecorder::distanceKm() / (TripRecorder::movingS() / 3600.0f) : 0;
  snprintf(b, sizeof(b), "%.1f km/h", avg);
  Apps::setText(s_ui.value[3], rec && avg > 0 ? b : "--");
  snprintf(b, sizeof(b), "%.1f km/h", TripRecorder::maxKmh());
  Apps::setText(s_ui.value[4], rec ? b : "--");
  snprintf(b, sizeof(b), "%lu", (unsigned long)TripRecorder::points());
  Apps::setText(s_ui.value[5], rec ? b : "--");
}

// ---- Trips (list + detail) -----------------------------------------------------------------
void rebuildList();

void freeRoute() {
  heap_caps_free(s_route);
  s_route = nullptr;
}

void closeDetail() {
  if (s_ui.detail) { lv_obj_delete(s_ui.detail); s_ui.detail = nullptr; }
  s_ui.delBtn = s_ui.delLbl = nullptr;
  s_delArmed = -1;
  freeRoute();
  lv_obj_set_hidden(s_ui.list, false);
  lv_obj_set_hidden(s_ui.back, true);
  lv_obj_set_hidden(s_ui.tripsCount, false);
  lv_label_set_text(s_ui.tripsTitle, LV_SYMBOL_LIST "  Trips");
}
void onBack(lv_event_t *) { closeDetail(); }

// Route shape: lat/lon scaled into the box (longitude shrunk by cos(latitude)), aspect kept.
void drawRoute(lv_obj_t *box, const TripRecorder::Summary &t) {
  float *lat = (float *)heap_caps_malloc(ROUTE_PTS * sizeof(float) * 2, MALLOC_CAP_SPIRAM);
  if (!lat) return;
  float *lon = lat + ROUTE_PTS;
  const int n = TripRecorder::track(t.base, lat, lon, ROUTE_PTS);
  if (n < 2) {
    heap_caps_free(lat);
    lv_obj_center(label(box, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB), "No route recorded"));
    return;
  }
  float la0 = lat[0], la1 = lat[0], lo0 = lon[0], lo1 = lon[0];
  for (int i = 1; i < n; i++) { la0 = min(la0, lat[i]); la1 = max(la1, lat[i]); lo0 = min(lo0, lon[i]); lo1 = max(lo1, lon[i]); }
  const float k = cosf((la0 + la1) / 2 * (float)M_PI / 180);
  const float w = max((lo1 - lo0) * k, 1e-6f), h = max(la1 - la0, 1e-6f);
  const float pad = 16, scale = min((ROUTE_W - 2 * pad) / w, (ROUTE_H - 2 * pad) / h);
  const float ox = (ROUTE_W - w * scale) / 2, oy = (ROUTE_H - h * scale) / 2;
  s_route = (lv_point_precise_t *)heap_caps_malloc(n * sizeof(lv_point_precise_t), MALLOC_CAP_SPIRAM);
  if (!s_route) { heap_caps_free(lat); return; }
  for (int i = 0; i < n; i++) {
    s_route[i].x = ox + (lon[i] - lo0) * k * scale;
    s_route[i].y = ROUTE_H - (oy + (lat[i] - la0) * scale);     // north up
  }
  heap_caps_free(lat);
  lv_obj_t *line = lv_line_create(box);
  lv_line_set_points(line, s_route, n);
  lv_obj_set_style_line_width(line, 4, 0);
  lv_obj_set_style_line_color(line, lv_color_hex(C_ROUTE), 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  const uint32_t dotColor[2] = { 0x43A047, 0xE53935 };           // start, end
  for (int e = 0; e < 2; e++) {
    const lv_point_precise_t &pt = s_route[e ? n - 1 : 0];
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dotColor[e]), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_pos(dot, (int)pt.x - 7, (int)pt.y - 7);
  }
}

void onShowOnMap(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_tripCount) return;
  mapAppShowTrip(s_trips[i].base.c_str());
  Shell::switchApp("Map");
}

// Follow the trip with Navigator (at its end: back to its start) and show the guidance
void onFollow(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_tripCount) return;
  if (Navigator::follow(s_trips[i].base, s_trips[i].title.c_str())) Shell::switchApp("Navigate");
}

// Delete: the three files of the trip, one background delete after the other (.json last,
// so a trip that could not be deleted completely stays in the list).
// Starts the delete of the next existing file from s_delStep on; false = nothing left or cannot start
// (s_delFailed tells which).
bool startDeleteStep() {
  static const char *const EXT[3] = { ".gpx", ".csv", ".json" };
  for (; s_delStep < 3; s_delStep++) {
    const String path = s_delBase + EXT[s_delStep];
    if (!Storage::exists(path.c_str())) continue;
    if (Storage::removeStart(path.c_str())) return true;
    s_delFailed = true;
    return false;
  }
  return false;
}

// Delete buttons: a small one (icon only) at the end of each list row, and a wide "Delete all trips"
// under the list (index DEL_ALL). Both arm on the first tap and delete on the second.
enum DelText { DEL_IDLE, DEL_ARMED, DEL_BUSY, DEL_FAILED };
constexpr int DEL_ALL = 999;
void setDelText(lv_obj_t *lbl, bool wide, DelText t) {
  if (!lbl) return;
  static const char *const WIDE[4] = { LV_SYMBOL_TRASH "  Delete all trips", LV_SYMBOL_TRASH "  Tap again: delete ALL trips",
                                       "Deleting...", "Could not delete (recording?) - retry" };
  static const char *const SMALL[4] = { LV_SYMBOL_TRASH, "Sure?", "...", "Failed" };
  lv_label_set_text(lbl, wide ? WIDE[t] : SMALL[t]);
}

void onDelete(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || (i >= s_tripCount && i != DEL_ALL) || s_delStep >= 0) return;
  lv_obj_t *btn = lv_event_get_target_obj(e);
  if (s_delArmed != i) {                                         // first tap arms, the second deletes
    if (s_delArmed >= 0) setDelText(s_ui.delLbl, s_delWide, DEL_IDLE);   // another one was armed: disarm it
    s_delArmed = i;
    s_ui.delBtn = btn;
    s_ui.delLbl = lv_obj_get_child(btn, 0);
    s_delWide = lv_obj_get_width(btn) > 200;
    setDelText(s_ui.delLbl, s_delWide, DEL_ARMED);
    return;
  }
  s_delAll = i == DEL_ALL;
  if (!s_delAll) s_delBase = s_trips[i].base;
  s_delStep = 0;
  s_delFailed = false;
  lv_obj_add_state(s_ui.delBtn, LV_STATE_DISABLED);
  setDelText(s_ui.delLbl, s_delWide, DEL_BUSY);
  if (s_delAll) {                                                // the whole folder in one background delete
    if (!Storage::removeStart("/data/trips")) { s_delFailed = true; s_delStep = 3; }   // refused while a trip is recorded
  } else if (!startDeleteStep()) s_delStep = 3;                  // lets updateDelete() report
}

// Called by update(): one file after the other until all are gone.
void updateDelete() {
  if (s_delStep < 0 || Storage::removing()) return;
  if (s_delAll && s_delStep < 3) s_delFailed = Storage::removeFailed();
  else if (s_delStep < 3) {                                      // the file of this step is finished
    if (Storage::removeFailed()) s_delFailed = true;
    else { s_delStep++; if (startDeleteStep()) return; }
  }
  s_delStep = -1;
  if (!s_delFailed) { if (s_ui.detail) closeDetail(); rebuildList(); }
  else if (s_ui.delLbl) {                                        // still the same screen: say so, allow a retry
    s_delArmed = -1;
    lv_obj_remove_state(s_ui.delBtn, LV_STATE_DISABLED);
    setDelText(s_ui.delLbl, s_delWide, DEL_FAILED);
  }
}

void openDetail(int i) {
  if (i < 0 || i >= s_tripCount) return;
  const TripRecorder::Summary &t = s_trips[i];
  lv_obj_t *p = s_ui.page[P_TRIPS];
  lv_obj_set_hidden(s_ui.list, true);
  lv_obj_set_hidden(s_ui.back, false);
  lv_obj_set_hidden(s_ui.tripsCount, true);
  lv_label_set_text(s_ui.tripsTitle, t.title.c_str());
  s_ui.detail = lv_obj_create(p);
  lv_obj_remove_style_all(s_ui.detail);
  lv_obj_set_pos(s_ui.detail, 0, 50);
  lv_obj_set_size(s_ui.detail, Apps::PAGE_INNER_W, Apps::PAGE_H - 70);
  lv_obj_t *box = lv_obj_create(s_ui.detail);                    // route shape
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, ROUTE_W, ROUTE_H);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x0C1016), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 10, 0);
  drawRoute(box, t);
  char b[32];
  const int x = 0, y0 = ROUTE_H + 12, dx = 230;
  snprintf(b, sizeof(b), "%.2f km", t.distanceKm);
  lv_label_set_text(stat(s_ui.detail, x, y0, "Distance", &lv_font_montserrat_20), b);
  lv_label_set_text(stat(s_ui.detail, x + dx, y0, "Duration", &lv_font_montserrat_20), hms(t.durationS).c_str());
  lv_label_set_text(stat(s_ui.detail, x, y0 + 58, "Moving", &lv_font_montserrat_20), hms(t.movingS).c_str());
  snprintf(b, sizeof(b), "%.1f km/h", t.avgKmh);
  lv_label_set_text(stat(s_ui.detail, x + dx, y0 + 58, "Average", &lv_font_montserrat_20), t.avgKmh > 0 ? b : "--");
  snprintf(b, sizeof(b), "%.1f km/h", t.maxKmh);
  lv_label_set_text(stat(s_ui.detail, x, y0 + 116, "Max speed", &lv_font_montserrat_20), b);
  snprintf(b, sizeof(b), "%lu", (unsigned long)t.points);
  lv_label_set_text(stat(s_ui.detail, x + dx, y0 + 116, "Track points", &lv_font_montserrat_20), b);
  const int bw = (Apps::PAGE_INNER_W - 10) / 2;
  lv_obj_t *mb = lv_button_create(s_ui.detail);                // the route on the street map
  lv_obj_set_size(mb, bw, 56);
  lv_obj_set_pos(mb, 0, ROUTE_H + 12 + 180);
  lv_obj_center(label(mb, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_IMAGE "  Show on map"));
  lv_obj_add_event_cb(mb, onShowOnMap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  lv_obj_t *fb = lv_button_create(s_ui.detail);                // guidance along it (back-track)
  lv_obj_set_size(fb, bw, 56);
  lv_obj_set_pos(fb, bw + 10, ROUTE_H + 12 + 180);
  lv_obj_set_style_bg_color(fb, lv_color_hex(0xF4511E), 0);
  lv_obj_center(label(fb, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_LOOP "  Follow"));
  lv_obj_add_event_cb(fb, onFollow, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  lv_obj_t *hint = label(s_ui.detail, &lv_font_montserrat_14, lv_color_hex(0xBBBBBB),
                         "Follow: guidance along this route - at its end, back to its start. GPX download: Phone app > NAV-1 page > Trips.");
  lv_obj_set_pos(hint, 0, ROUTE_H + 12 + 246);
  lv_obj_set_width(hint, Apps::PAGE_INNER_W);
}

void onTrip(lv_event_t *e) { openDetail((int)(intptr_t)lv_event_get_user_data(e)); }

void buildTrips(lv_obj_t *p) {
  s_ui.tripsTitle = Apps::pageTitle(p, LV_SYMBOL_LIST "  Trips");
  lv_obj_set_width(s_ui.tripsTitle, Apps::PAGE_INNER_W - 150);
  lv_label_set_long_mode(s_ui.tripsTitle, LV_LABEL_LONG_DOT);
  s_ui.tripsCount = label(p, &lv_font_montserrat_20, lv_color_hex(0xDDDDDD));
  lv_obj_align(s_ui.tripsCount, LV_ALIGN_TOP_RIGHT, -4, 10);
  s_ui.back = lv_button_create(p);
  lv_obj_set_size(s_ui.back, 130, 40);
  lv_obj_align(s_ui.back, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_center(label(s_ui.back, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_LEFT " All"));
  lv_obj_add_event_cb(s_ui.back, onBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_hidden(s_ui.back, true);
  s_ui.list = lv_obj_create(p);
  lv_obj_remove_style_all(s_ui.list);
  lv_obj_set_pos(s_ui.list, 0, 50);
  lv_obj_set_size(s_ui.list, Apps::PAGE_INNER_W, Apps::PAGE_H - 70);
  lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ui.list, 8, 0);
  lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
}

// ---- Totals --------------------------------------------------------------------------------
const char *const TOTAL_CAPTION[6] = { "Trips", "Distance", "Time", "Moving", "Longest trip", "Top speed" };

void buildTotals(lv_obj_t *p) {
  Apps::pageTitle(p, LV_SYMBOL_LOOP "  Totals");
  for (int i = 0; i < 6; i++) s_ui.total[i] = stat(p, 4 + (i % 2) * 262, 56 + (i / 2) * 92, TOTAL_CAPTION[i]);
}

void updateTotals() {
  uint32_t dur = 0, mov = 0;
  float km = 0, longest = 0, top = 0;
  for (int i = 0; i < s_tripCount; i++) {
    dur += s_trips[i].durationS;
    mov += s_trips[i].movingS;
    km += s_trips[i].distanceKm;
    longest = max(longest, s_trips[i].distanceKm);
    top = max(top, s_trips[i].maxKmh);
  }
  char b[32];
  snprintf(b, sizeof(b), "%d", s_tripCount);
  lv_label_set_text(s_ui.total[0], b);
  snprintf(b, sizeof(b), "%.2f km", km);
  lv_label_set_text(s_ui.total[1], b);
  lv_label_set_text(s_ui.total[2], hms(dur).c_str());
  lv_label_set_text(s_ui.total[3], hms(mov).c_str());
  snprintf(b, sizeof(b), "%.2f km", longest);
  lv_label_set_text(s_ui.total[4], b);
  snprintf(b, sizeof(b), "%.1f km/h", top);
  lv_label_set_text(s_ui.total[5], b);
}

void rebuildList() {
  s_tripCount = s_trips ? TripRecorder::list(s_trips, MAX_TRIPS) : 0;
  lv_obj_clean(s_ui.list);
  s_ui.delBtn = s_ui.delLbl = nullptr;
  s_delArmed = -1;
  lv_label_set_text_fmt(s_ui.tripsCount, "%d %s", s_tripCount, s_tripCount == 1 ? "trip" : "trips");
  if (!s_tripCount) label(s_ui.list, &lv_font_montserrat_20, lv_color_hex(0xBBBBBB), "No trips yet. Start one on the Record page.");
  for (int i = 0; i < s_tripCount; i++) {
    const TripRecorder::Summary &t = s_trips[i];
    lv_obj_t *row = lv_obj_create(s_ui.list);                    // trip button + small delete button
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 60);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, Apps::PAGE_INNER_W - DEL_W - 8, 60);
    lv_obj_add_event_cb(b, onTrip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *db = lv_button_create(row);
    lv_obj_set_size(db, DEL_W, 60);
    lv_obj_align(db, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(db, lv_color_hex(C_STOP), 0);
    lv_obj_center(label(db, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_TRASH));
    lv_obj_add_event_cb(db, onDelete, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *a = label(b, &lv_font_montserrat_20, lv_color_white(), t.title.c_str());
    lv_obj_align(a, LV_ALIGN_TOP_LEFT, 0, -6);
    char s[80];
    snprintf(s, sizeof(s), "%.2f km   %s   max %.0f km/h", t.distanceKm, hms(t.durationS).c_str(), t.maxKmh);
    lv_obj_align(label(b, &lv_font_montserrat_14, lv_color_hex(0xE0E0E0), s), LV_ALIGN_BOTTOM_LEFT, 0, 6);
    lv_obj_align(label(b, &lv_font_montserrat_20, lv_color_white(), LV_SYMBOL_RIGHT), LV_ALIGN_RIGHT_MID, 0, 0);
  }
  if (s_tripCount) {                                             // wide "delete all" at the very end
    lv_obj_t *all = lv_button_create(s_ui.list);
    lv_obj_set_size(all, lv_pct(100), 56);
    lv_obj_set_style_bg_color(all, lv_color_hex(C_STOP), 0);
    lv_obj_center(label(all, &lv_font_montserrat_20, lv_color_white(), ""));
    setDelText(lv_obj_get_child(all, 0), true, DEL_IDLE);
    lv_obj_add_event_cb(all, onDelete, LV_EVENT_CLICKED, (void *)(intptr_t)DEL_ALL);
  }
  updateTotals();
}

// ---- lifecycle -------------------------------------------------------------------------
void freeTrips() {
  if (!s_trips) return;
  for (int i = 0; i < MAX_TRIPS; i++) s_trips[i].~Summary();
  heap_caps_free(s_trips);
  s_trips = nullptr;
  s_tripCount = 0;
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  s_trips = (TripRecorder::Summary *)heap_caps_malloc(sizeof(TripRecorder::Summary) * MAX_TRIPS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (s_trips) for (int i = 0; i < MAX_TRIPS; i++) new (&s_trips[i]) TripRecorder::Summary();
  Apps::buildMenu(content, PAGE_NAME, P_COUNT, s_ui.menu, s_ui.page, onMenu);
  buildRecord(s_ui.page[P_RECORD]);
  buildTrips(s_ui.page[P_TRIPS]);
  buildTotals(s_ui.page[P_TOTALS]);
  rebuildList();
  s_ui.wasRecording = TripRecorder::recording();
  updateRecord();
  showPage(s_page);
  if (s_openTrip >= 0) { openDetail(s_openTrip); s_openTrip = -1; }
}

void update() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  updateDelete();
  if (s_page == P_RECORD) updateRecord();
  const bool rec = TripRecorder::recording();
  if (s_ui.wasRecording && !rec) s_ui.listPending = true;      // a trip was just stopped ...
  if (s_ui.listPending && !TripRecorder::saving()) {           // ... list it once its files are on the card
    s_ui.listPending = false;
    if (!s_ui.detail) rebuildList();
  }
  s_ui.wasRecording = rec;
}

void destroy() {
  s_delStep = -1;
  s_delArmed = -1;
  freeRoute();
  freeTrips();
  s_ui = Ui();
}

}  // namespace

extern const AppImpl TRIPS_APP = { "Trips", create, update, destroy };

// Console / screenshots: the page the app opens on next ("page Trips <0..2>");
// 10 + i opens trip i of the list (detail).
void tripsAppSetPage(int page) {
  s_openTrip = page >= 10 ? page - 10 : -1;
  s_page = page >= 10 ? P_TRIPS : constrain(page, 0, P_COUNT - 1);
}
