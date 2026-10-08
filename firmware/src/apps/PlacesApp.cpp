// =============================================================================
//  Places app: saved points. "Save here" stores the current GPS position under a name; the list is
//  sorted by distance (from the position, or the last known one) and shows how far and in which
//  direction each place is. Tap a place: Go to (Navigator), Show on map, Rename, Delete.
//  Places can also be added from the Map app (long press) and from the phone page.
//  The list itself is services/Places. Product UI only.
// =============================================================================
#include "App.h"
#include "../shell/Shell.h"

#include <Arduino.h>
#include "../services/Geo.h"
#include "../services/Location.h"
#include "../services/Navigator.h"
#include "../services/Places.h"
#include "../services/Settings.h"
#include "../ui/NavUi.h"
#include "../ui/UiText.h"

namespace {

constexpr uint32_t C_SAVE = 0x2E7D32, C_GO = 0x1E88E5, C_DEL = 0xC62828, C_PIN = 0xFFCA28;

struct Ui {
  lv_obj_t *save = nullptr, *count = nullptr, *hint = nullptr, *list = nullptr;
  lv_obj_t *detail = nullptr, *dName = nullptr, *dInfo = nullptr, *dDist = nullptr, *dDel = nullptr, *dDelLbl = nullptr;
  lv_obj_t *rowDist[Places::CAPACITY] = {};
};
Ui s_ui;
int s_order[Places::CAPACITY];                // list row -> place index
int s_rows = 0;
uint32_t s_listRev = 0;
int s_open = -1;                         // place shown in the detail view
bool s_delArmed = false;
uint32_t s_delArmedMs = 0;
int s_openNext = -1;                     // console: open this place's detail on the next create()

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

// Reference position for distances: the GPS position, else the last known one
bool here(double &lat, double &lon, bool *live = nullptr) {
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix && d.locValid;
  if (live) *live = fix;
  if (fix) { lat = d.lat; lon = d.lng; return true; }
  return Settings::lastPos(lat, lon);
}

void distanceText(const Places::Place &p, char *out, size_t n) {
  double lat, lon;
  if (!here(lat, lon)) { snprintf(out, n, "%.5f, %.5f", p.lat, p.lon); return; }
  char d[24];
  Geo::formatDistance(Geo::distanceM(lat, lon, p.lat, p.lon), d, sizeof(d));
  snprintf(out, n, "%s  %s", d, Geo::cardinal(Geo::bearingDeg(lat, lon, p.lat, p.lon)));
}

void openDetail(int i);
void rebuildList();

void onRow(lv_event_t *e) {
  const int row = (int)(intptr_t)lv_event_get_user_data(e);
  if (row >= 0 && row < s_rows) openDetail(s_order[row]);
}

void rebuildList() {
  s_listRev = Places::revision();
  lv_obj_clean(s_ui.list);
  s_rows = Places::count();
  double lat = 0, lon = 0;
  const bool have = here(lat, lon);
  float dist[Places::CAPACITY];
  for (int i = 0; i < s_rows; i++) {
    s_order[i] = i;
    Places::Place p;
    Places::get(i, p);
    dist[i] = have ? (float)Geo::distanceM(lat, lon, p.lat, p.lon) : 0;
  }
  if (have)                                                  // nearest first (insertion sort: <= 50 entries)
    for (int i = 1; i < s_rows; i++)
      for (int j = i; j > 0 && dist[s_order[j]] < dist[s_order[j - 1]]; j--) { const int t = s_order[j]; s_order[j] = s_order[j - 1]; s_order[j - 1] = t; }
  lv_label_set_text_fmt(s_ui.count, "%d / %d", s_rows, Places::CAPACITY);
  if (!s_rows) {
    lv_obj_t *l = label(s_ui.list, &lv_font_montserrat_20, 0xBBBBBB,
                        "No places yet.\n\nSave here: the current GPS position.\nMap: long press a point.\nPhone page: Places.");
    lv_obj_set_width(l, lv_pct(100));
  }
  for (int r = 0; r < s_rows; r++) {
    Places::Place p;
    Places::get(s_order[r], p);
    lv_obj_t *b = lv_button_create(s_ui.list);
    lv_obj_set_size(b, lv_pct(100), 64);
    lv_obj_add_event_cb(b, onRow, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    lv_obj_t *pin = label(b, &lv_font_montserrat_20, C_PIN, LV_SYMBOL_HOME);
    lv_obj_align(pin, LV_ALIGN_LEFT_MID, -4, 0);
    lv_obj_t *n = label(b, &nav_he_20, 0xFFFFFF);
    lv_obj_set_width(n, 330);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    UiText::setName(n, p.name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 30, -6);
    char s[48];
    distanceText(p, s, sizeof(s));
    s_ui.rowDist[r] = label(b, &lv_font_montserrat_16, 0xE0E0E0, s);
    lv_obj_align(s_ui.rowDist[r], LV_ALIGN_BOTTOM_LEFT, 30, 6);
    lv_obj_align(label(b, &lv_font_montserrat_20, 0xFFFFFF, LV_SYMBOL_RIGHT), LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

// ---- detail --------------------------------------------------------------------------------
void closeDetail() {
  if (s_ui.detail) { lv_obj_delete_async(s_ui.detail); s_ui.detail = nullptr; }   // async: called from its own buttons
  s_open = -1;
  lv_obj_set_hidden(s_ui.list, false);
  lv_obj_set_hidden(s_ui.save, false);
  lv_obj_set_hidden(s_ui.count, false);
  lv_obj_set_hidden(s_ui.hint, false);
}

void onBack(lv_event_t *) { closeDetail(); }

void onGo(lv_event_t *) {
  Places::Place p;
  if (!Places::get(s_open, p)) return;
  Navigator::goTo(p.name, p.lat, p.lon);
  Shell::switchApp("Navigate");
}

void onShowMap(lv_event_t *) {
  Places::Place p;
  if (!Places::get(s_open, p)) return;
  mapAppShowPlace(p.lat, p.lon);
  Shell::switchApp("Map");
}

void renamed(const char *text) {
  if (!text || s_open < 0) return;
  if (!Places::rename(s_open, text)) return;
  Places::Place p;
  if (Places::get(s_open, p) && s_ui.dName) UiText::setName(s_ui.dName, p.name);
}

void onRename(lv_event_t *e) {
  Places::Place p;
  if (!Places::get(s_open, p)) return;
  NavUi::keyboard(lv_event_get_target_obj(e), "Name of the place", p.name, Places::NAME_BYTES, renamed);
}

void onDelete(lv_event_t *) {
  if (!s_delArmed) {                                         // first tap arms, the second deletes
    s_delArmed = true;
    s_delArmedMs = millis();
    lv_label_set_text(s_ui.dDelLbl, LV_SYMBOL_TRASH "  Tap again");
    return;
  }
  Places::remove(s_open);
  closeDetail();
  rebuildList();
}

void openDetail(int i) {
  Places::Place p;
  if (!Places::get(i, p)) return;
  closeDetail();
  s_open = i;
  s_delArmed = false;
  lv_obj_t *content = lv_obj_get_parent(s_ui.list);
  lv_obj_set_hidden(s_ui.list, true);
  lv_obj_set_hidden(s_ui.save, true);
  lv_obj_set_hidden(s_ui.count, true);
  lv_obj_set_hidden(s_ui.hint, true);
  lv_obj_t *d = Apps::card(content, 12, 0, Apps::PAGE_W, Apps::CONTENT_H - 12);
  s_ui.detail = d;
  lv_obj_t *back = button(d, LV_SYMBOL_LEFT " All", 0x37474F, 130, 44, onBack);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 0);
  s_ui.dName = label(d, &nav_he_28, 0xFFFFFF);
  lv_obj_set_width(s_ui.dName, Apps::PAGE_INNER_W - 140);
  lv_label_set_long_mode(s_ui.dName, LV_LABEL_LONG_DOT);
  UiText::setName(s_ui.dName, p.name);
  lv_obj_set_pos(s_ui.dName, 4, 6);
  s_ui.dDist = label(d, &lv_font_montserrat_48, 0xFFFFFF, "--");
  lv_obj_set_pos(s_ui.dDist, 4, 70);
  s_ui.dInfo = label(d, &lv_font_montserrat_20, 0xBBBBBB);
  lv_obj_set_pos(s_ui.dInfo, 4, 136);
  lv_obj_set_width(s_ui.dInfo, Apps::PAGE_INNER_W - 8);
  const int bw = Apps::PAGE_INNER_W;
  lv_obj_set_pos(button(d, LV_SYMBOL_UP "  Go to", C_GO, bw, 72, onGo), 0, 240);
  lv_obj_set_pos(button(d, LV_SYMBOL_IMAGE "  Show on map", 0x00897B, bw, 60, onShowMap), 0, 326);
  lv_obj_set_pos(button(d, LV_SYMBOL_EDIT "  Rename", 0x37474F, (bw - 12) / 2, 60, onRename), 0, 400);
  s_ui.dDel = button(d, LV_SYMBOL_TRASH "  Delete", C_DEL, (bw - 12) / 2, 60, onDelete);
  lv_obj_set_pos(s_ui.dDel, (bw - 12) / 2 + 12, 400);
  s_ui.dDelLbl = lv_obj_get_child(s_ui.dDel, 0);
  lv_obj_t *hint = label(d, &lv_font_montserrat_14, 0xBBBBBB,
                         "Go to: the arrow and the distance in Navigate, on the Map and the Compass. Straight line - there is no road routing on the device.");
  lv_obj_set_width(hint, bw);
  lv_obj_set_pos(hint, 0, 480);
}

void updateDetail() {
  Places::Place p;
  if (!Places::get(s_open, p)) { closeDetail(); return; }
  double lat, lon;
  bool live = false;
  char b[96], d[24];
  if (here(lat, lon, &live)) {
    Geo::formatDistance(Geo::distanceM(lat, lon, p.lat, p.lon), d, sizeof(d));
    Apps::setText(s_ui.dDist, d);
    lv_obj_set_style_text_opa(s_ui.dDist, live ? LV_OPA_COVER : LV_OPA_50, 0);
    const double br = Geo::bearingDeg(lat, lon, p.lat, p.lon);
    snprintf(b, sizeof(b), "%s %03.0f\xC2\xB0%s\n%.6f, %.6f", Geo::cardinal(br), br, live ? "" : "  (last known position)", p.lat, p.lon);
  } else {
    Apps::setText(s_ui.dDist, "--");
    snprintf(b, sizeof(b), "No GPS position yet\n%.6f, %.6f", p.lat, p.lon);
  }
  Apps::setText(s_ui.dInfo, b);
  if (s_delArmed && millis() - s_delArmedMs > 4000) {        // not confirmed: disarm
    s_delArmed = false;
    lv_label_set_text(s_ui.dDelLbl, LV_SYMBOL_TRASH "  Delete");
  }
}

// ---- save here -----------------------------------------------------------------------------
double s_saveLat = 0, s_saveLon = 0;

void saved(const char *text) {
  if (!text) return;
  String name = text;
  name.trim();
  if (!name.length()) name = Places::defaultName();
  const int i = Places::add(name.c_str(), s_saveLat, s_saveLon);
  if (i < 0) { Apps::setText(s_ui.hint, Places::count() >= Places::CAPACITY ? "The list is full - delete a place first." : "Could not save the place."); return; }
  rebuildList();
}

void onSave(lv_event_t *e) {
  const GpsData d = Location::snapshot();
  if (Location::quality(d) != Location::Quality::Fix || !d.locValid) { Apps::setText(s_ui.hint, "Save here needs a GPS fix."); return; }
  if (Places::count() >= Places::CAPACITY) { Apps::setText(s_ui.hint, "The list is full - delete a place first."); return; }
  s_saveLat = d.lat; s_saveLon = d.lng;                      // the position at the tap, not after typing
  NavUi::keyboard(lv_event_get_target_obj(e), "Save this position as", Places::defaultName().c_str(), Places::NAME_BYTES, saved);
}

// ---- lifecycle -------------------------------------------------------------------------------
void create(lv_obj_t *content) {
  s_ui = Ui();
  s_open = -1;
  s_ui.save = button(content, LV_SYMBOL_PLUS "  Save here", C_SAVE, 300, 60, onSave);
  lv_obj_set_pos(s_ui.save, 12, 0);
  s_ui.count = label(content, &lv_font_montserrat_20, 0xDDDDDD);
  lv_obj_align(s_ui.count, LV_ALIGN_TOP_RIGHT, -16, 18);
  s_ui.hint = label(content, &lv_font_montserrat_16, 0xFFB300);
  lv_obj_set_pos(s_ui.hint, 16, 68);
  lv_obj_set_width(s_ui.hint, Apps::CONTENT_W - 32);
  s_ui.list = lv_obj_create(content);
  lv_obj_remove_style_all(s_ui.list);
  lv_obj_set_pos(s_ui.list, 12, 96);
  lv_obj_set_size(s_ui.list, Apps::PAGE_W, Apps::CONTENT_H - 108);
  lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ui.list, 8, 0);
  lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
  rebuildList();
  if (s_openNext >= 0) { openDetail(s_openNext); s_openNext = -1; }
}

void update() {
  static uint32_t last = 0;
  if (NavUi::keyboardOpen() || millis() - last < 1000) return;
  last = millis();
  if (s_ui.detail) { updateDetail(); return; }
  if (Places::revision() != s_listRev) { rebuildList(); return; }
  for (int r = 0; r < s_rows; r++) {                         // distances move with you; the order is kept until the list changes
    Places::Place p;
    if (!Places::get(s_order[r], p) || !s_ui.rowDist[r]) continue;
    char s[48];
    distanceText(p, s, sizeof(s));
    Apps::setText(s_ui.rowDist[r], s);
  }
}

void destroy() {
  NavUi::keyboardClose();
  s_ui = Ui();
  s_open = -1;
}

}  // namespace

extern const AppImpl PLACES_APP = { "Places", create, update, destroy };

// Console / screenshots: open place i's detail the next time the app opens (-1 = the list)
void placesAppOpen(int i) { s_openNext = i; }
