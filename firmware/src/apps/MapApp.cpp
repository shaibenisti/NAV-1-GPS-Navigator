// =============================================================================
//  Map app: the offline street map (/maps/israel.pmtiles on the SD card) on the screen, north up, with
//  zoom buttons, a scale bar, the position marker and the recording trip drawn on it. The view follows
//  the GPS position; dragging the map frees it (a button brings it back to the position). Without a GPS
//  fix the view opens at the last known position. Rendering happens in the MapRender task; this file is
//  the UI only. The picture is re-rendered when the view leaves the middle of it or the zoom changes; in
//  between only the marker moves. Product UI only.
// =============================================================================
#include "App.h"

#include <Arduino.h>
#include <math.h>
#include "../services/Location.h"
#include "../services/MapRender.h"
#include "../services/Settings.h"
#include "../services/TripRecorder.h"

namespace {

constexpr const char *MAP_FILE = "/maps/israel.pmtiles";
constexpr float ZOOMS[] = { 13.0f, 14.0f, 15.0f, 15.5f, 16.0f, 16.5f, 17.0f, 17.5f };
constexpr int N_ZOOM = sizeof(ZOOMS) / sizeof(ZOOMS[0]);
constexpr int RECENTRE_DX = 120, RECENTRE_DY = 170;          // px from the picture's centre that trigger a new render
constexpr int MARK = 44;                                      // marker object size
constexpr int DRAG_MIN = 12;                                  // px before a touch counts as a drag
constexpr uint32_t DRAG_UPDATE_MS = 120;                      // the picture follows the finger at most this often (full redraw ~0.2 s)
constexpr uint32_t TRACK_REFRESH_MS = 15000;

struct Ui {
  lv_obj_t *img = nullptr, *marker = nullptr, *status = nullptr, *scaleBar = nullptr, *scaleLbl = nullptr, *speed = nullptr;
  lv_obj_t *zin = nullptr, *zout = nullptr, *follow = nullptr, *mode = nullptr, *modeLbl = nullptr;
};
Ui s_ui;
lv_image_dsc_t s_dsc;
MapRender::Req s_shown;                 // what the displayed picture was rendered for
bool s_haveShown = false, s_started = false;
int s_zoomIdx = 3;                      // 15.5
double s_lon = 0, s_lat = 0;            // latest GPS position
bool s_havePos = false;
bool s_follow = true;
bool s_headingUp = false;               // picture turned so the direction of travel is up
bool s_showTrip = false;                // a finished trip is shown (Trips > Show on map): its track stays, the view is free
double s_vLon = 0, s_vLat = 0;          // view centre when not following (or without GPS)
bool s_haveView = false;
float s_heading = 0;
float s_speedKmh = 0;
uint32_t s_lastReqMs = 0, s_lastTrackMs = 0, s_lastSaveMs = 0;
// drag
bool s_pressed = false, s_dragging = false;
int s_startX = 0, s_startY = 0, s_dragX = 0, s_dragY = 0;
uint32_t s_dragMs = 0;

void onDrawMarker(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.radius = LV_RADIUS_CIRCLE;
  rd.bg_color = lv_color_hex(0x1E88E5);
  rd.bg_opa = LV_OPA_COVER;
  rd.border_color = lv_color_white();
  rd.border_width = 3;
  lv_area_t ca = { (int32_t)(cx - 16), (int32_t)(cy - 16), (int32_t)(cx + 16), (int32_t)(cy + 16) };
  lv_draw_rect(layer, &rd, &ca);
  lv_draw_triangle_dsc_t td;                                // heading arrow on the disc
  lv_draw_triangle_dsc_init(&td);
  td.color = lv_color_white();
  td.opa = LV_OPA_COVER;
  const float th = (s_heading - (s_headingUp && s_haveShown && s_shown.headingUp ? s_shown.heading : 0.0f)) * (float)M_PI / 180.0f;
  const float pts[3][2] = { { 0, -12 }, { -8, 8 }, { 8, 8 } };
  for (int i = 0; i < 3; i++) {
    td.p[i].x = (int32_t)lroundf(cx + pts[i][0] * cosf(th) - pts[i][1] * sinf(th));
    td.p[i].y = (int32_t)lroundf(cy + pts[i][0] * sinf(th) + pts[i][1] * cosf(th));
  }
  lv_draw_triangle(layer, &td);
}

// The recording trip as an overlay of the next picture
void refreshTrack() {
  s_lastTrackMs = millis();
  if (s_showTrip) return;                              // a finished trip is already set
  if (!TripRecorder::recording()) { MapRender::setTrack(nullptr, nullptr, 0); return; }
  float *lat = (float *)heap_caps_malloc(sizeof(float) * 1200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // not static: internal RAM is tight
  if (!lat) return;
  float *lon = lat + 600;
  const int n = TripRecorder::track(Settings::tripPath(), lat, lon, 600);
  MapRender::setTrack(lat, lon, n > 1 ? n : 0);
  heap_caps_free(lat);
}

// Position the next picture is centred on. Following in a car: a little ahead of the position (the direction of
// travel), so the picture lasts longer before it has to be rendered again.
void viewCentre(double &lon, double &lat) {
  if (!(s_follow && s_havePos)) { lon = s_vLon; lat = s_vLat; return; }
  lon = s_lon; lat = s_lat;
  if (s_headingUp || s_speedKmh < 8) return;
  const double mpp = MapRender::metersPerPixel(s_lat, ZOOMS[s_zoomIdx]);
  const double ahead = fmin(100.0, s_speedKmh / 3.6 * 10.0 / mpp) * mpp;          // up to 100 px, ~10 s of travel
  const double th = s_heading * M_PI / 180.0;
  lat += ahead * cos(th) / 110540.0;
  lon += ahead * sin(th) / (111320.0 * cos(s_lat * M_PI / 180.0));
}

void requestRender() {
  if (!s_haveView || !MapRender::running()) return;
  refreshTrack();
  MapRender::Req r;
  viewCentre(r.lon, r.lat);
  r.zoom = ZOOMS[s_zoomIdx];
  r.headingUp = s_headingUp && s_follow && s_havePos;
  r.heading = s_heading;
  if (r.headingUp) { r.lon = s_lon; r.lat = s_lat; }
  MapRender::request(r);
  s_lastReqMs = millis();
}

void onZoom(lv_event_t *e) {
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  const int z = constrain(s_zoomIdx + d, 0, N_ZOOM - 1);
  if (z == s_zoomIdx) return;
  s_zoomIdx = z;
  requestRender();
}

void onFollow(lv_event_t *) {
  if (!s_havePos) return;
  s_follow = true;
  requestRender();
}

void onMode(lv_event_t *) {
  s_headingUp = !s_headingUp;
  if (s_headingUp && !s_follow && s_havePos) s_follow = true;      // heading up always follows the position
  Apps::setText(s_ui.modeLbl, s_headingUp ? LV_SYMBOL_UP : "N");
  requestRender();
}

void onImg(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *in = lv_indev_active();
  if (!in) return;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  if (code == LV_EVENT_PRESSED) {
    s_pressed = true; s_dragging = false;
    s_startX = p.x; s_startY = p.y; s_dragX = s_dragY = 0;
  } else if (code == LV_EVENT_PRESSING && s_pressed && s_haveShown && !(s_headingUp && s_follow)) {
    const int dx = p.x - s_startX, dy = p.y - s_startY;
    if (!s_dragging && abs(dx) + abs(dy) < DRAG_MIN) return;
    s_dragging = true;
    s_dragX = dx; s_dragY = dy;
    if (millis() - s_dragMs >= DRAG_UPDATE_MS) {              // move the picture (a full redraw: not on every touch sample)
      s_dragMs = millis();
      lv_obj_set_pos(s_ui.img, dx, dy);
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (s_pressed && s_dragging && s_haveShown && !s_shown.headingUp) {
      lv_obj_set_pos(s_ui.img, s_dragX, s_dragY);
      double lon, lat;
      MapRender::positionAt(s_shown, -s_dragX, -s_dragY, lon, lat);   // the picture moved right = the view centre moved left
      s_vLon = lon; s_vLat = lat; s_haveView = true;
      s_follow = false;
      requestRender();
    }
    s_pressed = false; s_dragging = false;
  }
}

String s_pendingTrip;                    // base path of a trip to show (set by mapAppShowTrip before the app opens)

// Shows a finished trip: its track is drawn and the view is fitted to it
void showTrip(const String &base) {
  float *buf = (float *)heap_caps_malloc(sizeof(float) * 1200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) return;
  float *lat = buf, *lon = buf + 600;
  const int n = TripRecorder::track(base, lat, lon, 600);
  if (n >= 2) {
    float la0 = lat[0], la1 = lat[0], lo0 = lon[0], lo1 = lon[0];
    for (int i = 1; i < n; i++) { la0 = fminf(la0, lat[i]); la1 = fmaxf(la1, lat[i]); lo0 = fminf(lo0, lon[i]); lo1 = fmaxf(lo1, lon[i]); }
    s_vLat = (la0 + la1) / 2; s_vLon = (lo0 + lo1) / 2; s_haveView = true;
    const double wM = (lo1 - lo0) * 111320.0 * cos(s_vLat * M_PI / 180.0), hM = (la1 - la0) * 110540.0;
    int z = 0;                                                  // the closest zoom at which the whole trip fits the picture
    for (int i = N_ZOOM - 1; i >= 0; i--) {
      const double mpp = MapRender::metersPerPixel(s_vLat, ZOOMS[i]);
      if (wM / mpp <= 400 && hM / mpp <= 590) { z = i; break; }
    }
    s_zoomIdx = z;
    s_follow = false;
    s_showTrip = true;
    MapRender::setTrack(lat, lon, n);
  }
  heap_caps_free(buf);
}

lv_obj_t *button(lv_obj_t *p, const char *text, int x, int y, lv_event_cb_t cb, void *user) {
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_size(b, 64, 64);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  return b;
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  s_haveShown = false;
  s_started = false;
  s_pressed = s_dragging = false;
  lv_obj_set_style_bg_color(content, lv_color_hex(0x121620), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(content, true, 0);
  lv_obj_set_scrollable(content, false);
  s_ui.img = lv_image_create(content);
  lv_obj_set_pos(s_ui.img, 0, 0);
  lv_obj_set_size(s_ui.img, MapRender::W, MapRender::H);
  lv_obj_set_clickable(s_ui.img, true);
  lv_obj_set_scrollable(s_ui.img, false);
  lv_obj_add_event_cb(s_ui.img, onImg, LV_EVENT_ALL, nullptr);

  s_ui.marker = lv_obj_create(content);
  lv_obj_remove_style_all(s_ui.marker);
  lv_obj_set_size(s_ui.marker, MARK, MARK);
  lv_obj_set_clickable(s_ui.marker, false);
  lv_obj_add_event_cb(s_ui.marker, onDrawMarker, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_set_hidden(s_ui.marker, true);

  s_ui.status = lv_label_create(content);
  lv_obj_set_style_text_font(s_ui.status, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_ui.status, lv_color_white(), 0);
  lv_obj_set_style_bg_color(s_ui.status, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_ui.status, LV_OPA_60, 0);
  lv_obj_set_style_pad_all(s_ui.status, 6, 0);
  lv_obj_set_width(s_ui.status, 440);
  lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 10);
  lv_label_set_text(s_ui.status, "Loading map...");

  s_ui.zin = button(content, "+", 404, 458, onZoom, (void *)(intptr_t)1);
  s_ui.zout = button(content, LV_SYMBOL_MINUS, 404, 534, onZoom, (void *)(intptr_t)-1);
  s_ui.follow = button(content, LV_SYMBOL_GPS, 12, 534, onFollow, nullptr);
  lv_obj_set_hidden(s_ui.follow, true);
  s_ui.mode = button(content, "N", 12, 458, onMode, nullptr);
  s_ui.modeLbl = lv_obj_get_child(s_ui.mode, 0);

  s_ui.scaleBar = lv_obj_create(content);
  lv_obj_remove_style_all(s_ui.scaleBar);
  lv_obj_set_style_bg_color(s_ui.scaleBar, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_ui.scaleBar, LV_OPA_COVER, 0);
  lv_obj_set_size(s_ui.scaleBar, 80, 4);
  lv_obj_set_pos(s_ui.scaleBar, 16, 668);
  s_ui.scaleLbl = lv_label_create(content);
  lv_obj_set_style_text_font(s_ui.scaleLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_ui.scaleLbl, lv_color_white(), 0);
  lv_obj_set_style_bg_color(s_ui.scaleLbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_ui.scaleLbl, LV_OPA_50, 0);
  lv_obj_set_pos(s_ui.scaleLbl, 16, 640);
  lv_label_set_text(s_ui.scaleLbl, "");

  s_ui.speed = lv_label_create(content);
  lv_obj_set_style_text_font(s_ui.speed, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(s_ui.speed, lv_color_white(), 0);
  lv_obj_set_style_bg_color(s_ui.speed, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_ui.speed, LV_OPA_50, 0);
  lv_obj_set_style_pad_all(s_ui.speed, 4, 0);
  lv_obj_align(s_ui.speed, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
  lv_label_set_text(s_ui.speed, "");

  // the view starts at the GPS position, else at the last known one
  s_follow = true;
  s_havePos = false;
  s_headingUp = false;
  s_showTrip = false;
  s_haveView = Settings::lastPos(s_vLat, s_vLon);
  s_started = MapRender::begin(MAP_FILE);
  if (!s_started) lv_label_set_text(s_ui.status, "Map unavailable (out of memory)");
  else if (s_pendingTrip.length()) { showTrip(s_pendingTrip); s_pendingTrip = ""; }
  s_lastSaveMs = millis();
}

void updateScale(double lat) {
  const double mpp = MapRender::metersPerPixel(lat, ZOOMS[s_zoomIdx]);
  static const int STEPS[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000 };
  int pick = STEPS[0];
  for (int s : STEPS) { pick = s; if (s / mpp >= 70) break; }
  const int px = (int)lround(pick / mpp);
  lv_obj_set_width(s_ui.scaleBar, constrain(px, 10, 200));
  char b[24];
  if (pick >= 1000) snprintf(b, sizeof(b), "%d km", pick / 1000);
  else snprintf(b, sizeof(b), "%d m", pick);
  Apps::setText(s_ui.scaleLbl, b);
}

void update() {
  if (!s_started) return;
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix && d.locValid;
  if (fix) {
    s_lon = d.lng; s_lat = d.lat; s_havePos = true;
    if (!s_haveView) { s_vLon = d.lng; s_vLat = d.lat; s_haveView = true; }
    if (d.courseValid && d.speedValid && d.speedKmh >= 2) s_heading = (float)d.courseDeg;
    s_speedKmh = d.speedValid ? (float)d.speedKmh : 0.0f;
    if (millis() - s_lastSaveMs > 30000) {                      // remember where we were: the view without a fix next time
      s_lastSaveMs = millis();
      double pl, po;
      if (!Settings::lastPos(pl, po) || fabs(pl - d.lat) > 0.002 || fabs(po - d.lng) > 0.002) Settings::setLastPos(d.lat, d.lng);
    }
  }
  char b[32];
  if (fix && d.speedValid) snprintf(b, sizeof(b), "%.0f km/h", d.speedKmh);
  else b[0] = 0;
  Apps::setText(s_ui.speed, b);
  lv_obj_set_hidden(s_ui.follow, s_follow || !s_havePos);

  // a finished picture?
  const uint16_t *frame = nullptr;
  MapRender::Req rq;
  if (MapRender::poll(&frame, &rq)) {
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.flags = 0;
    s_dsc.header.w = MapRender::W;
    s_dsc.header.h = MapRender::H;
    s_dsc.header.stride = MapRender::W * 2;
    s_dsc.data_size = MapRender::W * MapRender::H * 2;
    s_dsc.data = (const uint8_t *)frame;
    lv_obj_set_pos(s_ui.img, 0, 0);                             // a dragged picture snaps to its new place
    lv_image_set_src(s_ui.img, &s_dsc);
    s_shown = rq;
    s_haveShown = true;
  }

  const MapRender::Status st = MapRender::status();
  // when to render again
  if (s_haveView && st.mapOk && !st.busy && !s_dragging && millis() - s_lastReqMs > 600) {
    bool need = !s_haveShown;
    if (s_haveShown) {
      if (s_shown.zoom != ZOOMS[s_zoomIdx]) need = true;
      else if (s_follow && s_havePos) {
        int x, y;
        MapRender::pixelOf(s_shown, s_lon, s_lat, x, y);
        const int cx = MapRender::W / 2, cy = s_shown.headingUp ? MapRender::H / 2 + 130 : MapRender::H / 2;
        if (s_shown.headingUp) {
          float dh = fabsf(s_heading - s_shown.heading);
          if (dh > 180) dh = 360 - dh;
          need = abs(x - cx) > 70 || abs(y - cy) > 90 || (dh > 12 && millis() - s_lastReqMs > 2500);
        } else {
          need = abs(x - cx) > RECENTRE_DX || abs(y - cy) > RECENTRE_DY;
        }
      }
      if (!need && TripRecorder::recording() && s_follow && !s_showTrip && millis() - s_lastTrackMs > TRACK_REFRESH_MS) need = true;   // the trace grows
    }
    if (need) requestRender();
  }
  if (s_haveShown) {                                            // marker follows the position over the picture
    bool in = false;
    if (s_havePos) {
      int px, py;
      MapRender::pixelOf(s_shown, s_lon, s_lat, px, py);
      const int x = px - MARK / 2 + lv_obj_get_x(s_ui.img), y = py - MARK / 2 + lv_obj_get_y(s_ui.img);
      in = x > -MARK && x < MapRender::W && y > -MARK && y < MapRender::H;
      if (in) { lv_obj_set_pos(s_ui.marker, x, y); lv_obj_invalidate(s_ui.marker); }
    }
    lv_obj_set_hidden(s_ui.marker, !in);
    updateScale(s_shown.lat);
  }

  if (!st.mapOk) {
    if (st.error[0]) { char m[96]; snprintf(m, sizeof(m), "No map on the card\n(%s)", st.error); Apps::setText(s_ui.status, m); }
    return;
  }
  if (!s_haveView) Apps::setText(s_ui.status, "Waiting for a GPS position...");
  else if (!s_haveShown) Apps::setText(s_ui.status, st.error[0] ? st.error : "Loading map...");
  else if (st.busy) Apps::setText(s_ui.status, "Updating map...");
  else Apps::setText(s_ui.status, "");
  lv_obj_set_hidden(s_ui.status, lv_label_get_text(s_ui.status)[0] == 0);
}

void destroy() {
  if (s_started) MapRender::end();                              // frees the picture buffers and tile caches (~2 MB PSRAM)
  MapRender::setTrack(nullptr, nullptr, 0);
  s_started = false;
  s_ui = Ui();
}

}  // namespace

void mapAppShowTrip(const char *base) { s_pendingTrip = base; }

void mapAppCommand(const char *arg) {
  if (!s_started) return;
  if (!strcmp(arg, "up")) { s_headingUp = true; s_follow = true; Apps::setText(s_ui.modeLbl, LV_SYMBOL_UP); }
  else if (!strcmp(arg, "north")) { s_headingUp = false; Apps::setText(s_ui.modeLbl, "N"); }
  else if (!strncmp(arg, "zoom ", 5)) s_zoomIdx = constrain(atoi(arg + 5), 0, N_ZOOM - 1);
  requestRender();
}

extern const AppImpl MAP_APP = { "Map", create, update, destroy };
