// =============================================================================
//  Compass app: heading while moving. The device has no magnetic sensor, so the heading is the GPS
//  course over ground - valid only while the device actually moves (>= 3 km/h to start, held down
//  to 1.5 km/h); standing still the dial keeps the last heading, dimmed, and says so. Heading-up dial
//  (the direction of travel is always at the top), north marked in red. Product UI only.
// =============================================================================
#include "App.h"

#include <Arduino.h>
#include <math.h>
#include "../services/Location.h"

namespace {

constexpr int DIAL = 456;                 // dial object size (square)
constexpr int R = DIAL / 2 - 6;           // ring radius
constexpr float START_KMH = 3.0f, HOLD_KMH = 1.5f;
constexpr uint32_t C_NORTH = 0xE53935, C_POINTER = 0xFFB300, C_RING = 0x9FA8DA;

struct Ui {
  lv_obj_t *heading = nullptr, *hint = nullptr, *dial = nullptr, *speed = nullptr, *unit = nullptr, *alt = nullptr, *sats = nullptr;
};
Ui s_ui;
float s_heading = 0;        // shown heading, degrees (smoothed)
bool s_moving = false;      // heading currently live
bool s_seen = false;        // a heading was ever shown

const char *cardinal(float deg) {
  static const char *const N[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  return N[(int)((deg + 22.5f) / 45.0f) & 7];
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

// Point on the dial for a compass bearing `bearing` (degrees), at radius r; the dial turns so that the
// heading is up.
void polar(int cx, int cy, float bearing, float r, int &x, int &y) {
  const float th = (bearing - s_heading) * (float)M_PI / 180.0f;
  x = cx + (int)lroundf(r * sinf(th));
  y = cy - (int)lroundf(r * cosf(th));
}

void onDraw(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
  const lv_opa_t live = s_moving ? LV_OPA_COVER : LV_OPA_50;       // standing still: last heading, dimmed

  lv_draw_rect_dsc_t disc;
  lv_draw_rect_dsc_init(&disc);
  disc.radius = LV_RADIUS_CIRCLE;
  disc.bg_color = lv_color_black();
  disc.bg_opa = LV_OPA_30;
  disc.border_color = lv_color_hex(C_RING);
  disc.border_width = 3;
  disc.border_opa = live;
  lv_area_t da = { cx - R, cy - R, cx + R, cy + R };
  lv_draw_rect(layer, &disc, &da);

  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.opa = live;
  for (int b = 0; b < 360; b += 10) {
    const bool major = b % 30 == 0;
    int x1, y1, x2, y2;
    polar(cx, cy, (float)b, R - 4, x1, y1);
    polar(cx, cy, (float)b, R - (major ? 26 : 14), x2, y2);
    ln.color = b == 0 ? lv_color_hex(C_NORTH) : lv_color_white();
    ln.width = major ? 4 : 2;
    ln.p1.x = x1; ln.p1.y = y1; ln.p2.x = x2; ln.p2.y = y2;
    lv_draw_line(layer, &ln);
  }

  static const char *const NAME[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  lv_draw_label_dsc_t ld;
  lv_draw_label_dsc_init(&ld);
  ld.align = LV_TEXT_ALIGN_CENTER;
  ld.opa = live;
  ld.text_static = 1;
  for (int i = 0; i < 8; i++) {
    const bool main = (i & 1) == 0;
    int x, y;
    polar(cx, cy, i * 45.0f, R - (main ? 88 : 80), x, y);
    ld.font = main ? &lv_font_montserrat_28 : &lv_font_montserrat_16;
    ld.color = i == 0 ? lv_color_hex(C_NORTH) : lv_color_white();
    ld.text = NAME[i];
    const int hh = main ? 18 : 11;
    lv_area_t la = { x - 30, y - hh, x + 30, y + hh };
    lv_draw_label(layer, &ld, &la);
  }

  // Direction of travel: fixed marker at the top of the ring
  lv_draw_triangle_dsc_t tri;
  lv_draw_triangle_dsc_init(&tri);
  tri.color = lv_color_hex(C_POINTER);
  tri.opa = live;
  tri.p[0].x = cx;      tri.p[0].y = cy - R + 30;
  tri.p[1].x = cx - 14; tri.p[1].y = cy - R + 62;
  tri.p[2].x = cx + 14; tri.p[2].y = cy - R + 62;
  lv_draw_triangle(layer, &tri);

  // Centre dot
  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.radius = LV_RADIUS_CIRCLE;
  dot.bg_color = lv_color_hex(C_POINTER);
  dot.bg_opa = live;
  lv_area_t ca = { cx - 7, cy - 7, cx + 7, cy + 7 };
  lv_draw_rect(layer, &dot, &ca);
}

void create(lv_obj_t *content) {
  s_ui = Ui();
  s_ui.heading = label(content, &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_align(s_ui.heading, LV_ALIGN_TOP_MID, 0, 0);
  lv_label_set_text(s_ui.heading, "--");
  s_ui.hint = label(content, &lv_font_montserrat_16, 0xDDDDDD);
  lv_obj_set_width(s_ui.hint, 456);
  lv_obj_set_style_text_align(s_ui.hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_ui.hint, LV_ALIGN_TOP_MID, 0, 62);

  s_ui.dial = lv_obj_create(content);
  lv_obj_remove_style_all(s_ui.dial);
  lv_obj_set_size(s_ui.dial, DIAL, DIAL);
  lv_obj_set_pos(s_ui.dial, 12, 92);
  lv_obj_add_event_cb(s_ui.dial, onDraw, LV_EVENT_DRAW_MAIN, nullptr);

  s_ui.speed = label(content, &lv_font_montserrat_48, 0xFFFFFF);
  lv_obj_align(s_ui.speed, LV_ALIGN_TOP_MID, -40, 568);
  lv_label_set_text(s_ui.speed, "--");
  s_ui.unit = label(content, &lv_font_montserrat_20, 0xDDDDDD);
  lv_label_set_text(s_ui.unit, "km/h");
  s_ui.alt = label(content, &lv_font_montserrat_20, 0xDDDDDD);
  lv_obj_align(s_ui.alt, LV_ALIGN_TOP_LEFT, 24, 640);
  s_ui.sats = label(content, &lv_font_montserrat_20, 0xDDDDDD);
  lv_obj_align(s_ui.sats, LV_ALIGN_TOP_RIGHT, -24, 640);
  s_moving = false;
}

void update() {
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix;
  const float kmh = (d.speedValid && fix) ? (float)d.speedKmh : -1.0f;
  const bool wasMoving = s_moving;
  const bool course = fix && d.courseValid && kmh >= 0;
  s_moving = course && kmh >= (wasMoving ? HOLD_KMH : START_KMH);

  bool redraw = s_moving != wasMoving;
  if (s_moving) {
    float diff = (float)d.courseDeg - s_heading;
    while (diff > 180) diff -= 360;
    while (diff < -180) diff += 360;
    if (!s_seen) { s_heading = (float)d.courseDeg; s_seen = true; redraw = true; }
    else if (fabsf(diff) >= 1.0f) { s_heading += diff * 0.6f; redraw = true; }
    while (s_heading < 0) s_heading += 360;
    while (s_heading >= 360) s_heading -= 360;
  }

  char b[48];
  if (s_seen) snprintf(b, sizeof(b), "%s  %03.0f\xC2\xB0", cardinal(s_heading), s_heading);
  else snprintf(b, sizeof(b), "--");
  Apps::setText(s_ui.heading, b);
  lv_obj_set_style_text_opa(s_ui.heading, s_moving ? LV_OPA_COVER : LV_OPA_50, 0);
  Apps::setText(s_ui.hint, s_moving ? "Heading from GPS movement"
                          : !fix    ? "Waiting for a GPS fix"
                                    : "Not moving - the heading comes from GPS movement");
  if (redraw) lv_obj_invalidate(s_ui.dial);

  if (kmh >= 0) snprintf(b, sizeof(b), "%.0f", kmh);
  else snprintf(b, sizeof(b), "--");
  Apps::setText(s_ui.speed, b);
  lv_obj_align_to(s_ui.unit, s_ui.speed, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -8);
  if (d.altValid && d.locValid && fix) snprintf(b, sizeof(b), "Altitude %.0f m", d.altM);
  else snprintf(b, sizeof(b), "Altitude --");
  Apps::setText(s_ui.alt, b);
  snprintf(b, sizeof(b), "%u satellites", (unsigned)d.satsUsed);
  Apps::setText(s_ui.sats, b);
}

void destroy() { s_ui = Ui(); }

}  // namespace

extern const AppImpl COMPASS_APP = { "Compass", create, update, destroy };
