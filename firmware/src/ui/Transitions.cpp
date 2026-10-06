#include "Transitions.h"
#include "../Display.h"

namespace {

constexpr int32_t SCREEN_W = Display::UI_W, SCREEN_H = Display::UI_H;

struct Zoom {
  lv_obj_t *card = nullptr;
  lv_area_t from = {};
  Transitions::DoneCb done = nullptr;
  void *user = nullptr;
};
Zoom s_zoom;

int32_t lerp(int32_t a, int32_t b, int32_t t) { return a + (b - a) * t / 1024; }   // t: 0..1024

void zoomExec(void *, int32_t t) {
  const lv_area_t &f = s_zoom.from;
  const int32_t x1 = lerp(f.x1, 0, t), y1 = lerp(f.y1, 0, t);
  const int32_t x2 = lerp(f.x2, SCREEN_W - 1, t), y2 = lerp(f.y2, SCREEN_H - 1, t);
  // Order matters: LVGL applies a pending size before a pending position, which briefly
  // puts the NEW size at the OLD position - a rectangle sticking out of the card, so LVGL
  // redraws everything beneath it. Settle the new position first; every intermediate
  // rectangle then lies inside the (growing) card and nothing beneath is drawn.
  lv_obj_set_pos(s_zoom.card, x1, y1);
  lv_obj_update_layout(s_zoom.card);
  lv_obj_set_size(s_zoom.card, x2 - x1 + 1, y2 - y1 + 1);
}

void zoomDone(lv_anim_t *) {
  lv_obj_t *card = s_zoom.card;
  Transitions::DoneCb done = s_zoom.done;
  void *user = s_zoom.user;
  s_zoom = Zoom();
  if (done) done(user);               // normally loads the app screen (same colour)
  lv_obj_delete(card);                // card was on the previous screen: no visible change
}

void pulseExec(void *obj, int32_t v) {
  // v: 0..200 -> outline 0..6..0 px (triangle)
  const int32_t w = (100 - LV_ABS(v - 100)) * 6 / 100;
  lv_obj_set_style_outline_width((lv_obj_t *)obj, w, 0);
}

}  // namespace

void Transitions::zoomOpen(lv_obj_t *screen, const lv_area_t &from, lv_color_t color,
                           uint32_t durationMs, DoneCb done, void *user) {
  if (s_zoom.card) return;            // one transition at a time
  s_zoom.from = from;
  s_zoom.done = done;
  s_zoom.user = user;

  lv_obj_t *card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_style_bg_color(card, color, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);   // opaque + radius 0 -> LVGL skips what's beneath
  lv_obj_set_scrollable(card, false);                 // clickable: swallows taps during the zoom
  s_zoom.card = card;
  zoomExec(nullptr, 0);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, card);
  lv_anim_set_exec_cb(&a, zoomExec);
  lv_anim_set_values(&a, 0, 1024);
  lv_anim_set_duration(&a, durationMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_completed_cb(&a, zoomDone);
  lv_anim_start(&a);
}

bool Transitions::zoomRunning() { return s_zoom.card != nullptr; }

void Transitions::pulse(lv_obj_t *obj, lv_color_t color, uint32_t durationMs) {
  if (!obj) return;
  lv_obj_set_style_outline_color(obj, color, 0);
  lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_pad(obj, 2, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, pulseExec);
  lv_anim_set_values(&a, 0, 200);
  lv_anim_set_duration(&a, durationMs);
  lv_anim_start(&a);
}
