#include "NavUi.h"

#include <Arduino.h>
#include <math.h>
#include "../Display.h"
#include "../services/Location.h"
#include "UiText.h"

namespace {

constexpr float START_KMH = 3.0f, HOLD_KMH = 1.5f;      // as the Compass app
bool s_moving = false;
float s_heading = 0;

struct ArrowData { float angle; uint32_t color; bool live; };

void onDrawArrow(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target_obj(e);
  const ArrowData *a = (const ArrowData *)lv_obj_get_user_data(o);
  if (!a) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t c;
  lv_obj_get_coords(o, &c);
  const float cx = (c.x1 + c.x2) / 2.0f, cy = (c.y1 + c.y2) / 2.0f, r = (c.x2 - c.x1) / 2.0f;
  const float th = a->angle * (float)M_PI / 180.0f, cs = cosf(th), sn = sinf(th);
  // arrow in unit coordinates (up = -y): a head and a shaft, so the direction reads at a glance
  const float P[7][2] = { { 0, -0.95f }, { 0.55f, -0.15f }, { -0.55f, -0.15f },        // head
                          { 0.2f, -0.2f }, { -0.2f, -0.2f }, { 0.2f, 0.9f }, { -0.2f, 0.9f } };   // shaft
  auto pt = [&](int i, lv_point_precise_t &p) {
    const float x = P[i][0] * r, y = P[i][1] * r;
    p.x = (lv_value_precise_t)(cx + x * cs - y * sn);
    p.y = (lv_value_precise_t)(cy + x * sn + y * cs);
  };
  lv_draw_triangle_dsc_t td;
  lv_draw_triangle_dsc_init(&td);
  td.color = lv_color_hex(a->color);
  td.opa = a->live ? LV_OPA_COVER : LV_OPA_40;
  static const uint8_t T[3][3] = { { 0, 1, 2 }, { 3, 4, 5 }, { 4, 6, 5 } };
  for (const auto &t : T) {
    pt(t[0], td.p[0]); pt(t[1], td.p[1]); pt(t[2], td.p[2]);
    lv_draw_triangle(layer, &td);
  }
}

void onDeleteArrow(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target_obj(e);
  free(lv_obj_get_user_data(o));
  lv_obj_set_user_data(o, nullptr);
}

// ---- keyboard dialog ----
lv_obj_t *s_kbOverlay = nullptr, *s_kbTa = nullptr;
void (*s_kbDone)(const char *) = nullptr;

void onKb(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
  void (*done)(const char *) = s_kbDone;
  char text[64];
  strlcpy(text, s_kbTa ? lv_textarea_get_text(s_kbTa) : "", sizeof(text));
  NavUi::keyboardClose();
  if (done) done(code == LV_EVENT_READY ? text : nullptr);
}

}  // namespace

lv_obj_t *NavUi::arrow(lv_obj_t *parent, int size, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, size, size);
  lv_obj_set_clickable(o, false);
  ArrowData *a = (ArrowData *)malloc(sizeof(ArrowData));
  if (a) { a->angle = 0; a->color = color; a->live = true; }
  lv_obj_set_user_data(o, a);
  lv_obj_add_event_cb(o, onDrawArrow, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(o, onDeleteArrow, LV_EVENT_DELETE, nullptr);
  return o;
}

void NavUi::setArrow(lv_obj_t *o, float angleDeg, bool live) {
  ArrowData *a = o ? (ArrowData *)lv_obj_get_user_data(o) : nullptr;
  if (!a) return;
  while (angleDeg < 0) angleDeg += 360;
  while (angleDeg >= 360) angleDeg -= 360;
  if (fabsf(a->angle - angleDeg) < 1.0f && a->live == live) return;
  a->angle = angleDeg;
  a->live = live;
  lv_obj_invalidate(o);
}

bool NavUi::heading(float &deg) {
  const GpsData d = Location::snapshot();
  const bool fix = Location::quality(d) == Location::Quality::Fix;
  const float kmh = d.speedValid && fix ? (float)d.speedKmh : -1.0f;
  s_moving = fix && d.courseValid && kmh >= (s_moving ? HOLD_KMH : START_KMH);
  if (s_moving) s_heading = (float)d.courseDeg;
  deg = s_heading;
  return s_moving;
}

bool NavUi::relative(float bearingDeg, float &angleDeg) {
  float h;
  const bool moving = heading(h);
  angleDeg = moving ? bearingDeg - h : bearingDeg;
  return moving;
}

void NavUi::formatEta(uint32_t s, char *out, size_t n) {
  if (!s) { snprintf(out, n, "--"); return; }
  const uint32_t m = (s + 30) / 60;
  if (m < 60) snprintf(out, n, "%lu min", (unsigned long)max<uint32_t>(1, m));
  else snprintf(out, n, "%lu h %02lu", (unsigned long)(m / 60), (unsigned long)(m % 60));
}

void NavUi::keyboard(lv_obj_t *anyChild, const char *title, const char *initial, int maxLen, void (*done)(const char *)) {
  keyboardClose();
  lv_obj_t *ov = lv_obj_create(lv_obj_get_screen(anyChild));
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, Display::UI_W, Display::UI_H);
  lv_obj_set_style_bg_color(ov, lv_color_hex(0x10141C), 0);
  lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
  lv_obj_set_clickable(ov, true);                   // swallows taps
  lv_obj_t *t = lv_label_create(ov);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_width(t, Display::UI_W - 48);
  lv_label_set_text_fmt(t, "%s\n" LV_SYMBOL_OK " save   " LV_SYMBOL_KEYBOARD " cancel", title);
  lv_obj_set_pos(t, 24, 60);
  lv_obj_t *ta = lv_textarea_create(ov);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, maxLen);
  lv_obj_set_size(ta, Display::UI_W - 48, 56);
  lv_obj_set_pos(ta, 24, 136);
  lv_textarea_set_text(ta, initial && UiText::isAscii(initial) ? initial : "");
  lv_obj_t *kb = lv_keyboard_create(ov);
  lv_obj_set_size(kb, Display::UI_W, 320);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_add_event_cb(kb, onKb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, onKb, LV_EVENT_CANCEL, nullptr);
  s_kbOverlay = ov;
  s_kbTa = ta;
  s_kbDone = done;
}

bool NavUi::keyboardOpen() { return s_kbOverlay != nullptr; }

void NavUi::keyboardClose() {
  if (s_kbOverlay) lv_obj_delete_async(s_kbOverlay);
  s_kbOverlay = s_kbTa = nullptr;
  s_kbDone = nullptr;
}
